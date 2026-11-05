/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq_test.ko - synthetic kwq clients driven by sysctl (../PLAN.txt P3;
 * the P0 and P1b subsets).
 *
 *   sysctl kern.kwq_test.scenario=lifecycle   # P0: lifecycle fifo notify
 *                                             #     reject discard sleep
 *                                             # P1b: fairness latency gaming
 *                                             #     overrun yield cost
 *                                             #     tq_baseline switch_baseline
 *                                             #     pingpong pingpong_tq clocks
 *                                             # P4 shape: ifpair
 *                                             # P4e: burst (SPEC-p4e 2.1, 2.4)
 *                                             # P6: steal scatter stealdrain
 *                                             #     stealdrain_discard (BULK)
 *                                             # P6g: anyburst starved
 *   sysctl kern.kwq_test.intr_cpu=1 kern.kwq_test.intr_us=100 \
 *          kern.kwq_test.intr_work_ns=20000 kern.kwq_test.intr_secs=10
 *                                             # interrupt load on CPU 1 beside
 *                                             # any scenario (SPEC-p4e 2.2)
 *   sysctl kern.kwq_test.items=1000 kern.kwq_test.reps=1000
 *   sysctl kern.kwq_test.run=1
 *   sysctl kern.kwq_test                      # poll result_state until
 *                                             # "done" or "fail"
 *
 * Time is kt_ticks() = get_cyclecount() at a rate calibrated at load, never
 * cpu_ticks(): see the comment at kt_ticks().
 *
 * A scenario runs in its own kernel thread; results land in the
 * kern.kwq_test.result_* sysctls.  The P1b scenarios leave their queues
 * alive (kern.kwq.net.queue.kta and ktb) until the next run or unload,
 * so kwq's own counters can be read afterwards.  The "sleep" scenario
 * deliberately sleeps inside a handler and is expected to panic an
 * INVARIANTS kernel; it refuses to run unless kern.kwq_test.allow_panic=1.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/kthread.h>
#include <sys/unistd.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/time.h>
#include <sys/sched.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>
#include <sys/cpuset.h>
#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/stdarg.h>

#include "kwq.h"

MALLOC_DEFINE(M_KWQ_TEST, "kwq_test", "kwq test items");

#define	TITEM_MAGIC	0x6b77712d74657374ULL	/* "kwq-test" */

/*
 * One line per item.  A real item (an mbuf, a request) is at least a
 * line; 40-byte items packed three to a line made the producer and the
 * worker write the same lines at different times, which is not a cost
 * kwq's clients pay (a07 PMC run, SCHED.md S15.7).
 */
#define	KT_LINE	128	/* KWQ_LINE: 128 covers amd64's prefetch pair and arm64 */

struct titem {
	struct kwq_item	ti_item;
	uint64_t	ti_magic;
	uint64_t	ti_seq;		/* per-CPU sequence, or an sbinuptime stamp */
	int		ti_cpu;
	int		ti_qidx;	/* flood queue index (P1b) */
	u_int		ti_seen;	/* P0: rep tag of the delivery, catches an item run twice */
	struct kt_sgroup *ti_grp;	/* scatter: the call the item belongs to */
} __aligned(KT_LINE);

/* Knobs. */
static char	kt_scenario[32] = "lifecycle";
static u_int	kt_items = 1000;
static u_int	kt_reps = 1000;
static u_int	kt_cost_us = 0;
static u_int	kt_limit = 0;
static int	kt_cpu = -1;		/* -1: round robin (P0) / CPU 1 (P1b) */
static int	kt_allow_panic = 0;
static u_int	kt_secs = 3;
static u_int	kt_weight_a = 1, kt_weight_b = 1;
static u_int	kt_cost_a = 50, kt_cost_b = 500;	/* us per item */
static u_int	kt_batch = 1;		/* items per kwq_enqueue_list() in the flood producer */
static u_int	kt_pairs = 1;		/* scale: producer/consumer pairs on disjoint CPUs */
static u_int	kt_fanin = 1;		/* fanin: producers on their own CPUs into one (queue, CPU) */
static char	kt_qname[KWQ_NAMELEN] = "test";	/* queue name for the P0 scenarios */
#define	KT_MAX_FANIN	64
/* ifpair: simulated interfaces, worker CPUs per interface, spin per packet */
static u_int	kt_ifaces = 1;
static u_int	kt_spread = 1;
static u_int	kt_prod_ns = 1000;	/* sender's stack work per packet before the enqueue */
static u_int	kt_cons_ns = 1500;	/* receiver's stack work per packet in the handler */
static u_int	kt_nnf = 64;		/* mix: notifiers in use (<= 64) */
static u_int	kt_prod_gap_us = 0;	/* ifpair: a sender's pause between packets; 0 = flat out */
/* burst (SPEC-p4e 2.1, 2.4) */
static u_int	kt_burst_items = 64;	/* mode 0: items per burst */
static u_int	kt_burst_gap_us = 500;	/* mode 0: the gap after a burst; 1, 2: the spacing (mean) */
static u_int	kt_burst_mode = 0;	/* 0 on/off bursts, 1 constant spacing, 2 exponential spacing */

/* Results. */
static char	kt_state[16] = "idle";
static char	kt_msg[256] = "";
static char	kt_clocks[256] = "";	/* clocks: ns per call of each clock */
static uint64_t	kt_r_in, kt_r_out, kt_r_discarded, kt_r_rejected, kt_r_coalesced,
		kt_r_runs, kt_r_ns, kt_r_fifo_errors;
static uint64_t	kt_r_glitches;	/* kt_ticks() deltas discarded, see kt_tickdelta() */
static uint64_t	kt_r_dups;	/* P0: items run or discarded twice in a rep */
static uint64_t	kt_r_cycles_a, kt_r_cycles_b, kt_r_maxlat_a_us, kt_r_maxlat_b_us,
		kt_r_ns_per_item, kt_r_prod_ns_per_item, kt_r_lat_ns;
/* ifpair */
static uint64_t	kt_r_enq_slow;		/* enqueues over 2 us */
static uint64_t	kt_r_enq_pct;		/* share of producer CPU time inside kwq_enqueue */
static uint64_t	kt_r_cons_pct;		/* share of consumer CPU time in packet work */
static uint64_t	kt_r_pool_waits;	/* producers found their window empty */
static uint64_t	kt_r_inversions;	/* mix: notifier/item deliveries out of arrival order */
static uint64_t	kt_r_exact_errors;	/* mix: notifiers delivered other than exactly once */
static uint64_t	kt_r_calls;		/* mix: handler invocations */
static uint64_t	kt_r_requeues;		/* mix: kwq_requeue() calls by the handler */
static uint64_t	kt_r_patterns;		/* mix: patterns checked */
static uint64_t	kt_r_with, kt_r_without;	/* mixstarve: items with/without the notifier */
static uint64_t	kt_r_wcpu;		/* mix: the worker CPU */
static uint64_t	kt_r_cpus;		/* P6: CPUs that ran items (steal; scatter: most in one call) */
static uint64_t	kt_r_offcpu;		/* P6 steal: items run on a CPU other than the target */
/* burst: a burst's first item served after a gap, and its last item, age at service */
static uint64_t	kt_r_bursts;
static uint64_t	kt_r_first_p50_us, kt_r_first_p99_us, kt_r_first_max_us;
static uint64_t	kt_r_last_p50_us, kt_r_last_p99_us, kt_r_last_max_us;
/* intr_load (independent of the run) */
static uint64_t	kt_r_intr_fired, kt_r_intr_late, kt_r_intr_work_ns;

/* Per-run state shared with handlers. */
static struct mtx kt_mtx;
static bool	kt_running;
static uint64_t	kt_handled;		/* items run */
static uint64_t	kt_discarded;		/* items handed with n < 0 */
static uint64_t	kt_nf_runs;		/* notifier handler runs */
static uint64_t	kt_nf_seen;		/* last state value the handler saw */
static uint64_t	kt_nf_state;		/* the "hardware state" the producer bumps */
static uint64_t	kt_fifo_errors;
static uint64_t	kt_dups;		/* P0: items delivered twice in one rep */
static u_int	kt_rep_tag;		/* P0: current rep + 1, stamped into ti_seen */
static int	kt_pcpu = -1;		/* P0: the producer's CPU, never a target */
static int	kt_pcpu_knob = -1;	/* producer CPU for the two-CPU scenarios; -1: the next CPU */
static uint64_t	*kt_last_seq;		/* [mp_maxid + 1] */
static enum kwq_class kt_class = KWQ_NET;	/* class kt_create() uses (P6: BULK) */
static bool	kt_nofifo;		/* P6: items may run off their CPU (stealing) */
static bool	kt_coop;		/* P6: the item handler honours its budget and
					   requeues the rest, as the contract asks */
static uint64_t	*kt_cpu_hits;		/* P6: [mp_maxid + 1] items run per CPU */

SYSCTL_NODE(_kern, OID_AUTO, kwq_test, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "kwq test harness");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, scenario, CTLFLAG_RW, kt_scenario,
    sizeof(kt_scenario), "lifecycle fifo notify reject discard sleep | "
    "fairness latency gaming overrun yield cost scale fanin tq_baseline switch_baseline pingpong pingpong_tq clocks burst | ifpair | "
    "mixorder mixpattern mixbudget mixstarve mixrearm mixcancel mixrequeue mixdrain mixdrain_discard | "
    "steal scatter stealdrain stealdrain_discard | anyburst starved");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, items, CTLFLAG_RW, &kt_items, 0,
    "items per repetition (P0), pool size per flood (P1b)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, reps, CTLFLAG_RW, &kt_reps, 0,
    "repetitions (create/.../destroy cycles)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, cost_us, CTLFLAG_RW, &kt_cost_us, 0,
    "busy time per item in the handler (P0 scenarios)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, limit, CTLFLAG_RW, &kt_limit, 0,
    "queue limit (0 = class default)");
SYSCTL_INT(_kern_kwq_test, OID_AUTO, cpu, CTLFLAG_RW, &kt_cpu, 0,
    "target CPU (-1 = round robin for P0, CPU 1 for P1b)");
SYSCTL_INT(_kern_kwq_test, OID_AUTO, pcpu, CTLFLAG_RW, &kt_pcpu_knob, 0,
    "producer CPU of the two-CPU scenarios (-1 = the CPU after the target; on an SMT machine that is its sibling; may equal the target: the producer then shares the worker's CPU)");
/*
 * The flood, pingpong and taskqueue producers run at the priority they
 * inherit from kthread_add(), a kernel one (41 here), which is better than
 * kwq's yield_prio (56): a same-CPU run with them measures the round-end
 * yield, never the wake's preemption.  prod_prio puts them at a
 * user-range priority instead (a network client's producer is a user
 * thread in sendmsg(2) at its own priority), so that the idle-class wait
 * (wake_delay_us, ../KWQ.md S16) can be exercised on one CPU.
 */
static int	kt_prod_prio = -1;
SYSCTL_INT(_kern_kwq_test, OID_AUTO, prod_prio, CTLFLAG_RW, &kt_prod_prio, 0,
    "priority of the producer threads (-1 = as created, a kernel priority; 56..223 = a user-range priority)");

static void
kt_prod_bind(int cpu)
{
	thread_lock(curthread);
	sched_bind(curthread, cpu);
	if (kt_prod_prio >= PRI_MIN_TIMESHARE && kt_prod_prio <= PRI_MAX_TIMESHARE)
		sched_prio(curthread, kt_prod_prio);
	thread_unlock(curthread);
}
SYSCTL_INT(_kern_kwq_test, OID_AUTO, allow_panic, CTLFLAG_RW, &kt_allow_panic,
    0, "allow scenarios that are expected to panic");
static u_int	kt_spin_ms = 100;	/* P6g starved: the spinner's time */
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, spin_ms, CTLFLAG_RW, &kt_spin_ms, 0,
    "starved: how long a kernel thread at PI_NET spins on the target CPU, ms");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, secs, CTLFLAG_RW, &kt_secs, 0,
    "duration of a P1b scenario");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, weight_a, CTLFLAG_RW, &kt_weight_a, 0, "");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, weight_b, CTLFLAG_RW, &kt_weight_b, 0, "");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, cost_a, CTLFLAG_RW, &kt_cost_a, 0,
    "us per item, queue a");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, cost_b, CTLFLAG_RW, &kt_cost_b, 0,
    "us per item, queue b");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, batch, CTLFLAG_RW, &kt_batch, 0,
    "flood producer: items per kwq_enqueue_list() call (1 = kwq_enqueue per item)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, pairs, CTLFLAG_RW, &kt_pairs, 0,
    "scale scenario: producer/consumer pairs, each on two CPUs of its own");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, fanin, CTLFLAG_RW, &kt_fanin, 0,
    "fanin scenario: producers, each on its own CPU, feeding one (queue, CPU) list; "
    "ifpair: senders per interface");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, ifaces, CTLFLAG_RW, &kt_ifaces, 0,
    "ifpair scenario: simulated interfaces (one queue each)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, spread, CTLFLAG_RW, &kt_spread, 0,
    "ifpair scenario: worker CPUs each interface's flows are hashed over");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, prod_ns, CTLFLAG_RW, &kt_prod_ns, 0,
    "ifpair scenario: sender-side busy work per packet before the enqueue, ns");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, cons_ns, CTLFLAG_RW, &kt_cons_ns, 0,
    "ifpair scenario: receiver-side busy work per packet in the handler, ns");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, prod_gap_us, CTLFLAG_RW, &kt_prod_gap_us, 0,
    "ifpair: microseconds each sender pauses between packets (0 = flat out; SPEC-p4e 2.3)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, burst_items, CTLFLAG_RW, &kt_burst_items, 0,
    "burst: items per burst (mode 0)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, burst_gap_us, CTLFLAG_RW, &kt_burst_gap_us, 0,
    "burst: the gap after a burst (mode 0), or the item spacing (mode 1) or its mean (mode 2), in us");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, burst_mode, CTLFLAG_RW, &kt_burst_mode, 0,
    "burst: 0 = on/off bursts of burst_items then burst_gap_us; 1 = one item every burst_gap_us; 2 = exponential spacing with mean burst_gap_us");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, nnf, CTLFLAG_RW, &kt_nnf, 0,
    "mix: notifiers in use, 1..64");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, qname, CTLFLAG_RW, kt_qname, sizeof(kt_qname),
    "queue name used by the P0 scenarios (invalid names such as a.b or a/b must be refused)");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, result_state, CTLFLAG_RD, kt_state,
    sizeof(kt_state), "idle | running | done | fail");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, result_msg, CTLFLAG_RD, kt_msg,
    sizeof(kt_msg), "failure description");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_in, CTLFLAG_RD, &kt_r_in, 0,
    "items/signals submitted");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_out, CTLFLAG_RD, &kt_r_out, 0,
    "items run by the handler");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_discarded, CTLFLAG_RD,
    &kt_r_discarded, 0, "items handed to the handler with n < 0");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_rejected, CTLFLAG_RD,
    &kt_r_rejected, 0, "enqueues refused");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_coalesced, CTLFLAG_RD,
    &kt_r_coalesced, 0, "kwq_notify() calls that returned false");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_runs, CTLFLAG_RD, &kt_r_runs, 0,
    "notifier handler runs / callout fires");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_fifo_errors, CTLFLAG_RD,
    &kt_r_fifo_errors, 0, "out-of-order deliveries seen");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_dups, CTLFLAG_RD, &kt_r_dups, 0,
    "P0: items delivered twice within a rep (must be 0)");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_glitches, CTLFLAG_RD, &kt_r_glitches, 0,
    "kt_ticks() deltas over 40 ms discarded from the cost sums (ticker glitches)");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_ns, CTLFLAG_RD, &kt_r_ns, 0,
    "wall time of the run");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_cycles_a, CTLFLAG_RD,
    &kt_r_cycles_a, 0, "handler cpu_ticks in queue a");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_cycles_b, CTLFLAG_RD,
    &kt_r_cycles_b, 0, "handler cpu_ticks in queue b");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_maxlat_a_us, CTLFLAG_RD,
    &kt_r_maxlat_a_us, 0, "max enqueue-to-run latency, queue a");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_maxlat_b_us, CTLFLAG_RD,
    &kt_r_maxlat_b_us, 0, "max enqueue-to-run latency, queue b");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_ns_per_item, CTLFLAG_RD,
    &kt_r_ns_per_item, 0, "wall ns per item (cost, tq_baseline) or per switch");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_prod_ns_per_item, CTLFLAG_RD,
    &kt_r_prod_ns_per_item, 0, "producer cpu ns per enqueue (cost, tq_baseline, ifpair)");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, result_clocks, CTLFLAG_RD, kt_clocks,
    sizeof(kt_clocks), "clocks: ns per call of each clock read, and of kwq_budget_left()");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_lat_ns, CTLFLAG_RD,
    &kt_r_lat_ns, 0, "pingpong: average enqueue-to-handler ns");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_enq_slow, CTLFLAG_RD,
    &kt_r_enq_slow, 0, "ifpair: enqueue calls that took over 2 us");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_enq_pct, CTLFLAG_RD,
    &kt_r_enq_pct, 0, "ifpair: percent of producer CPU time spent inside kwq_enqueue");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_cons_pct, CTLFLAG_RD,
    &kt_r_cons_pct, 0, "ifpair: percent of the worker CPUs' wall time spent in packet work");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_pool_waits, CTLFLAG_RD,
    &kt_r_pool_waits, 0, "ifpair: times a sender found its whole window in flight");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_inversions, CTLFLAG_RD,
    &kt_r_inversions, 0, "mix: notifier/item deliveries out of arrival order");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_exact_errors, CTLFLAG_RD,
    &kt_r_exact_errors, 0, "mix: notifiers delivered other than exactly once");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_calls, CTLFLAG_RD,
    &kt_r_calls, 0, "mix: handler invocations");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_requeues, CTLFLAG_RD,
    &kt_r_requeues, 0, "mix: kwq_requeue() calls by the handler");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_patterns, CTLFLAG_RD,
    &kt_r_patterns, 0, "mixpattern: patterns checked");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_with, CTLFLAG_RD,
    &kt_r_with, 0, "mixstarve: items delivered beside the re-arming notifier");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_without, CTLFLAG_RD,
    &kt_r_without, 0, "mixstarve: items delivered alone");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_bursts, CTLFLAG_RD, &kt_r_bursts, 0,
    "burst: bursts produced");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_first_p50_us, CTLFLAG_RD, &kt_r_first_p50_us, 0,
    "burst: age of a burst's first item at service, median, us (bucket upper bound)");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_first_p99_us, CTLFLAG_RD, &kt_r_first_p99_us, 0,
    "burst: the same, 99th percentile");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_first_max_us, CTLFLAG_RD, &kt_r_first_max_us, 0,
    "burst: the same, maximum (exact)");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_last_p50_us, CTLFLAG_RD, &kt_r_last_p50_us, 0,
    "burst: age of a burst's last item at service, median, us");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_last_p99_us, CTLFLAG_RD, &kt_r_last_p99_us, 0,
    "burst: the same, 99th percentile");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_last_max_us, CTLFLAG_RD, &kt_r_last_max_us, 0,
    "burst: the same, maximum (exact)");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_intr_fired, CTLFLAG_RD, &kt_r_intr_fired, 0,
    "intr_load: handler runs since the last start");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_intr_late, CTLFLAG_RD, &kt_r_intr_late, 0,
    "intr_load: runs that began more than one period after their deadline");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_intr_work_ns, CTLFLAG_RD, &kt_r_intr_work_ns, 0,
    "intr_load: busy time spent in interrupt context, ns");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_wcpu, CTLFLAG_RD,
    &kt_r_wcpu, 0, "mix: the worker CPU used; steal: the target CPU");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_cpus, CTLFLAG_RD,
    &kt_r_cpus, 0, "steal: CPUs that ran items; scatter: the most CPUs one call ran on");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_offcpu, CTLFLAG_RD,
    &kt_r_offcpu, 0, "steal: items run on a CPU other than the one they were enqueued to");

static void
kt_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(kt_msg, sizeof(kt_msg), fmt, ap);
	va_end(ap);
	strlcpy(kt_state, "fail", sizeof(kt_state));
	printf("kwq_test: FAIL: %s\n", kt_msg);
}

static uint64_t
kt_sbt2us(sbintime_t d)
{
	return (((uint64_t)d * 1000000ULL) >> 32);
}

static inline uint64_t
kt_sbt2ns(sbintime_t d)
{
	return ((uint64_t)sbttons(d));
}

/*
 * The test's own clock: get_cyclecount(), 64-bit and unfenced, at a rate
 * calibrated against sbinuptime() when the module loads.  Not kt_ticks():
 * on arm64 that is the kernel's 32-bit extension, which takes a decrease
 * for a wrap, and statclock() reads it from the clock interrupt, so a
 * thread reading it with interrupts enabled can see +2^32 ticks (171.8 s)
 * added to its CPU's ticker for good (tools/clockprobe, 2026-10-20).  This
 * module read it per item and per enqueue and was a steady source of those
 * jumps on a07.  The glitch filter stays: anything over 40 ms in one
 * handler item or one enqueue is a glitch, counted and left out.
 */
static uint64_t kt_tick_hz;		/* get_cyclecount() per second */

static inline uint64_t
kt_ticks(void)
{
	return (get_cyclecount());
}

static void
kt_calibrate_ticks(void)
{
	sbintime_t s0 = sbinuptime(), s1;
	uint64_t c0 = get_cyclecount(), c1;

	do { s1 = sbinuptime(); c1 = get_cyclecount(); } while (s1 - s0 < SBT_1MS * 100);
	kt_tick_hz = (c1 - c0) * 1000000000ULL / ((((uint64_t)(s1 - s0)) * 1000000000ULL) >> 32);
}

static inline uint64_t
kt_tickdelta(uint64_t t0)
{
	uint64_t d = kt_ticks() - t0;

	if (d > kt_tick_hz / 25) {
		kt_r_glitches++;
		return (0);
	}
	return (d);
}

/*
 * Busy work standing in for the network stack: spin for `ticks' of
 * kt_ticks().  A glitch (SCHED.md S13, kt_tickdelta above) can only end
 * the spin early.  The loop reads the clock, which on arm64 is a
 * serialising instruction; that is the only memory traffic real stack
 * work would have and this does not, so the model is optimistic about
 * cache pressure and pessimistic about nothing.
 */
static inline void
kt_spin_ticks(uint64_t ticks)
{
	uint64_t t0 = kt_ticks();

	while (kt_ticks() - t0 < ticks)
		cpu_spinwait();
}

static inline uint64_t
kt_ns2ticks(uint64_t ns)
{
	return (ns * kt_tick_hz / 1000000000ULL);
}

static int
kt_other_cpu(int cpu)
{
	int c = cpu;

	/*
	 * An explicit pcpu may equal the target CPU: the producer then shares
	 * the worker's CPU, the case the idle-class wait (wake_delay_us,
	 * ../KWQ.md S16) exists for.  The default stays the next CPU.
	 */
	if (kt_pcpu_knob >= 0 && kt_pcpu_knob <= mp_maxid &&
	    !CPU_ABSENT(kt_pcpu_knob))
		return (kt_pcpu_knob);
	do {
		c = (c + 1) % (mp_maxid + 1);
	} while (CPU_ABSENT(c) && c != cpu);
	return (c);
}

/* ================================================================ P0 */

static void
kt_item_handler(struct kwq *q, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;
	struct titem *ti;
	int cpu;
	u_int cnt;

	cpu = curcpu;
	cnt = 0;
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		KWQ_ITEM_INIT(it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC)
			panic("kwq_test: bad item %p", ti);
		/* Run or discarded, each item exactly once per rep. */
		if (ti->ti_seen == kt_rep_tag)
			atomic_add_64(&kt_dups, 1);
		ti->ti_seen = kt_rep_tag;
		if (n > 0) {
			if (!kt_nofifo && (ti->ti_cpu != cpu ||
			    ti->ti_seq <= kt_last_seq[cpu]))
				atomic_add_64(&kt_fifo_errors, 1);
			kt_last_seq[cpu] = ti->ti_seq;
			if (kt_cost_us != 0)
				DELAY(kt_cost_us);
			if (kt_cpu_hits != NULL)
				atomic_add_64(&kt_cpu_hits[cpu], 1);
		}
		cnt++;
		/*
		 * Only the live list can be stolen from, not a pass's batch:
		 * a handler that ignores its budget keeps a whole backlog out
		 * of reach (../SCHED.md S16).
		 */
		if (kt_coop && n > 0 && next != NULL && kwq_budget_left(q) == 0) {
			kwq_requeue(q, next, NULL, n - (int)cnt);
			atomic_add_64(&kt_handled, cnt);
			return;
		}
	}
	if (cnt != (u_int)(n < 0 ? -n : n))
		panic("kwq_test: handler count %u != n %d", cnt, n);
	if (n > 0)
		atomic_add_64(&kt_handled, cnt);
	else
		atomic_add_64(&kt_discarded, cnt);
}

static void
kt_sleep_handler(struct kwq *q, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		KWQ_ITEM_INIT(it);
	}
	if (n > 0)
		pause("kwqtst", 1);	/* must panic under INVARIANTS */
	atomic_add_64(&kt_handled, n < 0 ? -n : n);
}

static void
kt_notify_handler(struct kwq *q, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;

	/*
	 * Walk even though only the signal matters: fetching an element's
	 * successor is what consumes a notifier.  (The worker settles an
	 * unwalked head or tail itself, but that is a safety net, not the
	 * contract.)
	 */
	for (it = head; it != NULL; it = next)
		next = KWQ_ITEM_NEXT(q, it);
	if (n < 0)
		return;
	atomic_add_64(&kt_nf_runs, 1);
	/* Level semantics: read the state, not "one event". */
	atomic_store_64(&kt_nf_seen, atomic_load_64(&kt_nf_state));
}

static struct kwq *
kt_create(const char *name, uint32_t flags, u_int weight, kwq_handler_t *fn,
    void *ctx)
{
	struct kwq_params p;

	memset(&p, 0, sizeof(p));
	p.limit = kt_limit;
	p.weight = weight;
	p.domain = -1;
	return (kwq_create(name, kt_class, flags | KWQ_F_INACTIVE, &p, fn, ctx));
}

static int
kt_next_cpu(int *cur)
{
	int cpu;

	if (kt_cpu >= 0)
		return (kt_cpu);
	do {
		*cur = (*cur + 1) % (mp_maxid + 1);
		cpu = *cur;
	} while (CPU_ABSENT(cpu) || cpu == kt_pcpu);
	return (cpu);
}

/*
 * create/activate/enqueue/drain/destroy, kt_reps times.  The producer
 * (this thread) is bound to a CPU that is never a target: unbound, it
 * once ran on the worker's CPU on a07, was preempted by the worker per
 * item, and the reject and discard scenarios saw no rejects and two
 * discards where the guest saw thousands.  With the producer on its own
 * CPU the counts follow from the knobs alone: a list of `limit' items
 * drained at `cost_us' per item against a producer enqueuing in
 * nanoseconds must reject, and a drain with items still queued must
 * discard.
 */
static void
kt_run_items(uint32_t flags, kwq_handler_t *fn)
{
	struct kwq *q;
	struct titem *items, *ti;
	uint64_t in, rejected, *seq;
	u_int rep, i;
	int cpu, cur, error, pcpu;

	pcpu = kt_cpu >= 0 ? kt_other_cpu(kt_cpu) : (int)mp_maxid;
	while (CPU_ABSENT(pcpu) && pcpu > 0)
		pcpu--;
	kt_pcpu = pcpu;
	thread_lock(curthread);
	sched_bind(curthread, pcpu);
	thread_unlock(curthread);

	items = malloc(sizeof(*items) * kt_items, M_KWQ_TEST, M_WAITOK | M_ZERO);
	seq = malloc(sizeof(*seq) * (mp_maxid + 1), M_KWQ_TEST, M_WAITOK | M_ZERO);
	in = rejected = 0;
	cur = 0;
	kt_dups = 0;
	for (rep = 0; rep < kt_reps; rep++) {
		kt_rep_tag = rep + 1;
		q = kt_create(kt_qname, flags, 1, fn, NULL);
		if (q == NULL) {
			kt_fail("kwq_create failed at rep %u", rep);
			break;
		}
		memset(kt_last_seq, 0, sizeof(*kt_last_seq) * (mp_maxid + 1));
		memset(seq, 0, sizeof(*seq) * (mp_maxid + 1));
		/* Enqueue before activation must be refused. */
		ti = &items[0];
		ti->ti_magic = TITEM_MAGIC;
		KWQ_ITEM_INIT(&ti->ti_item);
		if (kwq_enqueue(q, 0, &ti->ti_item) != ENXIO) {
			kt_fail("enqueue before activate not refused");
			kwq_drain(q);
			kwq_destroy(q);
			break;
		}
		kwq_activate(q);
		for (i = 0; i < kt_items; i++) {
			ti = &items[i];
			cpu = kt_next_cpu(&cur);
			ti->ti_magic = TITEM_MAGIC;
			ti->ti_cpu = cpu;
			ti->ti_seq = ++seq[cpu];
			KWQ_ITEM_INIT(&ti->ti_item);
			error = kwq_enqueue(q, cpu, &ti->ti_item);
			in++;
			if (error != 0) {
				rejected++;
				seq[cpu]--;
				if (error != ENOBUFS) {
					kt_fail("enqueue error %d", error);
					break;
				}
			}
		}
		kwq_drain(q);
		kwq_destroy(q);
		if (kt_state[0] == 'f')
			break;
		if (kt_handled + kt_discarded + rejected != in) {
			kt_fail("rep %u: in %ju != run %ju + discarded %ju + "
			    "rejected %ju", rep, (uintmax_t)in,
			    (uintmax_t)kt_handled, (uintmax_t)kt_discarded,
			    (uintmax_t)rejected);
			break;
		}
	}
	kt_r_in = in;
	kt_r_out = kt_handled;
	kt_r_discarded = kt_discarded;
	kt_r_rejected = rejected;
	kt_r_fifo_errors = kt_fifo_errors;
	kt_r_dups = kt_dups;
	if (kt_fifo_errors != 0)
		kt_fail("%ju out-of-order deliveries", (uintmax_t)kt_fifo_errors);
	else if (kt_dups != 0)
		kt_fail("%ju items delivered twice", (uintmax_t)kt_dups);
	free(seq, M_KWQ_TEST);
	free(items, M_KWQ_TEST);
	thread_lock(curthread);
	sched_unbind(curthread);
	thread_unlock(curthread);
	kt_pcpu = -1;
}

static void
kt_run_notify(void)
{
	struct kwq *q;
	struct kwq_notifier nf;
	uint64_t signals, queued, coalesced;
	u_int i;
	int cpu;

	q = kt_create(kt_qname, 0, 1, kt_notify_handler, NULL);
	if (q == NULL) {
		kt_fail("kwq_create failed");
		return;
	}
	kwq_activate(q);
	cpu = kt_cpu >= 0 ? kt_cpu : (curcpu + 1) % (mp_maxid + 1);
	if (CPU_ABSENT(cpu))
		cpu = curcpu;
	kwq_notifier_init(&nf, cpu);
	signals = queued = coalesced = 0;
	for (i = 0; i < kt_items; i++) {
		atomic_add_64(&kt_nf_state, 1);
		signals++;
		if (kwq_notify(q, &nf))
			queued++;
		else
			coalesced++;
	}
	kwq_drain(q);
	kwq_destroy(q);
	kt_r_in = signals;
	kt_r_out = queued;
	kt_r_coalesced = coalesced;
	kt_r_runs = kt_nf_runs;
	if (kt_nf_seen != kt_nf_state)
		kt_fail("final state %ju not observed (saw %ju)",
		    (uintmax_t)kt_nf_state, (uintmax_t)kt_nf_seen);
	else if (kt_nf_runs > signals || kt_nf_runs < 1)
		kt_fail("runs %ju vs signals %ju", (uintmax_t)kt_nf_runs,
		    (uintmax_t)signals);
	else if (kt_nf_runs != queued)
		kt_fail("runs %ju != queued %ju", (uintmax_t)kt_nf_runs,
		    (uintmax_t)queued);
}

/* ================================================================ P1b */

/*
 * A flood: a pool of items cycled between a producer thread on another CPU
 * and the handler on the target CPU.  The handler burns `cost' us per item
 * (busy, no sleeping), accounts its own CPU time, honours the budget if
 * `coop', and hands items back to the pool.
 */
struct kt_flood {
	/* Read-mostly configuration. */
	struct kwq	*q;
	int		idx;
	int		cpu;		/* target (queue, CPU) */
	int		pcpu;		/* producer CPU */
	u_int		npool;
	struct titem	*items;
	uint64_t	cost_us;
	bool		coop;
	bool		stamp;		/* items carry an enqueue stamp */
	bool		empty_handler;
	/* producer mode */
	enum { F_FLOOD, F_BURSTPAUSE, F_IFPAIR, F_BURST, F_NONE } mode;
	u_int		burst;
	u_int		pause_us;
	/* ifpair: a sender's pause between packets, ns (0 = none) */
	uint64_t	gap_ns;
	/* burst (SPEC-p4e 2.1): mode, spacing, PRNG; the handler's burst state */
	u_int		burst_mode;
	uint64_t	burst_gap_ns;
	uint64_t	prng;
	bool		expect_first;	/* the next item served starts a burst */
	uint64_t	bursts;
	uint64_t	passes;
	/* ifpair: targets cpu .. cpu + spread - 1, spin ticks per packet */
	u_int		spread;
	u_int		nprod;		/* senders of this interface: kt_scale[idx .. idx + nprod - 1] */
	uint64_t	prod_spin;
	uint64_t	cons_spin;
	struct kt_cstat	*cstat;		/* [spread], the interface's worker CPUs */
	struct thread	*td;
	volatile bool	stop, exited;
	bool		owns_q;		/* drains and destroys q at teardown */
	bool		fanin;		/* handler routes items back by ti_qidx to kt_scale[] */

	/* Shared: the pool, taken by the producer, refilled by the handler. */
	struct mtx	mtx __aligned(KT_LINE);
	STAILQ_HEAD(, kwq_item) free;
	u_int		nfree;
	u_int		waiters;	/* producers asleep on an empty pool */

	/* Written by the handler (the worker CPU) only. */
	uint64_t	cycles __aligned(KT_LINE);
	uint64_t	items_out, maxlat_us, requeued;

	/* Written by the producer only. */
	uint64_t	produced __aligned(KT_LINE);
	uint64_t	prod_ticks;
	uint64_t	enq_slow;	/* ifpair: enqueues over 2 us */
	uint64_t	rejected;	/* ifpair: ENOBUFS */
	uint64_t	waits;		/* ifpair: window empty, slept */
};

/*
 * ifpair: what one worker CPU did for one interface.  Written by that
 * CPU's worker only, one line each.
 */
struct kt_cstat {
	uint64_t	items;
	uint64_t	passes;
	uint64_t	spin_ticks;	/* packet work */
	uint64_t	hand_ticks;	/* whole handler call, spin included */
	uint64_t	requeued;
	uint64_t	lat_sum;	/* enqueue-to-handler, sbintime units */
	uint64_t	lat_max;
} __aligned(KT_LINE);

static struct kt_flood *kt_floods[2];
static struct kt_flood **kt_scale;	/* scale scenario's pairs */
static u_int kt_nscale;

static void
kt_flood_return(struct kt_flood *f, struct kwq_item *it)
{
	mtx_lock(&f->mtx);
	STAILQ_INSERT_TAIL(&f->free, it, kwi_link);
	f->nfree++;
	if (f->waiters != 0)
		wakeup_one(&f->free);
	mtx_unlock(&f->mtx);
}

/*
 * Return a whole batch under one lock.  Per-item returns made the pool
 * mutex the second most contended lock on a07 (36 % of adaptive spins)
 * and cost a lock round trip per item; a client frees or recycles its
 * items in batches (or through a per-CPU cache), so the harness should
 * not charge kwq for a shared lock it would not have.
 */
STAILQ_HEAD(kt_itemq, kwq_item);

static void
kt_flood_return_list(struct kt_flood *f, struct kt_itemq *lst, u_int n)
{
	if (n == 0)
		return;
	mtx_lock(&f->mtx);
	STAILQ_CONCAT(&f->free, lst);
	f->nfree += n;
	if (f->waiters != 0)
		wakeup(&f->free);
	mtx_unlock(&f->mtx);
}

/*
 * Sleep until the pool refills (or a tick passes).  Producers used to
 * kern_yield(PRI_UNCHANGED) in a loop here.  On the GENERIC kernel that
 * loop switched ~1M times/s, holding the producer CPU's scheduler lock
 * almost continuously; the timer interrupt that migrated the callout
 * thread to that CPU then spun on the lock for 40 ms .. 1 s (SCHED.md
 * S10.2, the "GENERIC stall").  A test load must not do that; nothing in
 * kwq does.  Called with f->mtx held; returns with it released.
 */
static void
kt_pool_wait(struct kt_flood *f)
{
	mtx_assert(&f->mtx, MA_OWNED);
	if (!f->stop) {
		f->waiters++;
		msleep(&f->free, &f->mtx, 0, "ktpool", 1);
		f->waiters--;
	}
	mtx_unlock(&f->mtx);
}

static void kt_fanin_handler(struct kt_flood *f, struct kwq_item *head, int n);
static void kt_flood_handler_one(struct kt_flood *f, struct kwq_item *head, int n);
static void kt_ifpair_handler(struct kt_flood *f, struct kwq_item *head, int n);
static void kt_ifpair_producer(struct kt_flood *f);
static void kt_burst_handler(struct kt_flood *f, struct kwq_item *head, int n);

static void
kt_flood_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
{
	struct kt_flood *f = ctx;
	struct kwq_item *it, *next;
	struct titem *ti;
	struct kt_itemq back = STAILQ_HEAD_INITIALIZER(back);
	int left;

	left = n < 0 ? -n : n;
	if (n < 0) {
		for (it = head; it != NULL; it = next) {
			next = KWQ_ITEM_NEXT(f->q, it);
			KWQ_ITEM_INIT(it);
			if (f->fanin) {
				ti = __containerof(it, struct titem, ti_item);
				kt_flood_return(kt_scale[ti->ti_qidx], it);
			} else
				STAILQ_INSERT_TAIL(&back, it, kwi_link);
		}
		kt_flood_return_list(f, &back, left);
		return;
	}
	if (f->mode == F_IFPAIR)
		kt_ifpair_handler(f, head, n);
	else if (f->mode == F_BURST)
		kt_burst_handler(f, head, n);
	else if (f->fanin)
		kt_fanin_handler(f, head, n);
	else
		kt_flood_handler_one(f, head, n);
}

/*
 * ifpair receiver, run by the worker of whichever of the interface's CPUs
 * the flow hashed to.  Per packet: spin for the receiving stack's work,
 * check the budget (items cost over a microsecond, S5 guidance: per
 * item).  Items go back to their sender's window in one batch per sender
 * per pass, the way a stack frees mbufs through its per-CPU caches rather
 * than one lock round trip per packet.
 */
static void
kt_ifpair_handler(struct kt_flood *f, struct kwq_item *head, int n)
{
	struct kt_itemq back[KT_MAX_FANIN];
	u_int cnt[KT_MAX_FANIN];
	struct kwq_item *it, *next;
	struct titem *ti;
	struct kt_cstat *cs;
	uint64_t t0, now, spin = 0;
	u_int i, nprod, base, local;
	int done = 0;

	/*
	 * The run's own sender count, never the live kt_fanin knob: the
	 * queue keeps working off its backlog after the scenario reported
	 * "done", and a knob written for the next run meanwhile made this
	 * handler index past kt_scale[] (the 2026-09-29 panics, a07 and the
	 * guest: "bad ifpair item").
	 */
	nprod = f->nprod;
	base = (u_int)f->idx;		/* the interface's first sender */
	for (i = 0; i < nprod; i++) {
		STAILQ_INIT(&back[i]);
		cnt[i] = 0;
	}
	local = (u_int)(curcpu - f->cpu);
	if (local >= f->spread)
		panic("kwq_test: ifpair handler on CPU %d outside %d..%d", curcpu,
		    f->cpu, f->cpu + (int)f->spread - 1);
	cs = &f->cstat[local];
	t0 = kt_ticks();
	now = (uint64_t)sbinuptime();
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(f->q, it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC || (u_int)ti->ti_qidx - base >= nprod)
			panic("kwq_test: bad ifpair item %p", ti);
		if (ti->ti_cpu != curcpu)
			panic("kwq_test: ifpair item for CPU %d handled on %d", ti->ti_cpu, curcpu);
		if (now > ti->ti_seq) {	/* stamped at enqueue, one read per pass */
			cs->lat_sum += now - ti->ti_seq;
			if (now - ti->ti_seq > cs->lat_max)
				cs->lat_max = now - ti->ti_seq;
		}
		if (f->cons_spin != 0) {
			kt_spin_ticks(f->cons_spin);
			spin += f->cons_spin;
		}
		KWQ_ITEM_INIT(it);
		STAILQ_INSERT_TAIL(&back[ti->ti_qidx - base], it, kwi_link);
		cnt[ti->ti_qidx - base]++;
		done++;
		if (next != NULL && kwq_budget_left(f->q) == 0) {
			/* tail NULL: the rest of the handed list, no walk */
			kwq_requeue(f->q, next, NULL, n - done);
			cs->requeued += n - done;
			break;
		}
	}
	cs->hand_ticks += kt_tickdelta(t0);
	cs->spin_ticks += spin;
	cs->items += done;
	cs->passes++;
	for (i = 0; i < nprod; i++)
		kt_flood_return_list(kt_scale[base + i], &back[i], cnt[i]);
}

/* fanin: items came from several producers' pools; return each to its own. */
static void
kt_fanin_handler(struct kt_flood *f, struct kwq_item *head, int n)
{
	struct kt_itemq back[KT_MAX_FANIN];
	u_int cnt[KT_MAX_FANIN];
	struct kwq_item *it, *next;
	struct titem *ti;
	uint64_t t0;
	u_int i;
	int done = 0;

	for (i = 0; i < kt_nscale; i++) {
		STAILQ_INIT(&back[i]);
		cnt[i] = 0;
	}
	t0 = kt_ticks();
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(f->q, it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC || (u_int)ti->ti_qidx >= kt_nscale)
			panic("kwq_test: bad fanin item %p", ti);
		KWQ_ITEM_INIT(it);
		STAILQ_INSERT_TAIL(&back[ti->ti_qidx], it, kwi_link);
		cnt[ti->ti_qidx]++;
		done++;
		if (next != NULL && kwq_budget_left(f->q) == 0) {
			kwq_requeue(f->q, next, NULL, n - done);
			f->requeued += n - done;
			break;
		}
	}
	f->cycles += kt_tickdelta(t0);
	f->items_out += done;
	for (i = 0; i < kt_nscale; i++)
		kt_flood_return_list(kt_scale[i], &back[i], cnt[i]);
}

static void
kt_flood_handler_one(struct kt_flood *f, struct kwq_item *head, int n)
{
	struct kwq_item *it, *next;
	struct titem *ti;
	struct kt_itemq back = STAILQ_HEAD_INITIALIZER(back);
	uint64_t t0;
	sbintime_t now;
	int left = n, done = 0;

	t0 = kt_ticks();
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(f->q, it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC)
			panic("kwq_test: bad flood item %p", ti);
		if (f->stamp) {
			now = sbinuptime();
			if (now > (sbintime_t)ti->ti_seq) {
				uint64_t l = kt_sbt2us(now - (sbintime_t)ti->ti_seq);
				if (l > f->maxlat_us)
					f->maxlat_us = l;
			}
		}
		if (!f->empty_handler && f->cost_us != 0)
			DELAY(f->cost_us);
		KWQ_ITEM_INIT(it);
		STAILQ_INSERT_TAIL(&back, it, kwi_link);
		done++; left--;
		/* Progress rule: at least one item, then the budget decides. */
		if (f->coop && next != NULL && kwq_budget_left(f->q) == 0) {
			kwq_requeue(f->q, next, NULL, left);
			f->requeued += left;
			break;
		}
	}
	f->cycles += kt_tickdelta(t0);
	f->items_out += done;
	kt_flood_return_list(f, &back, done);
}

/*
 * burst (SPEC-p4e 2.1, 2.4): latency histograms in quarter-octave buckets
 * (four buckets per power of two: a bucket's width is 19 % of its value),
 * nanoseconds in, the bucket's upper bound in microseconds out; the
 * maximum is kept exactly.  One burst scenario runs at a time.
 */
#define	KT_HIST_N	(64 * 4)
static uint64_t	kt_hist_first[KT_HIST_N], kt_hist_last[KT_HIST_N];
static uint64_t	kt_hist_first_n, kt_hist_last_n, kt_hist_first_max, kt_hist_last_max;

static inline u_int
kt_hist_bucket(uint64_t ns)
{
	int msb;

	if (ns < 4)
		return ((u_int)ns);
	msb = fls((int)(ns >> 32)) ? 32 + fls((int)(ns >> 32)) - 1 : fls((int)ns) - 1;
	return ((u_int)msb * 4 + (u_int)((ns >> (msb - 2)) & 3));
}

static uint64_t
kt_hist_bound_us(u_int b)
{
	uint64_t ns;

	if (b < 8)		/* 0..3 ns exact; 4..7 unused */
		return (0);
	ns = ((uint64_t)(4 + (b & 3) + 1)) << ((b / 4) - 2);	/* upper bound of the bucket */
	return ((ns + 999) / 1000);
}

static uint64_t
kt_hist_pct(const uint64_t *h, uint64_t n, u_int pct)
{
	uint64_t acc = 0, want;
	u_int b;

	if (n == 0)
		return (0);
	want = (n * pct + 99) / 100;
	for (b = 0; b < KT_HIST_N; b++) {
		acc += h[b];
		if (acc >= want)
			return (kt_hist_bound_us(b));
	}
	return (kt_hist_bound_us(KT_HIST_N - 1));
}

static void
kt_hist_add(uint64_t *h, uint64_t *n, uint64_t *max, uint64_t ns)
{
	h[kt_hist_bucket(ns)]++;
	(*n)++;
	if (ns > *max)
		*max = ns;
}

/*
 * The burst handler: cons_spin per item, the budget honoured per item,
 * items back to the pool in one batch.  An item with ti_seen set is the
 * last of its burst; the item after it (or the first ever) starts one.
 * Ages are read against one sbinuptime() per pass, as the ifpair handler
 * does.
 */
static void
kt_burst_handler(struct kt_flood *f, struct kwq_item *head, int n)
{
	struct kwq_item *it, *next;
	struct titem *ti;
	struct kt_itemq back = STAILQ_HEAD_INITIALIZER(back);
	uint64_t t0, now, age;
	int done = 0;

	t0 = kt_ticks();
	now = (uint64_t)sbinuptime();
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(f->q, it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC)
			panic("kwq_test: bad burst item %p", ti);
		age = now > ti->ti_seq ? kt_sbt2ns((sbintime_t)(now - ti->ti_seq)) : 0;
		if (f->expect_first) {
			kt_hist_add(kt_hist_first, &kt_hist_first_n, &kt_hist_first_max, age);
			f->expect_first = false;
		}
		if (ti->ti_seen != 0) {
			kt_hist_add(kt_hist_last, &kt_hist_last_n, &kt_hist_last_max, age);
			f->expect_first = true;
		}
		if (f->cons_spin != 0)
			kt_spin_ticks(f->cons_spin);
		KWQ_ITEM_INIT(it);
		STAILQ_INSERT_TAIL(&back, it, kwi_link);
		done++;
		if (next != NULL && kwq_budget_left(f->q) == 0) {
			kwq_requeue(f->q, next, NULL, n - done);
			f->requeued += n - done;
			break;
		}
	}
	f->cycles += kt_tickdelta(t0);
	f->items_out += done;
	f->passes++;
	kt_flood_return_list(f, &back, done);
}

/* xorshift64*: good enough for an arrival process. */
static inline uint64_t
kt_prng(uint64_t *st)
{
	uint64_t x = *st;

	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	*st = x;
	return (x * 0x2545F4914F6CDD1DULL);
}

/*
 * An exponential gap with mean `mean' ns: -ln(U) x mean, with -ln(U) from
 * the integer part of -log2(U) (the leading zeros of a 64-bit U) plus a
 * 64-entry table of the fraction, in 1/65536.  Capped at 32 x mean.
 */
static uint64_t
kt_exp_gap(uint64_t *st, uint64_t mean)
{
	static const uint16_t log2_frac[64] = {	/* log2(1 + i/64) x 65536 */
	    0, 1455, 2866, 4236, 5568, 6863, 8124, 9352, 10549, 11716, 12855, 13968,
	    15055, 16117, 17156, 18173, 19168, 20143, 21098, 22034, 22952, 23852,
	    24736, 25604, 26455, 27292, 28114, 28922, 29717, 30498, 31267, 32024,
	    32768, 33501, 34223, 34934, 35634, 36324, 37004, 37674, 38335, 38986,
	    39629, 40263, 40888, 41505, 42114, 42715, 43309, 43895, 44473, 45045,
	    45609, 46167, 46718, 47262, 47800, 48332, 48858, 49378, 49892, 50400,
	    50903, 51400 };
	uint64_t u, nlog2;	/* -log2(U) in 1/65536 */
	int lz;

	u = kt_prng(st);
	if (u == 0)
		u = 1;
	lz = 63 - (fls((int)(u >> 32)) ? 32 + fls((int)(u >> 32)) - 1 : fls((int)u) - 1);
	/* U = u / 2^64 = m x 2^(-lz-1) with m in [1, 2): -log2 U = lz + 1 - log2 m */
	nlog2 = ((uint64_t)(lz + 1) << 16) - log2_frac[(u << lz >> 57) & 63];
	/* x ln 2 (45426 / 65536), then x mean */
	return ((((nlog2 * 45426) >> 16) * mean) >> 16 < 32 * mean ?
	    ((nlog2 * 45426) >> 16) * mean >> 16 : 32 * mean);
}

/* Wait `ns': sleep with a loose precision from 100 us up, spin below. */
static void
kt_gap(uint64_t ns)
{
	if (ns == 0)
		return;
	if (ns >= 100000)
		pause_sbt("ktgap", nstosbt(ns), nstosbt(ns / 8), 0);
	else
		kt_spin_ticks(kt_ns2ticks(ns));
}

/*
 * The burst producer: mode 0 enqueues burst_items back to back and then
 * waits burst_gap; modes 1 and 2 enqueue one item per gap, constant or
 * exponentially distributed (SPEC-p4e 2.4).  Every item is stamped; the
 * last item of a burst (every item in modes 1 and 2) carries ti_seen = 1.
 */
static void
kt_burst_producer(struct kt_flood *f)
{
	struct kwq_item *it;
	struct titem *ti;
	uint64_t t0, gap;
	u_int k, nb;

	f->expect_first = true;
	f->prng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)f;
	while (!f->stop) {
		nb = f->burst_mode == 0 ? f->burst : 1;
		for (k = 0; k < nb && !f->stop; k++) {
			mtx_lock(&f->mtx);
			it = STAILQ_FIRST(&f->free);
			if (it == NULL) {
				kt_pool_wait(f);	/* releases f->mtx */
				k--;
				continue;
			}
			STAILQ_REMOVE_HEAD(&f->free, kwi_link);
			f->nfree--;
			mtx_unlock(&f->mtx);
			if (f->prod_spin != 0)
				kt_spin_ticks(f->prod_spin);
			KWQ_ITEM_INIT(it);
			ti = __containerof(it, struct titem, ti_item);
			ti->ti_seen = (k == nb - 1) ? 1 : 0;
			ti->ti_seq = (uint64_t)sbinuptime();
			t0 = kt_ticks();
			if (kwq_enqueue(f->q, f->cpu, it) != 0) {
				kt_flood_return(f, it);
				f->rejected++;
			} else
				f->produced++;
			f->prod_ticks += kt_tickdelta(t0);
		}
		f->bursts++;
		gap = f->burst_mode == 2 ? kt_exp_gap(&f->prng, f->burst_gap_ns) :
		    f->burst_gap_ns;
		kt_gap(gap);
	}
}

static void
kt_flood_producer(void *arg)
{
	struct kt_flood *f = arg;
	struct kwq_item *it;
	struct titem *ti;
	uint64_t t0;
	u_int k;
	int error;

	kt_prod_bind(f->pcpu);
	if (f->mode == F_IFPAIR) {
		kt_ifpair_producer(f);
		goto out;
	}
	if (f->mode == F_BURST) {
		kt_burst_producer(f);
		goto out;
	}
	while (!f->stop) {
		if (f->mode == F_BURSTPAUSE) {
			/* wait until every item is back (the list emptied) */
			mtx_lock(&f->mtx);
			if (f->nfree < f->npool) {
				kt_pool_wait(f);
				continue;
			}
			mtx_unlock(&f->mtx);
			DELAY(f->pause_us);
			for (k = 0; k < f->burst && !f->stop; k++) {
				mtx_lock(&f->mtx);
				it = STAILQ_FIRST(&f->free);
				if (it == NULL) { mtx_unlock(&f->mtx); break; }
				STAILQ_REMOVE_HEAD(&f->free, kwi_link);
				f->nfree--;
				mtx_unlock(&f->mtx);
				KWQ_ITEM_INIT(it);
				if (kwq_enqueue(f->q, f->cpu, it) != 0)
					kt_flood_return(f, it);
				else
					f->produced++;
			}
			continue;
		}
		mtx_lock(&f->mtx);
		it = STAILQ_FIRST(&f->free);
		if (it == NULL) {
			kt_pool_wait(f);	/* releases f->mtx */
			continue;
		}
		if (kt_batch > 1) {
			/* S16 batching: up to kt_batch items in one call. */
			struct kwq_item *tail = it, *nx;
			u_int k = 1;

			while (k < kt_batch && (nx = STAILQ_NEXT(tail, kwi_link)) != NULL) {
				tail = nx;
				k++;
			}
			nx = STAILQ_NEXT(tail, kwi_link);
			if (nx == NULL)
				STAILQ_INIT(&f->free);
			else
				STAILQ_FIRST(&f->free) = nx;
			f->nfree -= k;
			mtx_unlock(&f->mtx);
			KWQ_ITEM_INIT(tail);
			t0 = kt_ticks();
			error = kwq_enqueue_list(f->q, f->cpu, it, tail, (int)k);
			f->prod_ticks += kt_tickdelta(t0);
			if (error != 0) {
				struct kt_itemq back;

				STAILQ_INIT(&back);
				for (; it != NULL; it = nx) {
					nx = STAILQ_NEXT(it, kwi_link);
					KWQ_ITEM_INIT(it);
					STAILQ_INSERT_TAIL(&back, it, kwi_link);
				}
				kt_flood_return_list(f, &back, k);
			} else
				f->produced += k;
			continue;
		}
		STAILQ_REMOVE_HEAD(&f->free, kwi_link);
		f->nfree--;
		mtx_unlock(&f->mtx);
		KWQ_ITEM_INIT(it);
		if (f->stamp) {
			ti = __containerof(it, struct titem, ti_item);
			ti->ti_seq = (uint64_t)sbinuptime();
		}
		t0 = kt_ticks();
		error = kwq_enqueue(f->q, f->cpu, it);
		f->prod_ticks += kt_tickdelta(t0);
		if (error != 0)
			kt_flood_return(f, it);
		else
			f->produced++;
	}
out:
	f->exited = true;
	wakeup(f);
	kthread_exit();
}

/*
 * ifpair sender: per packet, spin for the sending stack's work, then one
 * kwq_enqueue() (kt_batch > 1: one kwq_enqueue_list() per kt_batch
 * packets) to the worker CPU the packet's flow hashes to.  The window is
 * the pool: when every item is in flight the sender sleeps until the
 * handler returns some, like a TCP sender blocked on its congestion
 * window.  The time inside the enqueue call is measured per call, so its
 * share of the sender's CPU and its tail (calls over 2 us: a lock convoy)
 * come out separately from the stack work.
 */
static void
kt_ifpair_producer(struct kt_flood *f)
{
	struct kwq_item *it, *tail, *nx;
	struct titem *ti;
	uint64_t d, slow, now, seq = 0;
	uint32_t h;
	u_int k, want;
	int error, target;

	slow = kt_ns2ticks(2000);
	while (!f->stop) {
		want = kt_batch > 1 ? kt_batch : 1;
		mtx_lock(&f->mtx);
		it = STAILQ_FIRST(&f->free);
		if (it == NULL) {
			f->waits++;
			kt_pool_wait(f);	/* releases f->mtx */
			continue;
		}
		tail = it;
		k = 1;
		while (k < want && (nx = STAILQ_NEXT(tail, kwi_link)) != NULL) {
			tail = nx;
			k++;
		}
		nx = STAILQ_NEXT(tail, kwi_link);
		if (nx == NULL)
			STAILQ_INIT(&f->free);
		else
			STAILQ_FIRST(&f->free) = nx;
		f->nfree -= k;
		mtx_unlock(&f->mtx);
		KWQ_ITEM_INIT(tail);
		/*
		 * The stack's work for k packets, then the flow hash to a
		 * worker.  Each sender's flow sequence is mixed with its own
		 * seed: with one shared sequence every sender aimed at the
		 * same worker at the same time and the layout degenerated
		 * into one hot list and idle workers, which no set of real
		 * flows does.
		 */
		if (f->prod_spin != 0)
			kt_spin_ticks(f->prod_spin * k);
		seq++;
		h = (uint32_t)seq ^ ((uint32_t)f->idx * 0x9E3779B9u);
		h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
		target = f->cpu + (int)(h % f->spread);
		now = (uint64_t)sbinuptime();
		for (nx = it; nx != NULL; nx = STAILQ_NEXT(nx, kwi_link)) {
			ti = __containerof(nx, struct titem, ti_item);
			ti->ti_cpu = target;
			ti->ti_seq = now;	/* enqueue stamp: queue latency */
		}
		d = kt_ticks();
		if (k == 1)
			error = kwq_enqueue(f->q, target, it);
		else
			error = kwq_enqueue_list(f->q, target, it, tail, (int)k);
		d = kt_tickdelta(d);
		f->prod_ticks += d;
		if (d > slow)
			f->enq_slow++;
		if (error != 0) {
			struct kt_itemq back;

			STAILQ_INIT(&back);
			for (; it != NULL; it = nx) {
				nx = STAILQ_NEXT(it, kwi_link);
				KWQ_ITEM_INIT(it);
				STAILQ_INSERT_TAIL(&back, it, kwi_link);
			}
			kt_flood_return_list(f, &back, k);
			f->rejected += k;
		} else
			f->produced += k;
		/* A sparse sender (SPEC-p4e 2.3): the pause between packets. */
		if (f->gap_ns != 0)
			kt_gap(f->gap_ns);
	}
}

static struct kt_flood *
kt_flood_alloc(int idx, u_int npool, uint64_t cost_us, bool coop, int cpu, int pcpu)
{
	struct kt_flood *f;
	u_int i;

	f = malloc(sizeof(*f), M_KWQ_TEST, M_WAITOK | M_ZERO);
	f->idx = idx; f->cpu = cpu; f->pcpu = pcpu; f->npool = npool;
	f->cost_us = cost_us; f->coop = coop; f->mode = F_FLOOD;
	mtx_init(&f->mtx, "kwq_test flood", NULL, MTX_DEF);
	STAILQ_INIT(&f->free);
	f->items = malloc(sizeof(*f->items) * npool, M_KWQ_TEST, M_WAITOK | M_ZERO);
	for (i = 0; i < npool; i++) {
		f->items[i].ti_magic = TITEM_MAGIC;
		f->items[i].ti_qidx = idx;
		KWQ_ITEM_INIT(&f->items[i].ti_item);
		STAILQ_INSERT_TAIL(&f->free, &f->items[i].ti_item, kwi_link);
	}
	f->nfree = npool;
	return (f);
}

static struct kt_flood *
kt_flood_create(int idx, const char *name, u_int weight, u_int npool,
    uint64_t cost_us, bool coop, int cpu, int pcpu)
{
	struct kt_flood *f;

	f = kt_flood_alloc(idx, npool, cost_us, coop, cpu, pcpu);
	f->q = kt_create(name, KWQ_F_DISCARD, weight, kt_flood_handler, f);
	if (f->q == NULL) {
		kt_fail("kwq_create(%s) failed", name);
		return (f);
	}
	f->owns_q = true;
	kwq_activate(f->q);
	return (f);
}

/* A producer with its own pool feeding another flood's queue (fanin). */
static struct kt_flood *
kt_flood_attach(int idx, struct kwq *q, u_int npool, int cpu, int pcpu)
{
	struct kt_flood *f;

	f = kt_flood_alloc(idx, npool, 0, true, cpu, pcpu);
	f->q = q;
	f->owns_q = false;
	return (f);
}

static void
kt_flood_start(struct kt_flood *f)
{
	if (f->mode == F_NONE)
		return;
	if (kthread_add(kt_flood_producer, f, NULL, &f->td, 0, 0,
	    "kwq_test_prod%d", f->idx) != 0) {
		kt_fail("producer thread");
		f->exited = true;
	}
}

static void
kt_flood_stop(struct kt_flood *f)
{
	f->stop = true;
	mtx_lock(&f->mtx);
	if (f->waiters != 0)
		wakeup(&f->free);
	mtx_unlock(&f->mtx);
	while (f->td != NULL && !f->exited)
		tsleep(f, 0, "ktprod", hz / 100);
}

/* Destroy the previous run's floods (queues stay alive for inspection). */
static void
kt_floods_destroy(void)
{
	struct kt_flood *f;
	int i;

	for (i = 0; i < 2; i++) {
		f = kt_floods[i];
		if (f == NULL)
			continue;
		if (f->q != NULL) {
			kwq_drain(f->q);
			kwq_destroy(f->q);
		}
		mtx_destroy(&f->mtx);
		free(f->items, M_KWQ_TEST);
		free(f, M_KWQ_TEST);
		kt_floods[i] = NULL;
	}
	for (i = 0; i < (int)kt_nscale; i++) {
		f = kt_scale[i];
		if (f == NULL)
			continue;
		if (f->q != NULL && f->owns_q) {
			kwq_drain(f->q);
			kwq_destroy(f->q);
		}
		mtx_destroy(&f->mtx);
		free(f->cstat, M_KWQ_TEST);
		free(f->items, M_KWQ_TEST);
		free(f, M_KWQ_TEST);
	}
	free(kt_scale, M_KWQ_TEST);
	kt_scale = NULL;
	kt_nscale = 0;
}

/*
 * scale: kt_pairs independent producer/consumer pairs, pair i's queue
 * served on CPU 1 + 2i and fed from CPU 2 + 2i (CPU 0 is left to the
 * rest of the system), empty handler, kt_batch items per enqueue call.
 * Reports the aggregate rate and the per-pair spread.
 */
/*
 * fanin: kt_fanin producers, each on its own CPU (2 .. fanin + 1), feed one
 * (queue, CPU) list served on CPU 1; each producer has its own item pool and
 * the handler returns items to their pools in per-pool batches.  Set
 * kern.kwq_test.limit high enough for fanin x items in flight.
 */
static void
kt_run_fanin(void)
{
	struct kt_flood *f;
	sbintime_t t0, t1;
	uint64_t lo = UINT64_MAX, hi = 0, wall_us, total;
	u_int i, m, avail;

	avail = 0;
	CPU_FOREACH(i)
		avail++;
	m = kt_fanin;
	if (m == 0)
		m = 1;
	if (m > KT_MAX_FANIN || m + 2 > avail) {
		kt_fail("fanin: %u producers need %u CPUs, %u present (max %u)", m, m + 2, avail, KT_MAX_FANIN);
		return;
	}
	kt_scale = malloc(sizeof(*kt_scale) * m, M_KWQ_TEST, M_WAITOK | M_ZERO);
	kt_nscale = m;
	f = kt_flood_create(0, "ktf", 1, kt_items ? kt_items : 1024, 0, true, 1, 2);
	kt_scale[0] = f;
	if (f->q == NULL)
		return;
	f->empty_handler = true;
	f->fanin = true;
	for (i = 1; i < m; i++)
		kt_scale[i] = kt_flood_attach(i, f->q, kt_items ? kt_items : 1024, 1, 2 + i);
	t0 = sbinuptime();
	for (i = 0; i < m; i++)
		kt_flood_start(kt_scale[i]);
	pause("ktrun", kt_secs * hz);
	for (i = 0; i < m; i++)
		kt_flood_stop(kt_scale[i]);
	t1 = sbinuptime();
	wall_us = kt_sbt2us(t1 - t0);
	total = f->items_out;
	for (i = 0; i < m; i++) {
		kt_r_in += kt_scale[i]->produced;
		if (kt_scale[i]->produced < lo) lo = kt_scale[i]->produced;
		if (kt_scale[i]->produced > hi) hi = kt_scale[i]->produced;
	}
	kt_r_out = total;
	kt_r_runs = m;
	kt_r_ns_per_item = total ? wall_us * 1000 / total : 0;
	kt_r_cycles_a = wall_us ? lo * 1000000 / wall_us : 0;
	kt_r_cycles_b = wall_us ? hi * 1000000 / wall_us : 0;
	snprintf(kt_msg, sizeof(kt_msg), "fanin %u, batch %u: %ju items/s, per producer %ju..%ju, requeued %ju",
	    m, kt_batch, (uintmax_t)(wall_us ? total * 1000000 / wall_us : 0),
	    (uintmax_t)kt_r_cycles_a, (uintmax_t)kt_r_cycles_b, (uintmax_t)f->requeued);
}

static void
kt_run_scale(void)
{
	struct kt_flood *f;
	sbintime_t t0, t1;
	uint64_t total = 0, lo = UINT64_MAX, hi = 0, wall_us;
	char name[KWQ_NAMELEN];
	u_int i, n, avail;

	avail = 0;
	CPU_FOREACH(i)
		avail++;
	n = kt_pairs;
	if (n == 0)
		n = 1;
	if (2 * n + 1 > avail) {
		kt_fail("scale: %u pairs need %u CPUs, %u present", n, 2 * n + 1, avail);
		return;
	}
	kt_scale = malloc(sizeof(*kt_scale) * n, M_KWQ_TEST, M_WAITOK | M_ZERO);
	kt_nscale = n;
	for (i = 0; i < n; i++) {
		snprintf(name, sizeof(name), "kts%u", i);
		f = kt_flood_create(i, name, 1, kt_items ? kt_items : 1024, 0, true,
		    1 + 2 * i, 2 + 2 * i);
		kt_scale[i] = f;
		if (f->q == NULL)
			return;
		f->empty_handler = true;
	}
	t0 = sbinuptime();
	for (i = 0; i < n; i++)
		kt_flood_start(kt_scale[i]);
	pause("ktrun", kt_secs * hz);
	for (i = 0; i < n; i++)
		kt_flood_stop(kt_scale[i]);
	t1 = sbinuptime();
	wall_us = kt_sbt2us(t1 - t0);
	for (i = 0; i < n; i++) {
		f = kt_scale[i];
		total += f->items_out;
		kt_r_in += f->produced;
		if (f->items_out < lo) lo = f->items_out;
		if (f->items_out > hi) hi = f->items_out;
	}
	kt_r_out = total;
	kt_r_runs = n;
	kt_r_ns_per_item = total ? wall_us * 1000 / total : 0;	/* aggregate */
	kt_r_cycles_a = wall_us ? lo * 1000000 / wall_us : 0;	/* slowest pair items/s */
	kt_r_cycles_b = wall_us ? hi * 1000000 / wall_us : 0;	/* fastest pair items/s */
	snprintf(kt_msg, sizeof(kt_msg), "%u pairs, batch %u: %ju items/s aggregate, per pair %ju..%ju",
	    n, kt_batch, (uintmax_t)(wall_us ? total * 1000000 / wall_us : 0),
	    (uintmax_t)kt_r_cycles_a, (uintmax_t)kt_r_cycles_b);
}

/*
 * ifpair: the shape of if_pair on kwq (P4).  kt_ifaces interfaces, each a
 * queue whose flows are hashed over kt_spread worker CPUs and fed by
 * kt_fanin senders on CPUs of their own; per packet the sender spins
 * kt_prod_ns and enqueues once, the handler spins kt_cons_ns.  Worker CPUs
 * come first (1 .. ifaces * spread), sender CPUs after them, so the two
 * sides never share a core and every cycle a side loses to the handoff
 * shows up in its own numbers.  kt_items is each sender's window.
 *
 * Reads: items/s; result_enq_pct and result_prod_ns_per_item (what the
 * enqueue costs the sender, mean, and its share of the sender's CPU),
 * result_enq_slow (calls over 2 us: convoys); result_cons_pct (share of the
 * worker CPUs' time in packet work: the rest is kwq's passes, the queue
 * lock, and returning items); result_pool_waits (senders blocked on a
 * full window: the receivers are the limit).  kwq's own counters per
 * (queue, CPU) and per worker are left readable, and lockstat(1) during
 * the run shows the queue locks by name ("kwq ifpN").
 */
static void
kt_run_ifpair(void)
{
	struct kt_flood *f, *owner;
	struct kt_cstat *cs;
	sbintime_t t0, t1;
	uint64_t wall_us, wall_ticks, total = 0, produced = 0, prod_ticks = 0,
	    spin_ticks = 0, hand_ticks = 0, lo = UINT64_MAX, hi = 0, lat_sum = 0,
	    lat_max = 0;
	char name[KWQ_NAMELEN];
	u_int i, j, nif, nprod, spread, avail, ncons, nsend, npool;
	int cpu, pcpu;

	avail = 0;
	CPU_FOREACH(i)
		avail++;
	nif = kt_ifaces != 0 ? kt_ifaces : 1;
	nprod = kt_fanin != 0 ? kt_fanin : 1;
	spread = kt_spread != 0 ? kt_spread : 1;
	ncons = nif * spread;
	nsend = nif * nprod;
	if (nprod > KT_MAX_FANIN || 1 + ncons + nsend > avail) {
		kt_fail("ifpair: %u ifaces x (%u workers + %u senders) need %u CPUs, %u present (fanin max %u)",
		    nif, spread, nprod, 1 + ncons + nsend, avail, KT_MAX_FANIN);
		return;
	}
	for (i = 1; i <= ncons + nsend; i++) {
		if (CPU_ABSENT(i)) {
			kt_fail("ifpair: CPU %u absent; needs CPUs 1..%u", i, ncons + nsend);
			return;
		}
	}
	npool = kt_items ? kt_items : 1024;
	kt_scale = malloc(sizeof(*kt_scale) * nsend, M_KWQ_TEST, M_WAITOK | M_ZERO);
	kt_nscale = nsend;
	for (i = 0; i < nif; i++) {
		cpu = 1 + (int)(i * spread);
		snprintf(name, sizeof(name), "ifp%u", i);
		owner = kt_flood_create((int)(i * nprod), name, 1, npool, 0, true, cpu, 0);
		kt_scale[i * nprod] = owner;
		if (owner->q == NULL)
			return;
		owner->cstat = malloc(sizeof(*owner->cstat) * spread, M_KWQ_TEST,
		    M_WAITOK | M_ZERO);
		for (j = 0; j < nprod; j++) {
			pcpu = 1 + (int)ncons + (int)(i * nprod + j);
			f = j == 0 ? owner : kt_flood_attach((int)(i * nprod + j), owner->q,
			    npool, cpu, pcpu);
			kt_scale[i * nprod + j] = f;
			f->pcpu = pcpu;
			f->mode = F_IFPAIR;
			f->fanin = true;	/* discards route back by ti_qidx */
			f->spread = spread;
			f->nprod = nprod;
			f->prod_spin = kt_ns2ticks(kt_prod_ns);
			f->cons_spin = kt_ns2ticks(kt_cons_ns);
			f->gap_ns = (uint64_t)kt_prod_gap_us * 1000;
		}
	}
	t0 = sbinuptime();
	for (i = 0; i < nsend; i++)
		kt_flood_start(kt_scale[i]);
	pause("ktrun", kt_secs * hz);
	for (i = 0; i < nsend; i++)
		kt_flood_stop(kt_scale[i]);
	t1 = sbinuptime();
	wall_us = kt_sbt2us(t1 - t0);
	wall_ticks = wall_us * kt_tick_hz / 1000000ULL;
	for (i = 0; i < nsend; i++) {
		f = kt_scale[i];
		produced += f->produced;
		prod_ticks += f->prod_ticks;
		kt_r_rejected += f->rejected;
		kt_r_enq_slow += f->enq_slow;
		kt_r_pool_waits += f->waits;
		if (f->produced < lo) lo = f->produced;
		if (f->produced > hi) hi = f->produced;
	}
	for (i = 0; i < nif; i++) {
		owner = kt_scale[i * nprod];
		for (j = 0; j < spread; j++) {
			cs = &owner->cstat[j];
			total += cs->items;
			spin_ticks += cs->spin_ticks;
			hand_ticks += cs->hand_ticks;
			kt_r_runs += cs->passes;
			lat_sum += cs->lat_sum;
			if (cs->lat_max > lat_max)
				lat_max = cs->lat_max;
		}
	}
	kt_r_maxlat_b_us = kt_sbt2us((sbintime_t)lat_max);		/* queue latency, max */
	kt_r_maxlat_a_us = total ? kt_sbt2us((sbintime_t)(lat_sum / total)) : 0;	/* mean */
	kt_r_in = produced;
	kt_r_out = total;
	kt_r_ns_per_item = total ? wall_us * 1000 / total : 0;
	kt_r_prod_ns_per_item = produced ?
	    prod_ticks * 1000000000ULL / kt_tick_hz / produced : 0;
	/* Senders run (or sleep on their window) for the whole wall time. */
	kt_r_enq_pct = wall_ticks ? prod_ticks * 100 / (wall_ticks * nsend) : 0;
	kt_r_cons_pct = wall_ticks ? spin_ticks * 100 / (wall_ticks * ncons) : 0;
	kt_r_cycles_a = spin_ticks * 1000000000ULL / kt_tick_hz;	/* packet work, ns */
	kt_r_cycles_b = hand_ticks * 1000000000ULL / kt_tick_hz;	/* handler calls, ns */
	snprintf(kt_msg, sizeof(kt_msg), "%u ifaces x %u workers x %u senders, spin %u/%u ns, batch %u, gap %u us: "
	    "%ju pkt/s (%ju..%ju per sender); enqueue %ju ns = %ju%% of sender CPU, %ju over 2 us; "
	    "workers %ju%% in packet work, %ju passes; queue latency mean %ju max %ju us; %ju rejects, %ju window waits",
	    nif, spread, nprod, kt_prod_ns, kt_cons_ns, kt_batch, kt_prod_gap_us,
	    (uintmax_t)(wall_us ? total * 1000000 / wall_us : 0),
	    (uintmax_t)(wall_us ? lo * 1000000 / wall_us : 0),
	    (uintmax_t)(wall_us ? hi * 1000000 / wall_us : 0),
	    (uintmax_t)kt_r_prod_ns_per_item, (uintmax_t)kt_r_enq_pct, (uintmax_t)kt_r_enq_slow,
	    (uintmax_t)kt_r_cons_pct, (uintmax_t)kt_r_runs,
	    (uintmax_t)kt_r_maxlat_a_us, (uintmax_t)kt_r_maxlat_b_us,
	    (uintmax_t)kt_r_rejected, (uintmax_t)kt_r_pool_waits);
}

static void
kt_floods_report(struct kt_flood *a, struct kt_flood *b)
{
	kt_r_in = a->produced + (b ? b->produced : 0);
	kt_r_out = a->items_out + (b ? b->items_out : 0);
	kt_r_cycles_a = a->cycles;
	kt_r_cycles_b = b ? b->cycles : 0;
	kt_r_maxlat_a_us = a->maxlat_us;
	kt_r_maxlat_b_us = b ? b->maxlat_us : 0;
}

/* fairness: two floods, ratio of handler CPU time vs weights. */
static void
kt_run_fairness(void)
{
	struct kt_flood *a, *b;
	int cpu, pcpu, pcpu_b;

	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	pcpu_b = kt_other_cpu(pcpu);
	if (pcpu_b == cpu) pcpu_b = pcpu;
	a = kt_floods[0] = kt_flood_create(0, "kta", kt_weight_a, 64, kt_cost_a, true, cpu, pcpu);
	b = kt_floods[1] = kt_flood_create(1, "ktb", kt_weight_b, 64, kt_cost_b, true, cpu, pcpu);
	if (a->q == NULL || b->q == NULL) return;
	kt_flood_start(a); kt_flood_start(b);
	pause("ktrun", kt_secs * hz);
	kt_flood_stop(a); kt_flood_stop(b);
	kt_floods_report(a, b);
	if (a->cycles == 0 || b->cycles == 0) {
		kt_fail("a flood got no service");
		return;
	}
	{
		/* ratio x 1000, expected weight_a/weight_b */
		uint64_t r = a->cycles * 1000 / b->cycles;
		uint64_t want = (uint64_t)kt_weight_a * 1000 / kt_weight_b;
		uint64_t tol = kt_weight_a == kt_weight_b ? 50 : 100;
		if (r > want + want * tol / 1000 || r + want * tol / 1000 < want)
			kt_fail("fairness ratio %ju.%03ju, expected %u:%u",
			    (uintmax_t)r / 1000, (uintmax_t)r % 1000, kt_weight_a, kt_weight_b);
	}
}

/* latency: a heavy flood (a) and a light 1 kHz queue (b) stamped at enqueue. */
static struct callout kt_lat_co;
static struct kt_flood *kt_lat_light;
static volatile int kt_lat_stop;

static void
kt_lat_tick(void *arg)
{
	struct kt_flood *f = arg;
	struct kwq_item *it;
	struct titem *ti;

	if (kt_lat_stop)
		return;
	mtx_lock(&f->mtx);
	it = STAILQ_FIRST(&f->free);
	if (it != NULL) {
		STAILQ_REMOVE_HEAD(&f->free, kwi_link);
		f->nfree--;
	}
	mtx_unlock(&f->mtx);
	if (it != NULL) {
		KWQ_ITEM_INIT(it);
		ti = __containerof(it, struct titem, ti_item);
		ti->ti_seq = (uint64_t)sbinuptime();
		if (kwq_enqueue(f->q, f->cpu, it) != 0)
			kt_flood_return(f, it);
		else
			f->produced++;
	}
	callout_schedule_sbt(&kt_lat_co, SBT_1MS, SBT_1US * 50, 0);
}

static void
kt_run_latency(void)
{
	struct kt_flood *a, *b;
	int cpu, pcpu;

	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	a = kt_floods[0] = kt_flood_create(0, "kta", 1, 256, kt_cost_a, true, cpu, pcpu);
	b = kt_floods[1] = kt_flood_create(1, "ktb", 1, 16, 5, true, cpu, pcpu);
	if (a->q == NULL || b->q == NULL) return;
	a->stamp = true; b->stamp = true; b->mode = F_NONE;
	kt_lat_light = b; kt_lat_stop = 0;
	callout_init(&kt_lat_co, 1);
	kt_flood_start(a);
	callout_reset_sbt_on(&kt_lat_co, SBT_1MS, SBT_1US * 50, kt_lat_tick, b, pcpu, 0);
	pause("ktrun", kt_secs * hz);
	kt_lat_stop = 1;
	callout_drain(&kt_lat_co);
	kt_flood_stop(a);
	kt_floods_report(a, b);
	kt_r_runs = b->produced;
}

/* gaming: flood a vs burst-and-pause b. */
static void
kt_run_gaming(void)
{
	struct kt_flood *a, *b;
	int cpu, pcpu, pcpu_b;

	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	pcpu_b = kt_other_cpu(pcpu);
	if (pcpu_b == cpu) pcpu_b = pcpu;
	a = kt_floods[0] = kt_flood_create(0, "kta", 1, 256, 20, true, cpu, pcpu);
	b = kt_floods[1] = kt_flood_create(1, "ktb", 1, 32, 20, true, cpu, pcpu);
	if (a->q == NULL || b->q == NULL) return;
	b->mode = F_BURSTPAUSE; b->burst = 32; b->pause_us = 50;
	kt_flood_start(a); kt_flood_start(b);
	pause("ktrun", kt_secs * hz);
	kt_flood_stop(a); kt_flood_stop(b);
	kt_floods_report(a, b);
	if (b->cycles > a->cycles + a->cycles / 50)
		kt_fail("gaming queue got more than the flood");
}

/* overrun: cooperative flood a vs budget-ignoring 2 ms items b. */
static void
kt_run_overrun(void)
{
	struct kt_flood *a, *b;
	int cpu, pcpu, pcpu_b;

	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	pcpu_b = kt_other_cpu(pcpu);
	if (pcpu_b == cpu) pcpu_b = pcpu;
	a = kt_floods[0] = kt_flood_create(0, "kta", 1, 256, 20, true, cpu, pcpu);
	b = kt_floods[1] = kt_flood_create(1, "ktb", 1, 4, 2000, false, cpu, pcpu);
	if (a->q == NULL || b->q == NULL) return;
	kt_flood_start(a); kt_flood_start(b);
	pause("ktrun", kt_secs * hz);
	kt_flood_stop(a); kt_flood_stop(b);
	kt_floods_report(a, b);
	if (b->cycles > a->cycles * 3 / 2)
		kt_fail("overrunner share %ju/1000 exceeds 1.5",
		    (uintmax_t)(b->cycles * 1000 / (a->cycles ? a->cycles : 1)));
}

/*
 * yield: a saturating flood on the target CPU, a 1 kHz callout pinned there.
 * With items=0 the flood is not started: the callout alone then measures
 * the timer and hypervisor noise the loaded run has to be compared with.
 */
static struct callout kt_yield_co;
static volatile uint64_t kt_yield_fires, kt_yield_late_max_us;
static volatile uint64_t kt_yield_late_at_us, kt_yield_stalls;
static volatile int kt_yield_late_cpu;
static sbintime_t kt_yield_deadline, kt_yield_t0;
static volatile int kt_yield_stop;

/*
 * Absolute 1 ms deadlines: a late fire does not delay the next one, so
 * the count measures lost fires and the max lateness measures B7.
 */
static void
kt_yield_tick(void *arg __unused)
{
	sbintime_t now;

	if (kt_yield_stop)
		return;
	now = sbinuptime();
	kt_yield_fires++;
	if (now > kt_yield_deadline) {
		uint64_t late = kt_sbt2us(now - kt_yield_deadline);
		if (late > 2000)
			kt_yield_stalls++;
		if (late > kt_yield_late_max_us) {
			kt_yield_late_max_us = late;
			kt_yield_late_at_us = kt_sbt2us(now - kt_yield_t0);
			kt_yield_late_cpu = curcpu;
		}
	}
	kt_yield_deadline += SBT_1MS;
	callout_reset_sbt_on(&kt_yield_co, kt_yield_deadline, SBT_1US * 50,
	    kt_yield_tick, NULL, kt_cpu >= 0 ? kt_cpu : 1, C_ABSOLUTE);
}

static void
kt_run_yield(void)
{
	struct kt_flood *a;
	sbintime_t t0;
	int cpu, pcpu;

	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	a = kt_floods[0] = kt_flood_create(0, "kta", 1, 256, 20, true, cpu, pcpu);
	if (a->q == NULL) return;
	kt_yield_fires = 0; kt_yield_stop = 0; kt_yield_late_max_us = 0;
	kt_yield_late_at_us = 0; kt_yield_stalls = 0; kt_yield_late_cpu = -1;
	callout_init(&kt_yield_co, 1);
	kt_yield_t0 = sbinuptime();
	kt_yield_deadline = kt_yield_t0 + SBT_1MS;
	callout_reset_sbt_on(&kt_yield_co, kt_yield_deadline, SBT_1US * 50,
	    kt_yield_tick, NULL, cpu, C_ABSOLUTE);
	if (kt_items > 0)	/* items=0: callout only, the VM-noise baseline */
		kt_flood_start(a);
	t0 = sbinuptime();
	pause("ktrun", kt_secs * hz);
	kt_r_ns = kt_sbt2us(sbinuptime() - t0) * 1000;	/* the run's wall time */
	kt_yield_stop = 1;
	callout_drain(&kt_yield_co);
	if (kt_items > 0)
		kt_flood_stop(a);
	kt_floods_report(a, NULL);
	kt_r_runs = kt_yield_fires;
	kt_r_maxlat_b_us = kt_yield_late_max_us;	/* B7: callout lateness */
	/* Where and when the worst fire happened, for the log. */
	kt_r_fifo_errors = kt_yield_stalls;		/* fires > 2 ms late */
	kt_r_cycles_b = kt_yield_late_at_us;		/* us since start */
	kt_r_cycles_a = kt_yield_late_cpu;		/* CPU that ran it */
	if (kt_yield_fires < (uint64_t)kt_secs * 1000 * 99 / 100)
		kt_fail("callout fired %ju times in %u s (expected ~%u)",
		    (uintmax_t)kt_yield_fires, kt_secs, kt_secs * 1000);
	/* one tick plus one pass: 10 ms at hz=100 plus a 2Q pass, plus VM noise */
	if (kt_yield_late_max_us > 1000000 / hz + 2000)
		kt_fail("callout late by %ju us (bound: one tick + one pass)",
		    (uintmax_t)kt_yield_late_max_us);
}

/* cost: one queue, empty handler, producer on another CPU. */
static void
kt_run_cost(void)
{
	struct kt_flood *a;
	sbintime_t t0;
	int cpu, pcpu;

	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	a = kt_floods[0] = kt_flood_create(0, "kta", 1, kt_items ? kt_items : 1024, 0, true, cpu, pcpu);
	if (a->q == NULL) return;
	a->empty_handler = true;
	t0 = sbinuptime();
	kt_flood_start(a);
	pause("ktrun", kt_secs * hz);
	kt_flood_stop(a);
	kt_floods_report(a, NULL);
	kt_r_ns_per_item = a->items_out ? kt_sbt2us(sbinuptime() - t0) * 1000 / a->items_out : 0;
	kt_r_prod_ns_per_item = a->produced ?
	    (a->prod_ticks * 1000000000ULL / kt_tick_hz) / a->produced : 0;
}

/*
 * burst (SPEC-p4e 2.1 and, through burst_mode 1 and 2, the conformance rows
 * of 2.4): one queue on cpu, one producer on pcpu (prod_prio applies), the
 * arrival process set by burst_mode, burst_items and burst_gap_us.  Reads:
 * result_bursts, the first-item and last-item latency percentiles,
 * result_runs (passes), result_in/out, result_ns_per_item (wall per item),
 * result_prod_ns_per_item (the enqueue).  The worker's wakeups, ticks and
 * the rest are kwq's own counters, read by the runner.
 */
static void
kt_run_burst(void)
{
	struct kt_flood *a;
	sbintime_t t0;
	uint64_t wall_us;
	u_int npool;
	int cpu, pcpu;

	if (kt_burst_mode > 2 || kt_burst_items == 0) {
		kt_fail("burst: burst_mode 0..2, burst_items >= 1");
		return;
	}
	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	npool = 4 * kt_burst_items;
	if (npool < 256) npool = 256;
	if (kt_items > npool) npool = kt_items;
	memset(kt_hist_first, 0, sizeof(kt_hist_first));
	memset(kt_hist_last, 0, sizeof(kt_hist_last));
	kt_hist_first_n = kt_hist_last_n = kt_hist_first_max = kt_hist_last_max = 0;
	a = kt_floods[0] = kt_flood_create(0, "kta", 1, npool, 0, true, cpu, pcpu);
	if (a->q == NULL) return;
	a->mode = F_BURST;
	a->stamp = true;
	a->burst = kt_burst_items;
	a->burst_mode = kt_burst_mode;
	a->burst_gap_ns = (uint64_t)kt_burst_gap_us * 1000;
	a->prod_spin = kt_ns2ticks(kt_prod_ns);
	a->cons_spin = kt_ns2ticks(kt_cons_ns);
	t0 = sbinuptime();
	kt_flood_start(a);
	pause("ktrun", kt_secs * hz);
	kt_flood_stop(a);
	wall_us = kt_sbt2us(sbinuptime() - t0);
	kt_floods_report(a, NULL);
	kt_r_runs = a->passes;
	kt_r_bursts = a->bursts;
	kt_r_first_p50_us = kt_hist_pct(kt_hist_first, kt_hist_first_n, 50);
	kt_r_first_p99_us = kt_hist_pct(kt_hist_first, kt_hist_first_n, 99);
	kt_r_first_max_us = (kt_hist_first_max + 999) / 1000;
	kt_r_last_p50_us = kt_hist_pct(kt_hist_last, kt_hist_last_n, 50);
	kt_r_last_p99_us = kt_hist_pct(kt_hist_last, kt_hist_last_n, 99);
	kt_r_last_max_us = (kt_hist_last_max + 999) / 1000;
	kt_r_maxlat_a_us = kt_r_first_max_us;
	kt_r_maxlat_b_us = kt_r_last_max_us;
	kt_r_ns_per_item = a->items_out ? wall_us * 1000 / a->items_out : 0;
	kt_r_prod_ns_per_item = a->produced ?
	    (a->prod_ticks * 1000000000ULL / kt_tick_hz) / a->produced : 0;
	snprintf(kt_msg, sizeof(kt_msg), "burst mode %u, %u items, gap %u us, spin %u/%u ns: "
	    "%ju bursts, %ju items, %ju passes (%ju items/pass), %ju bursts/s; "
	    "first-item latency p50 %ju p99 %ju max %ju us, last-item p50 %ju p99 %ju max %ju us; %ju rejects",
	    kt_burst_mode, kt_burst_items, kt_burst_gap_us, kt_prod_ns, kt_cons_ns,
	    (uintmax_t)a->bursts, (uintmax_t)a->items_out, (uintmax_t)a->passes,
	    (uintmax_t)(a->passes ? a->items_out / a->passes : 0),
	    (uintmax_t)(wall_us ? a->bursts * 1000000 / wall_us : 0),
	    (uintmax_t)kt_r_first_p50_us, (uintmax_t)kt_r_first_p99_us, (uintmax_t)kt_r_first_max_us,
	    (uintmax_t)kt_r_last_p50_us, (uintmax_t)kt_r_last_p99_us, (uintmax_t)kt_r_last_max_us,
	    (uintmax_t)a->rejected);
}

/*
 * intr_load (SPEC-p4e 2.2): interrupt context on a CPU beside any scenario.
 * A direct callout on intr_cpu re-arms itself every intr_us and busy-waits
 * intr_work_ns with DELAY() in the timer interrupt, for intr_secs; the
 * duty cycle is refused above 50 %.  Writing intr_secs > 0 starts it, 0
 * stops it; it stops itself at the end and at unload.  Independent of the
 * run state machine, so it can straddle scenarios.
 */
static struct callout	kt_intr_co;
static int	kt_intr_cpu = -1;
static u_int	kt_intr_us = 100;
static u_int	kt_intr_work_ns = 20000;
static u_int	kt_intr_secs_knob;
static volatile bool kt_intr_on;
static sbintime_t kt_intr_end, kt_intr_next, kt_intr_period;
static u_int	kt_intr_work_us;	/* DELAY() argument, rounded up */

SYSCTL_INT(_kern_kwq_test, OID_AUTO, intr_cpu, CTLFLAG_RW, &kt_intr_cpu, 0,
    "intr_load: the CPU the interrupt load runs on (-1 = none)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, intr_us, CTLFLAG_RW, &kt_intr_us, 0,
    "intr_load: period of the interrupt load, us");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, intr_work_ns, CTLFLAG_RW, &kt_intr_work_ns, 0,
    "intr_load: busy time per interrupt, ns (at most 200000; duty cycle at most 50 %)");

static void
kt_intr_handler(void *arg __unused)
{
	sbintime_t now;

	if (!kt_intr_on)
		return;
	now = sbinuptime();
	if (now >= kt_intr_end) {
		kt_intr_on = false;
		return;
	}
	kt_r_intr_fired++;
	if (now > kt_intr_next + kt_intr_period)
		kt_r_intr_late++;
	DELAY(kt_intr_work_us);
	kt_r_intr_work_ns += kt_intr_work_us * 1000;
	do
		kt_intr_next += kt_intr_period;
	while (kt_intr_next <= now);
	callout_reset_sbt_on(&kt_intr_co, kt_intr_next, kt_intr_period / 8,
	    kt_intr_handler, NULL, kt_intr_cpu, C_ABSOLUTE | C_DIRECT_EXEC);
}

static void
kt_intr_stop(void)
{
	kt_intr_on = false;
	callout_drain(&kt_intr_co);
}

static int
kt_sysctl_intr_secs(SYSCTL_HANDLER_ARGS)
{
	u_int v;
	int error;

	v = kt_intr_secs_knob;
	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v == 0) {
		kt_intr_stop();
		kt_intr_secs_knob = 0;
		return (0);
	}
	if (kt_intr_on)
		return (EBUSY);
	if (kt_intr_cpu < 0 || kt_intr_cpu > (int)mp_maxid || CPU_ABSENT(kt_intr_cpu) ||
	    kt_intr_us == 0 || kt_intr_work_ns == 0 || kt_intr_work_ns > 200000 ||
	    kt_intr_work_ns * 2 > (uint64_t)kt_intr_us * 1000)
		return (EINVAL);
	kt_intr_secs_knob = v;
	kt_r_intr_fired = kt_r_intr_late = kt_r_intr_work_ns = 0;
	kt_intr_work_us = (kt_intr_work_ns + 999) / 1000;
	kt_intr_period = (sbintime_t)kt_intr_us * SBT_1US;
	kt_intr_next = sbinuptime() + kt_intr_period;
	kt_intr_end = sbinuptime() + (sbintime_t)v * SBT_1S;
	kt_intr_on = true;
	callout_reset_sbt_on(&kt_intr_co, kt_intr_next, kt_intr_period / 8,
	    kt_intr_handler, NULL, kt_intr_cpu, C_ABSOLUTE | C_DIRECT_EXEC);
	return (0);
}
SYSCTL_PROC(_kern_kwq_test, OID_AUTO, intr_secs,
    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, kt_sysctl_intr_secs, "IU",
    "intr_load: write the seconds to run (starts it: intr_cpu, intr_us, intr_work_ns), 0 stops it");

/* tq_baseline: the same producer into a taskqueue with epair's shape. */
static struct taskqueue *kt_tq;
static struct task kt_tq_task;
static struct mtx kt_tq_mtx;
static STAILQ_HEAD(, kwq_item) kt_tq_list = STAILQ_HEAD_INITIALIZER(kt_tq_list);
static bool kt_tq_scheduled;
static struct kt_flood *kt_tq_flood;

static void
kt_tq_run(void *arg __unused, int pending __unused)
{
	struct kwq_item *it, *next, *head;
	struct kt_flood *f = kt_tq_flood;
	int n = 0;

	mtx_lock(&kt_tq_mtx);
	head = STAILQ_FIRST(&kt_tq_list);
	STAILQ_INIT(&kt_tq_list);
	kt_tq_scheduled = false;
	mtx_unlock(&kt_tq_mtx);
	for (it = head; it != NULL; it = next) {
		next = STAILQ_NEXT(it, kwi_link);
		KWQ_ITEM_INIT(it);
		kt_flood_return(f, it);
		n++;
	}
	f->items_out += n;
}

static void
kt_tq_producer(void *arg)
{
	struct kt_flood *f = arg;
	struct kwq_item *it;
	uint64_t t0;
	bool sched;

	kt_prod_bind(f->pcpu);
	while (!f->stop) {
		mtx_lock(&f->mtx);
		it = STAILQ_FIRST(&f->free);
		if (it == NULL) {
			kt_pool_wait(f);	/* releases f->mtx */
			continue;
		}
		STAILQ_REMOVE_HEAD(&f->free, kwi_link);
		f->nfree--;
		mtx_unlock(&f->mtx);
		KWQ_ITEM_INIT(it);
		t0 = kt_ticks();
		mtx_lock(&kt_tq_mtx);
		STAILQ_INSERT_TAIL(&kt_tq_list, it, kwi_link);
		sched = !kt_tq_scheduled;
		kt_tq_scheduled = true;
		mtx_unlock(&kt_tq_mtx);
		if (sched)
			taskqueue_enqueue(kt_tq, &kt_tq_task);
		f->prod_ticks += kt_tickdelta(t0);
		f->produced++;
	}
	f->exited = true;
	wakeup(f);
	kthread_exit();
}

static void
kt_run_tq_baseline(void)
{
	struct kt_flood *f;
	cpuset_t mask;
	sbintime_t t0;
	int cpu, pcpu;

	cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(cpu)) cpu = 0;
	pcpu = kt_other_cpu(cpu);
	f = malloc(sizeof(*f), M_KWQ_TEST, M_WAITOK | M_ZERO);
	f->idx = 0; f->cpu = cpu; f->pcpu = pcpu; f->npool = kt_items ? kt_items : 1024;
	mtx_init(&f->mtx, "kwq_test tq flood", NULL, MTX_DEF);
	STAILQ_INIT(&f->free);
	f->items = malloc(sizeof(*f->items) * f->npool, M_KWQ_TEST, M_WAITOK | M_ZERO);
	for (u_int i = 0; i < f->npool; i++) {
		f->items[i].ti_magic = TITEM_MAGIC;
		KWQ_ITEM_INIT(&f->items[i].ti_item);
		STAILQ_INSERT_TAIL(&f->free, &f->items[i].ti_item, kwi_link);
	}
	f->nfree = f->npool;
	kt_tq_flood = f;
	mtx_init(&kt_tq_mtx, "kwq_test tq", NULL, MTX_DEF);
	TASK_INIT(&kt_tq_task, 0, kt_tq_run, NULL);
	kt_tq = taskqueue_create("kwq_test_tq", M_WAITOK, taskqueue_thread_enqueue, &kt_tq);
	CPU_SETOF(cpu, &mask);
	taskqueue_start_threads_cpuset(&kt_tq, 1, PI_NET, &mask, "kwq_test_tq");
	t0 = sbinuptime();
	if (kthread_add(kt_tq_producer, f, NULL, &f->td, 0, 0, "kwq_test_tqprod") != 0) {
		kt_fail("producer thread"); f->exited = true;
	}
	pause("ktrun", kt_secs * hz);
	kt_flood_stop(f);
	taskqueue_drain(kt_tq, &kt_tq_task);
	kt_r_in = f->produced; kt_r_out = f->items_out;
	kt_r_ns_per_item = f->items_out ? kt_sbt2us(sbinuptime() - t0) * 1000 / f->items_out : 0;
	kt_r_prod_ns_per_item = f->produced ?
	    (f->prod_ticks * 1000000000ULL / kt_tick_hz) / f->produced : 0;
	taskqueue_free(kt_tq);
	kt_tq = NULL;
	mtx_destroy(&kt_tq_mtx);
	mtx_destroy(&f->mtx);
	free(f->items, M_KWQ_TEST);
	free(f, M_KWQ_TEST);
}

/* switch_baseline: two threads on one CPU passing a token with sleep/wakeup. */
static struct mtx kt_sw_mtx;
static int kt_sw_token;
static volatile bool kt_sw_stop;
static uint64_t kt_sw_rounds;
static volatile int kt_sw_exited;

static void
kt_sw_thread(void *arg)
{
	int me = (int)(intptr_t)arg;

	thread_lock(curthread);
	sched_bind(curthread, kt_cpu >= 0 ? kt_cpu : 1);
	thread_unlock(curthread);
	mtx_lock(&kt_sw_mtx);
	while (!kt_sw_stop) {
		while (kt_sw_token != me && !kt_sw_stop)
			msleep(&kt_sw_token, &kt_sw_mtx, 0, "ktsw", 0);
		if (kt_sw_stop)
			break;
		kt_sw_token = 1 - me;
		if (me == 0)
			kt_sw_rounds++;
		wakeup_one(&kt_sw_token);
	}
	atomic_add_int(&kt_sw_exited, 1);
	wakeup(__DEVOLATILE(void *, &kt_sw_exited));
	mtx_unlock(&kt_sw_mtx);
	kthread_exit();
}

static void
kt_run_switch_baseline(void)
{
	struct thread *td;
	sbintime_t t0;

	mtx_init(&kt_sw_mtx, "kwq_test sw", NULL, MTX_DEF);
	kt_sw_token = 0; kt_sw_stop = false; kt_sw_rounds = 0; kt_sw_exited = 0;
	t0 = sbinuptime();
	kthread_add(kt_sw_thread, (void *)(intptr_t)0, NULL, &td, 0, 0, "kwq_test_sw0");
	kthread_add(kt_sw_thread, (void *)(intptr_t)1, NULL, &td, 0, 0, "kwq_test_sw1");
	pause("ktrun", kt_secs * hz);
	mtx_lock(&kt_sw_mtx);
	kt_sw_stop = true;
	wakeup(&kt_sw_token);
	while (kt_sw_exited < 2)
		msleep(__DEVOLATILE(void *, &kt_sw_exited), &kt_sw_mtx, 0, "ktswx", hz);
	mtx_unlock(&kt_sw_mtx);
	kt_r_runs = kt_sw_rounds;
	/* two switches per round trip */
	kt_r_ns_per_item = kt_sw_rounds ? kt_sbt2us(sbinuptime() - t0) * 1000 / (2 * kt_sw_rounds) : 0;
	mtx_destroy(&kt_sw_mtx);
}

/*
 * pingpong, pingpong_tq: one item handed back and forth.  The producer,
 * bound to another CPU, enqueues the item and sleeps; the handler wakes it;
 * repeat.  Every enqueue therefore finds the worker asleep and pays the
 * whole wake path, which the flood scenarios amortise over a batch.  The
 * reply (a wakeup_one() of the producer) is the same in both variants, so
 * the difference between them is the request path: kwq_enqueue() with its
 * doorbell and the worker's way to the handler, against a taskqueue's
 * list append, taskqueue_enqueue() and the thread's loop.  Measured
 * in-kernel with kt_ticks(), no probes: wall ns per round trip
 * (result_ns_per_item), the producer's ns inside the enqueue call
 * (result_prod_ns_per_item), and the enqueue-to-handler latency from an
 * sbinuptime() stamp (result_lat_ns average, result_maxlat_a_us worst).
 */
struct kt_pp {
	struct kwq	*q;
	struct taskqueue *tq;
	struct task	task;
	struct titem	item;
	int		cpu, pcpu;
	volatile bool	stop, exited;
	struct thread	*td;
	/* Shared under mtx. */
	struct mtx	mtx __aligned(KT_LINE);
	volatile int	in_flight;
	/* Written by the producer. */
	uint64_t	rounds __aligned(KT_LINE);
	uint64_t	prod_ticks;
	sbintime_t	stamp;
	/* Written by the handler. */
	uint64_t	lat_sum __aligned(KT_LINE);	/* sbintime units */
	uint64_t	lat_max;
	uint64_t	runs;
};

static struct kt_pp *kt_pp;

static void
kt_pp_reply(struct kt_pp *pp)
{
	sbintime_t d = sbinuptime() - pp->stamp;

	if (d > 0) {
		pp->lat_sum += (uint64_t)d;
		if ((uint64_t)d > pp->lat_max)
			pp->lat_max = (uint64_t)d;
	}
	pp->runs++;
	mtx_lock(&pp->mtx);
	pp->in_flight = 0;
	wakeup_one(__DEVOLATILE(void *, &pp->in_flight));
	mtx_unlock(&pp->mtx);
}

static void
kt_pp_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
{
	struct kt_pp *pp = ctx;
	struct kwq_item *it, *next;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		KWQ_ITEM_INIT(it);
	}
	if (n > 0)
		kt_pp_reply(pp);
}

static void
kt_pp_task(void *ctx, int pending __unused)
{
	kt_pp_reply(ctx);
}

static void
kt_pp_producer(void *arg)
{
	struct kt_pp *pp = arg;
	uint64_t t0;

	kt_prod_bind(pp->pcpu);
	while (!pp->stop) {
		pp->in_flight = 1;
		KWQ_ITEM_INIT(&pp->item.ti_item);
		pp->stamp = sbinuptime();
		t0 = kt_ticks();
		if (pp->tq != NULL)
			taskqueue_enqueue(pp->tq, &pp->task);
		else if (kwq_enqueue(pp->q, pp->cpu, &pp->item.ti_item) != 0) {
			pp->in_flight = 0;
			continue;
		}
		pp->prod_ticks += kt_tickdelta(t0);
		mtx_lock(&pp->mtx);
		while (pp->in_flight && !pp->stop)
			msleep(__DEVOLATILE(void *, &pp->in_flight), &pp->mtx, 0, "ktpp", hz);
		mtx_unlock(&pp->mtx);
		pp->rounds++;
	}
	pp->exited = true;
	wakeup(pp);
	kthread_exit();
}

static void
kt_run_pingpong(bool use_tq)
{
	struct kt_pp *pp;
	cpuset_t mask;
	sbintime_t t0;

	pp = malloc(sizeof(*pp), M_KWQ_TEST, M_WAITOK | M_ZERO);
	kt_pp = pp;
	pp->cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(pp->cpu)) pp->cpu = 0;
	pp->pcpu = kt_other_cpu(pp->cpu);
	pp->item.ti_magic = TITEM_MAGIC;
	KWQ_ITEM_INIT(&pp->item.ti_item);
	mtx_init(&pp->mtx, "kwq_test pp", NULL, MTX_DEF);
	if (use_tq) {
		TASK_INIT(&pp->task, 0, kt_pp_task, pp);
		pp->tq = taskqueue_create("kwq_test_pp", M_WAITOK, taskqueue_thread_enqueue, &pp->tq);
		CPU_SETOF(pp->cpu, &mask);
		taskqueue_start_threads_cpuset(&pp->tq, 1, PI_NET, &mask, "kwq_test_pp");
	} else {
		pp->q = kt_create("kta", KWQ_F_DISCARD, 1, kt_pp_handler, pp);
		if (pp->q == NULL) {
			kt_fail("kwq_create(kta) failed");
			goto out;
		}
		kwq_activate(pp->q);
	}
	t0 = sbinuptime();
	if (kthread_add(kt_pp_producer, pp, NULL, &pp->td, 0, 0, "kwq_test_pp") != 0) {
		kt_fail("producer thread");
		goto out;
	}
	pause("ktrun", kt_secs * hz);
	pp->stop = true;
	mtx_lock(&pp->mtx);
	wakeup(__DEVOLATILE(void *, &pp->in_flight));
	mtx_unlock(&pp->mtx);
	while (!pp->exited)
		tsleep(pp, 0, "ktpp", hz / 100);
	kt_r_in = kt_r_out = pp->rounds;
	kt_r_runs = pp->runs;
	kt_r_ns_per_item = pp->rounds ? kt_sbt2us(sbinuptime() - t0) * 1000 / pp->rounds : 0;
	kt_r_prod_ns_per_item = pp->rounds ?
	    (pp->prod_ticks * 1000000000ULL / kt_tick_hz) / pp->rounds : 0;
	kt_r_lat_ns = pp->runs ? kt_sbt2us(pp->lat_sum / pp->runs * 1000) : 0;
	kt_r_maxlat_a_us = kt_sbt2us(pp->lat_max);
out:
	if (pp->tq != NULL) {
		taskqueue_drain(pp->tq, &pp->task);
		taskqueue_free(pp->tq);
	}
	if (pp->q != NULL) {
		kwq_drain(pp->q);
		kwq_destroy(pp->q);
	}
	mtx_destroy(&pp->mtx);
	free(pp, M_KWQ_TEST);
	kt_pp = NULL;
}

/*
 * clocks: what one read of each clock costs on this machine, and what one
 * kwq_budget_left() call costs under the current kern.kwq.net.
 * budget_check_every (1: a scheduler clock read per call; 64: the
 * estimate).  The raw reads run on this thread, bound to the producer CPU;
 * kwq_budget_left() runs where it must, inside a handler on the worker
 * CPU, timed with sbinuptime() around KT_CLK_BL calls that stay inside the
 * pass budget (weight 8), so that the estimate path is timed and not the
 * "budget spent, read the clock every call" path.  Results in
 * result_clocks as ns per call, nothing else is measured.
 */
#define	KT_CLK_N	1000000u
#define	KT_CLK_BATCH	100u	/* kwq_budget_left() calls per timed batch */
#define	KT_CLK_BATCHES	20u	/* 2000 calls x <= 150 ns stay inside a weight-8
				   budget (1.6 ms at Q = 200 us), so the estimate path
				   is what is timed when budget_check_every is large */


struct kt_clk {
	struct kwq	*q;
	struct titem	item;
	int		cpu, pcpu;
	volatile int	phase;		/* 0 cold pass pending, 1 warm pass pending, 2 done */
	uint64_t	bl_min, bl_med;	/* ns per kwq_budget_left() call: best and median batch */
	u_int		bl_zero;	/* calls that answered 0: the budget was spent, or a
					   clock glitch ended the pass (S13); the fast exit */
	uint64_t	h_ticks, h_cyc, h_sbin;	/* the raw reads on the worker CPU, best batch */
	uint64_t	cold_ns;	/* ns per kwq_budget_left() in the first batch, CPU fresh from idle */
};

/* Best of KT_CLK_BATCHES batches of KT_CLK_BATCH evaluations of expr, ns each. */
#define	KT_CLK_BEST(expr, out) do {						uint64_t _best = ~0ULL, _c0, _c1;					volatile uint64_t _sink = 0;						u_int _b, _i;								for (_b = 0; _b < KT_CLK_BATCHES; _b++) {					_c0 = get_cyclecount();							for (_i = 0; _i < KT_CLK_BATCH; _i++)						_sink += (uint64_t)(expr);					_c1 = get_cyclecount();							if (_c1 - _c0 < _best)								_best = _c1 - _c0;					}									(out) = _best * 1000000000ULL / kt_tick_hz / KT_CLK_BATCH;	} while (0)

static void
kt_clk_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
{
	struct kt_clk *c = ctx;
	struct kwq_item *it, *next;
	u_int i;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		KWQ_ITEM_INIT(it);
	}
	if (n <= 0)
		return;
	if (c->phase == 0) {
		/*
		 * First pass, on a CPU fresh from idle: a core coming out of a
		 * low-power state ran every instruction 2.7-3.5x slower for a
		 * while on both test machines (host DVFS at 816 MHz in the
		 * guest's case).  Keep one batch as "cold", then spin 20 ms to
		 * warm the CPU; the overrun only parks the queue for a few
		 * rounds, and the second pass measures the warm CPU.
		 */
		uint64_t c0 = get_cyclecount(), c1;
		sbintime_t s0;

		for (i = 0; i < KT_CLK_BATCH; i++)
			(void)kwq_budget_left(q);
		c1 = get_cyclecount();
		c->cold_ns = (c1 - c0) * 1000000000ULL / kt_tick_hz / KT_CLK_BATCH;
		s0 = sbinuptime();
		while (sbinuptime() - s0 < SBT_1MS * 20)
			cpu_spinwait();
		c->phase = 1;
		wakeup(c);
		return;
	}
	KT_CLK_BEST(cpu_ticks(), c->h_ticks);
	KT_CLK_BEST(get_cyclecount(), c->h_cyc);
	KT_CLK_BEST(sbinuptime(), c->h_sbin);
	/* Batches timed with get_cyclecount(); the best one is the cost without interference. */
	{
		uint64_t d[KT_CLK_BATCHES], c0, c1, tmp;
		u_int b, j;

		for (b = 0; b < KT_CLK_BATCHES; b++) {
			c0 = get_cyclecount();
			for (i = 0; i < KT_CLK_BATCH; i++)
				if (kwq_budget_left(q) == 0)
					c->bl_zero++;
			c1 = get_cyclecount();
			d[b] = c1 - c0;
		}
		for (b = 1; b < KT_CLK_BATCHES; b++)	/* insertion sort */
			for (j = b; j > 0 && d[j - 1] > d[j]; j--) {
				tmp = d[j]; d[j] = d[j - 1]; d[j - 1] = tmp;
			}
		c->bl_min = d[0] * 1000000000ULL / kt_tick_hz / KT_CLK_BATCH;
		c->bl_med = d[KT_CLK_BATCHES / 2] * 1000000000ULL / kt_tick_hz / KT_CLK_BATCH;
	}
	c->phase = 2;
	wakeup(c);
}

#define	KT_CLK_LOOP(expr, out) do {					\
	sbintime_t _t0, _t1;						\
	u_int _i;							\
	volatile uint64_t _sink = 0;					\
	_t0 = sbinuptime();						\
	for (_i = 0; _i < KT_CLK_N; _i++)				\
		_sink += (uint64_t)(expr);				\
	_t1 = sbinuptime();						\
	(out) = kt_sbt2us((_t1 - _t0) * 1000) / KT_CLK_N;		\
} while (0)

static void
kt_run_clocks(void)
{
	struct kt_clk *c;
	uint64_t t_ticks, t_cyc, t_sbin, t_getsbin, t_rate;
	int i;

	c = malloc(sizeof(*c), M_KWQ_TEST, M_WAITOK | M_ZERO);
	c->cpu = kt_cpu >= 0 ? kt_cpu : 1;
	if (CPU_ABSENT(c->cpu)) c->cpu = 0;
	c->pcpu = kt_other_cpu(c->cpu);
	thread_lock(curthread);
	sched_bind(curthread, c->pcpu);
	thread_unlock(curthread);
	KT_CLK_LOOP(cpu_ticks(), t_ticks);
	KT_CLK_LOOP(get_cyclecount(), t_cyc);
	KT_CLK_LOOP(sbinuptime(), t_sbin);
	KT_CLK_LOOP(getsbinuptime(), t_getsbin);
	KT_CLK_LOOP(cpu_tickrate(), t_rate);
	thread_lock(curthread);
	sched_unbind(curthread);
	thread_unlock(curthread);

	c->item.ti_magic = TITEM_MAGIC;
	KWQ_ITEM_INIT(&c->item.ti_item);
	c->q = kt_create("kta", KWQ_F_DISCARD, 8, kt_clk_handler, c);
	if (c->q == NULL) {
		kt_fail("kwq_create(kta) failed");
		free(c, M_KWQ_TEST);
		return;
	}
	kwq_activate(c->q);
	if (kwq_enqueue(c->q, c->cpu, &c->item.ti_item) != 0)
		kt_fail("enqueue failed");
	for (i = 0; i < 100 && c->phase < 1 && kt_state[0] != 'f'; i++)
		tsleep(c, 0, "ktclk", hz / 10);
	if (c->phase < 1 && kt_state[0] != 'f')
		kt_fail("cold pass did not run");
	else if (kt_state[0] != 'f') {
		KWQ_ITEM_INIT(&c->item.ti_item);
		if (kwq_enqueue(c->q, c->cpu, &c->item.ti_item) != 0)
			kt_fail("second enqueue failed");
		for (i = 0; i < 100 && c->phase < 2 && kt_state[0] != 'f'; i++)
			tsleep(c, 0, "ktclk", hz / 10);
		if (c->phase < 2 && kt_state[0] != 'f')
			kt_fail("warm pass did not run");
	}
	kwq_drain(c->q);
	kwq_destroy(c->q);
	snprintf(kt_clocks, sizeof(kt_clocks),
	    "cpu_ticks %ju get_cyclecount %ju sbinuptime %ju getsbinuptime %ju cpu_tickrate %ju | worker cpu warm: cpu_ticks %ju get_cyclecount %ju sbinuptime %ju kwq_budget_left %ju (median %ju, cold %ju, %u of %u calls answered 0)",
	    (uintmax_t)t_ticks, (uintmax_t)t_cyc, (uintmax_t)t_sbin, (uintmax_t)t_getsbin,
	    (uintmax_t)t_rate, (uintmax_t)c->h_ticks, (uintmax_t)c->h_cyc, (uintmax_t)c->h_sbin,
	    (uintmax_t)c->bl_min, (uintmax_t)c->bl_med, (uintmax_t)c->cold_ns, c->bl_zero,
	    KT_CLK_BATCH * KT_CLK_BATCHES);
	kt_r_ns_per_item = c->bl_min;
	free(c, M_KWQ_TEST);
}

/* ================================================================ runner */

/* ================================================================ mix */

/*
 * Notifiers and items on one queue.  Do they interleave in arrival order,
 * are notifiers budgeted, does a self-re-arming notifier starve the items
 * (SCHED.md S14 case 4), and do re-arm, cancel, requeue and drain keep
 * every notifier exactly-once?  Every scenario here runs one queue on one
 * worker CPU with the producer (this thread) bound to another CPU, and
 * builds exact lists behind a GATE: an item whose handler spins until
 * released, so that the next pass swaps out precisely what was built in
 * the meantime.  The handler reads only the run struct, never a knob
 * (the 2026-09-29 lesson); a delivery log written by the worker CPU alone
 * records the order the handler saw.
 */
#define	KT_MIX_NNF		64
#define	KT_MIX_LOG		16384
#define	KT_MIX_POOL		1024
#define	KT_MIX_GATE_MAGIC	0x6761746531323334ULL

struct kt_mixlog { u_int kind; u_int idx; };	/* 0 item, 1 notifier, 2 gate */

struct kt_mix {
	struct kwq	*q;
	int		wcpu, pcpu;
	struct kwq_notifier nf[KT_MIX_NNF];
	struct titem	*items;
	u_int		nitems;
	struct titem	gate;
	/* Behaviour, fixed before the run; the handler never reads a knob. */
	uint64_t	item_spin, nf_spin;	/* cpu_ticks */
	u_int		stop_after;	/* elements per call before a requeue; 0 = by budget */
	bool		coop;		/* honour kwq_budget_left() */
	/* Control flags the producer flips during a run (not knobs). */
	volatile int	rearm;		/* nf[0]'s handler re-arms nf[0] */
	volatile int	gate_hold, in_gate;
	/* Written by the handler, i.e. by the worker CPU only. */
	struct kt_mixlog *log __aligned(KT_LINE);
	u_int		nlog;
	uint64_t	calls, nf_runs, item_runs, discarded, requeues;
	uint64_t	level_seen;
	volatile int	in_nf;		/* nf[0]'s handler is running */
	sbintime_t	nf_exit;	/* sbinuptime() when it last returned */
	/* Written by the producer. */
	volatile uint64_t level __aligned(KT_LINE);
};

static void
kt_mix_log(struct kt_mix *m, u_int kind, u_int idx)
{
	if (m->nlog < KT_MIX_LOG) {
		m->log[m->nlog].kind = kind;
		m->log[m->nlog].idx = idx;
		m->nlog++;
	}
}

static void
kt_mix_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
{
	struct kt_mix *m = ctx;
	struct kwq_item *it, *next;
	struct titem *ti;
	uintptr_t off;
	uint64_t t0, lim;
	u_int done = 0, idx;
	int left = n < 0 ? -n : n;

	m->calls++;
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		off = (uintptr_t)it - (uintptr_t)m->nf;
		if (off < sizeof(m->nf) &&
		    off % sizeof(struct kwq_notifier) == 0) {
			idx = off / sizeof(struct kwq_notifier);
			kt_mix_log(m, 1, idx);
			if (n < 0) {
				m->discarded++;
			} else {
				m->nf_runs++;
				if (idx == 0)
					m->in_nf = 1;
				if (m->nf_spin != 0)
					kt_spin_ticks(m->nf_spin);
				/* Level semantics: read the state, not "one event". */
				m->level_seen = m->level;
				if (idx == 0) {
					if (m->rearm)
						kwq_notify(q, &m->nf[0]);
					m->nf_exit = sbinuptime();
					m->in_nf = 0;
				}
			}
		} else {
			KWQ_ITEM_INIT(it);
			ti = __containerof(it, struct titem, ti_item);
			if (ti->ti_magic == KT_MIX_GATE_MAGIC) {
				kt_mix_log(m, 2, 0);
				if (n > 0) {
					/* Hold this pass until released, 100 ms at most. */
					t0 = kt_ticks();
					lim = kt_ns2ticks(100000000ULL);
					m->in_gate = 1;
					while (m->gate_hold && kt_ticks() - t0 < lim)
						cpu_spinwait();
					m->in_gate = 0;
				}
			} else {
				if (ti->ti_magic != TITEM_MAGIC)
					panic("kwq_test: mix: bad item %p", ti);
				kt_mix_log(m, 0, (u_int)(ti - m->items));
				if (n < 0) {
					m->discarded++;
				} else {
					m->item_runs++;
					if (m->item_spin != 0)
						kt_spin_ticks(m->item_spin);
				}
				atomic_store_rel_int(&ti->ti_seen, 0);	/* back to the pool */
			}
		}
		left--;
		done++;
		if (n > 0 && next != NULL &&
		    (m->stop_after != 0 ? done >= m->stop_after :
		    (m->coop && kwq_budget_left(q) == 0))) {
			m->requeues++;
			kwq_requeue(q, next, NULL, left);
			return;
		}
	}
}

static struct kt_mix *
kt_mix_init(uint32_t flags)
{
	struct kt_mix *m;
	u_int i;
	int pcpu;

	pcpu = kt_cpu >= 0 ? kt_other_cpu(kt_cpu) : (int)mp_maxid;
	while (CPU_ABSENT(pcpu) && pcpu > 0)
		pcpu--;
	thread_lock(curthread);
	sched_bind(curthread, pcpu);
	thread_unlock(curthread);

	m = malloc(sizeof(*m), M_KWQ_TEST, M_WAITOK | M_ZERO);
	m->pcpu = pcpu;
	m->wcpu = kt_cpu >= 0 ? kt_cpu : kt_other_cpu(pcpu);
	m->nitems = KT_MIX_POOL;
	m->items = malloc(sizeof(*m->items) * m->nitems, M_KWQ_TEST,
	    M_WAITOK | M_ZERO);
	m->log = malloc(sizeof(*m->log) * KT_MIX_LOG, M_KWQ_TEST,
	    M_WAITOK | M_ZERO);
	m->item_spin = kt_ns2ticks((uint64_t)kt_cost_us * 1000);
	m->nf_spin = kt_ns2ticks((uint64_t)kt_cost_a * 1000);
	m->coop = true;
	m->gate.ti_magic = KT_MIX_GATE_MAGIC;
	m->q = kt_create(kt_qname, flags, 1, kt_mix_handler, m);
	if (m->q == NULL) {
		kt_fail("kwq_create failed");
		free(m->log, M_KWQ_TEST);
		free(m->items, M_KWQ_TEST);
		free(m, M_KWQ_TEST);
		return (NULL);
	}
	for (i = 0; i < KT_MIX_NNF; i++)
		kwq_notifier_init(&m->nf[i], m->wcpu);
	kwq_activate(m->q);
	return (m);
}

static void
kt_mix_fini(struct kt_mix *m)
{
	m->rearm = 0;
	m->gate_hold = 0;
	kwq_drain(m->q);
	kwq_destroy(m->q);
	free(m->log, M_KWQ_TEST);
	free(m->items, M_KWQ_TEST);
	free(m, M_KWQ_TEST);
}

/*
 * Wait for a flag the handler flips, polling well under a tick: a handler
 * run of a few ms is shorter than one pause(9) at hz=100, so pause(1)
 * missed it every time in the guest.  Returns false after about a second.
 */
static bool
kt_mix_wait_flag(volatile int *flag, int want)
{
	sbintime_t end = sbinuptime() + SBT_1S;

	while (*flag != want) {
		if (sbinuptime() > end)
			return (false);
		pause_sbt("kwqflg", 50 * SBT_1US, 0, C_PREL(1));
	}
	return (true);
}

/* Wait until the log holds `want' entries; false after about a second. */
static bool
kt_mix_wait_log(struct kt_mix *m, u_int want)
{
	int i;

	for (i = 0; i < 1000 && m->nlog < want; i++)
		pause("kwqmix", 1);
	return (m->nlog >= want);
}

/* Open the gate: a pass is now spinning on it with the list behind it empty. */
static bool
kt_mix_gate_open(struct kt_mix *m)
{
	m->gate_hold = 1;
	m->in_gate = 0;
	KWQ_ITEM_INIT(&m->gate.ti_item);
	if (kwq_enqueue(m->q, m->wcpu, &m->gate.ti_item) != 0) {
		kt_fail("gate enqueue failed");
		return (false);
	}
	if (!kt_mix_wait_flag(&m->in_gate, 1)) {
		kt_fail("gate never reached");
		m->gate_hold = 0;
		return (false);
	}
	return (true);
}

static int
kt_mix_enq(struct kt_mix *m, u_int i)
{
	struct titem *ti = &m->items[i];

	ti->ti_magic = TITEM_MAGIC;
	ti->ti_seq = i;
	atomic_store_rel_int(&ti->ti_seen, 1);
	KWQ_ITEM_INIT(&ti->ti_item);
	return (kwq_enqueue(m->q, m->wcpu, &ti->ti_item));
}

/*
 * Compare the delivery log from `from' with an arrival list.  Counts, for
 * every notifier delivered, the items that arrived before it but were
 * delivered after it and the items that arrived after it but were
 * delivered before it; and the notifiers delivered other than exactly
 * once.  Item-vs-item order is FIFO in every design and checked by the
 * fifo scenario, so only pairs involving a notifier are counted.
 */
static void
kt_mix_check(struct kt_mix *m, u_int from, const struct kt_mixlog *arr,
    u_int narr, uint64_t *inversions, uint64_t *exact_errors)
{
	u_int a, d, seen, apos_nf, dpos_nf, apos_it, dpos_it;

	for (a = 0; a < narr; a++) {
		if (arr[a].kind != 1)
			continue;
		apos_nf = a;
		seen = 0;
		dpos_nf = 0;
		for (d = from; d < m->nlog; d++)
			if (m->log[d].kind == 1 && m->log[d].idx == arr[a].idx) {
				seen++;
				dpos_nf = d;
			}
		if (seen != 1) {
			(*exact_errors)++;
			continue;
		}
		for (apos_it = 0; apos_it < narr; apos_it++) {
			if (arr[apos_it].kind != 0)
				continue;
			for (dpos_it = from; dpos_it < m->nlog; dpos_it++)
				if (m->log[dpos_it].kind == 0 &&
				    m->log[dpos_it].idx == arr[apos_it].idx)
					break;
			if (dpos_it >= m->nlog)
				continue;	/* not delivered: counted elsewhere */
			if ((apos_it < apos_nf && dpos_it > dpos_nf) ||
			    (apos_it > apos_nf && dpos_it < dpos_nf))
				(*inversions)++;
		}
	}
}

/*
 * mixorder: K items, one signal, K items, built behind the gate; the
 * signal must be delivered between the two halves.
 */
static void
kt_run_mixorder(void)
{
	struct kt_mix *m;
	struct kt_mixlog *arr;
	u_int rep, i, k, narr, from;

	k = kt_items;
	if (k == 0 || 2 * k + 8 > KT_MIX_POOL) {
		kt_fail("items must be 1..%u", (KT_MIX_POOL - 8) / 2);
		return;
	}
	if ((m = kt_mix_init(0)) == NULL)
		return;
	arr = malloc(sizeof(*arr) * (2 * k + 1), M_KWQ_TEST, M_WAITOK);
	for (rep = 0; rep < kt_reps; rep++) {
		from = m->nlog;
		if (!kt_mix_gate_open(m))
			break;
		from++;		/* the gate entry itself */
		narr = 0;
		for (i = 0; i < k; i++) {
			if (kt_mix_enq(m, i) == 0) {
				arr[narr].kind = 0; arr[narr++].idx = i;
			} else
				kt_r_rejected++;	/* a tiny limit: refused, not an error */
		}
		kt_r_in += k;
		if (kwq_notify(m->q, &m->nf[0])) {
			arr[narr].kind = 1; arr[narr++].idx = 0;
		} else
			kt_r_coalesced++;
		kt_r_in++;
		for (i = k; i < 2 * k; i++) {
			if (kt_mix_enq(m, i) == 0) {
				arr[narr].kind = 0; arr[narr++].idx = i;
			} else
				kt_r_rejected++;
		}
		kt_r_in += k;
		m->gate_hold = 0;
		if (!kt_mix_wait_log(m, from + narr)) {
			kt_fail("rep %u: %u of %u delivered", rep, m->nlog - from, narr);
			break;
		}
		kt_mix_check(m, from, arr, narr, &kt_r_inversions, &kt_r_exact_errors);
		if (m->nlog > KT_MIX_LOG - 2 * k - 8)
			m->nlog = 0;	/* log full: start over, reps are independent */
	}
	free(arr, M_KWQ_TEST);
	kt_r_out = m->item_runs;
	kt_r_runs = m->nf_runs;
	kt_r_calls = m->calls;
	kt_r_wcpu = m->wcpu;
	kt_mix_fini(m);
}

/*
 * mixpattern: every pattern of length 1..4 over {item, N0, N1}, built
 * behind the gate and checked for arrival order and exactly-once.  A
 * notifier signalled twice in one pattern coalesces, so the arrival list
 * holds it once.  Patterns ending in a notifier are the tail-tag case.
 */
static void
kt_run_mixpattern(void)
{
	struct kt_mix *m;
	struct kt_mixlog arr[4];
	u_int len, code, pos, sym, narr, from, nit, c;

	if ((m = kt_mix_init(0)) == NULL)
		return;
	for (len = 1; len <= 4 && kt_state[0] != 'f'; len++) {
		for (code = 0; code < (1u << (2 * len)) && kt_state[0] != 'f'; code++) {
			/* 2 bits per position: 0 item, 1 N0, 2 N1, 3 skip */
			for (pos = 0, c = code; pos < len; pos++, c >>= 2)
				if ((c & 3) == 3)
					break;
			if (pos < len)
				continue;
			from = m->nlog;
			if (!kt_mix_gate_open(m))
				break;
			from++;
			narr = nit = 0;
			for (pos = 0, c = code; pos < len; pos++, c >>= 2) {
				sym = c & 3;
				if (sym == 0) {
					if (kt_mix_enq(m, nit) == 0) {
						arr[narr].kind = 0; arr[narr++].idx = nit;
					} else
						kt_r_rejected++;
					nit++;
				} else {
					if (kwq_notify(m->q, &m->nf[sym - 1])) {
						arr[narr].kind = 1; arr[narr++].idx = sym - 1;
					} else
						kt_r_coalesced++;
				}
				kt_r_in++;
			}
			m->gate_hold = 0;
			if (!kt_mix_wait_log(m, from + narr)) {
				kt_fail("pattern %u/%u: %u of %u delivered", len, code,
				    m->nlog - from, narr);
				break;
			}
			kt_mix_check(m, from, arr, narr, &kt_r_inversions, &kt_r_exact_errors);
			kt_r_patterns++;
			if (m->nlog > KT_MIX_LOG - 16)
				m->nlog = 0;
		}
	}
	kt_r_out = m->item_runs;
	kt_r_runs = m->nf_runs;
	kt_r_calls = m->calls;
	kt_r_wcpu = m->wcpu;
	kt_mix_fini(m);
}

/*
 * mixbudget: `nnf' notifiers of cost_a us each and K items, built behind
 * the gate.  A budget-honouring handler must spread them over passes;
 * the ATF case reads overruns, parks and requeued on wcpu before and after.
 */
static void
kt_run_mixbudget(void)
{
	struct kt_mix *m;
	u_int rep, i, nnf, from, want;

	nnf = kt_nnf;
	if (nnf == 0 || nnf > KT_MIX_NNF || kt_items + 8 > KT_MIX_POOL) {
		kt_fail("nnf 1..%u, items <= %u", KT_MIX_NNF, KT_MIX_POOL - 8);
		return;
	}
	if ((m = kt_mix_init(0)) == NULL)
		return;
	for (rep = 0; rep < kt_reps; rep++) {
		from = m->nlog;
		if (!kt_mix_gate_open(m))
			break;
		from++;
		want = 0;
		for (i = 0; i < nnf; i++) {
			if (kwq_notify(m->q, &m->nf[i]))
				want++;
			kt_r_in++;
		}
		for (i = 0; i < kt_items; i++) {
			if (kt_mix_enq(m, i) == 0)
				want++;
			else
				kt_r_rejected++;
			kt_r_in++;
		}
		m->gate_hold = 0;
		if (!kt_mix_wait_log(m, from + want)) {
			kt_fail("rep %u: %u of %u delivered", rep, m->nlog - from, want);
			break;
		}
		if (m->nlog > KT_MIX_LOG - nnf - kt_items - 8)
			m->nlog = 0;
	}
	kt_r_out = m->item_runs;
	kt_r_runs = m->nf_runs;
	kt_r_calls = m->calls;
	kt_r_requeues = m->requeues;
	kt_r_wcpu = m->wcpu;
	kt_mix_fini(m);
}

/*
 * mixstarve (SCHED.md S14 case 4): a flood of cost_us items from this CPU
 * for `secs', once alone and once beside a notifier of cost_a us that
 * re-arms itself in its own handler.  Reports items delivered in each
 * phase.
 */
static uint64_t
kt_mix_flood(struct kt_mix *m, u_int secs)
{
	struct titem *ti;
	sbintime_t end;
	uint64_t before, spins = 0;
	u_int i = 0;

	before = m->item_runs;
	end = sbinuptime() + (sbintime_t)secs * SBT_1S;
	while (sbinuptime() < end) {
		ti = &m->items[i];
		if (atomic_load_acq_int(&ti->ti_seen) == 0) {
			if (kt_mix_enq(m, i) != 0) {
				atomic_store_rel_int(&ti->ti_seen, 0);
				kt_r_rejected++;
			} else
				kt_r_in++;
		} else if (++spins % 64 == 0)
			cpu_spinwait();
		i = (i + 1) % m->nitems;
	}
	/* Let the backlog drain so the phases do not overlap. */
	for (i = 0; i < 2000; i++) {
		for (spins = 0; spins < m->nitems; spins++)
			if (atomic_load_acq_int(&m->items[spins].ti_seen) != 0)
				break;
		if (spins == m->nitems)
			break;
		pause("kwqstv", 1);
	}
	return (m->item_runs - before);
}

static void
kt_run_mixstarve(void)
{
	struct kt_mix *m;

	if ((m = kt_mix_init(0)) == NULL)
		return;
	kt_r_without = kt_mix_flood(m, kt_secs);
	m->rearm = 1;
	if (!kwq_notify(m->q, &m->nf[0])) {
		kt_fail("first signal refused");
		kt_mix_fini(m);
		return;
	}
	kt_r_with = kt_mix_flood(m, kt_secs);
	m->rearm = 0;
	(void)kt_mix_wait_flag(&m->in_nf, 0);
	kt_r_out = m->item_runs;
	kt_r_runs = m->nf_runs;
	kt_r_calls = m->calls;
	kt_r_wcpu = m->wcpu;
	kt_mix_fini(m);
}

/*
 * mixrearm: a signal that lands while the notifier's handler is running
 * must produce exactly one further run, which sees the final level.
 */
static void
kt_run_mixrearm(void)
{
	struct kt_mix *m;
	uint64_t runs0;
	u_int rep, s;
	int i;

	if ((m = kt_mix_init(0)) == NULL)
		return;
	if (m->nf_spin == 0)
		m->nf_spin = kt_ns2ticks(2000000);	/* 2 ms so the race is reachable */
	for (rep = 0; rep < kt_reps; rep++) {
		runs0 = m->nf_runs;
		m->level++;
		if (!kwq_notify(m->q, &m->nf[0])) {
			kt_fail("rep %u: signal on an idle notifier refused", rep);
			break;
		}
		kt_r_in++;
		if (!kt_mix_wait_flag(&m->in_nf, 1)) {
			kt_fail("rep %u: handler did not start", rep);
			break;
		}
		for (s = 0; s < (kt_batch ? kt_batch : 1); s++) {
			m->level++;
			if (!kwq_notify(m->q, &m->nf[0]))
				kt_r_coalesced++;
			kt_r_in++;
		}
		for (i = 0; i < 1000 && m->nf_runs < runs0 + 2; i++)
			pause("kwqrea", 1);
		(void)kt_mix_wait_flag(&m->in_nf, 0);
		if (m->nf_runs != runs0 + 2) {
			kt_fail("rep %u: %ju runs, expected exactly 2", rep,
			    (uintmax_t)(m->nf_runs - runs0));
			break;
		}
		if (m->level_seen != m->level) {
			kt_fail("rep %u: final level %ju not observed (saw %ju)", rep,
			    (uintmax_t)m->level, (uintmax_t)m->level_seen);
			break;
		}
	}
	kt_r_runs = m->nf_runs;
	kt_r_calls = m->calls;
	kt_r_wcpu = m->wcpu;
	kt_mix_fini(m);
}

/*
 * mixcancel: (a) cancel while the handler runs returns only after it has
 * returned, the cancelled notifier refuses signals, and re-init revives
 * it; (b) a kthread signals nf[1] flat out while this thread cancels and
 * re-inits it for `secs'.
 */
static volatile int kt_mix_hammer_stop;
static uint64_t kt_mix_hammer_signals;
static sbintime_t kt_mix_hammer_end;

/*
 * A signaller with about a microsecond of work between signals, which is
 * the least any real producer does.  Without that work the loop takes
 * and drops the per-CPU mutex back to back and, FreeBSD mutexes being
 * unfair, starves both the worker and the canceller on it indefinitely
 * (seen in the guest 2026-10-08: the canceller slept in
 * kwq_notify_cancel's mtx_lock for minutes).  The thread also has a
 * deadline of its own, so a stuck producer cannot leave it spinning.
 */
static void
kt_mix_hammer(void *arg)
{
	struct kt_mix *m = arg;
	uint64_t spin = kt_ns2ticks(1000);

	while (!kt_mix_hammer_stop && sbinuptime() < kt_mix_hammer_end) {
		kwq_notify(m->q, &m->nf[1]);
		kt_mix_hammer_signals++;
		kt_spin_ticks(spin);
	}
	kthread_exit();
}

static void
kt_run_mixcancel(void)
{
	struct kt_mix *m;
	struct thread *td;
	sbintime_t t1;
	uint64_t runs0, cancels = 0;
	u_int rep;
	int i;

	if ((m = kt_mix_init(0)) == NULL)
		return;
	if (m->nf_spin == 0)
		m->nf_spin = kt_ns2ticks(2000000);
	for (rep = 0; rep < kt_reps; rep++) {
		runs0 = m->nf_runs;
		if (!kwq_notify(m->q, &m->nf[0])) {
			kt_fail("rep %u: signal refused", rep);
			break;
		}
		if (!kt_mix_wait_flag(&m->in_nf, 1)) {
			kt_fail("rep %u: handler did not start", rep);
			break;
		}
		kwq_notify_cancel(m->q, &m->nf[0]);
		t1 = sbinuptime();
		cancels++;
		if (m->in_nf || t1 < m->nf_exit) {
			kt_fail("rep %u: cancel returned before the handler", rep);
			break;
		}
		if (kwq_notify(m->q, &m->nf[0])) {
			kt_fail("rep %u: cancelled notifier accepted a signal", rep);
			break;
		}
		kwq_notifier_init(&m->nf[0], m->wcpu);
		if (!kwq_notify(m->q, &m->nf[0])) {
			kt_fail("rep %u: re-initialised notifier refused", rep);
			break;
		}
		for (i = 0; i < 1000 && m->nf_runs < runs0 + 2; i++)
			pause("kwqcan", 1);
		(void)kt_mix_wait_flag(&m->in_nf, 0);
		if (m->nf_runs != runs0 + 2) {
			kt_fail("rep %u: %ju runs, expected 2", rep,
			    (uintmax_t)(m->nf_runs - runs0));
			break;
		}
	}
	if (kt_state[0] != 'f' && kt_secs > 0) {
		m->nf_spin = 0;
		kt_mix_hammer_stop = 0;
		kt_mix_hammer_signals = 0;
		kt_mix_hammer_end = sbinuptime() + (sbintime_t)(kt_secs + 2) * SBT_1S;
		if (kthread_add(kt_mix_hammer, m, NULL, &td, 0, 0, "kwq_test_hammer") != 0) {
			kt_fail("kthread_add");
		} else {
			sbintime_t end = sbinuptime() + (sbintime_t)kt_secs * SBT_1S;

			while (sbinuptime() < end) {
				kwq_notify_cancel(m->q, &m->nf[1]);
				kwq_notifier_init(&m->nf[1], m->wcpu);
				cancels++;
			}
			kt_mix_hammer_stop = 1;
			pause("kwqham", hz / 10);
			kwq_notify_cancel(m->q, &m->nf[1]);
			kwq_notifier_init(&m->nf[1], m->wcpu);
			runs0 = m->nf_runs;
			if (!kwq_notify(m->q, &m->nf[1]))
				kt_fail("nf[1] refused after the hammer");
			for (i = 0; i < 1000 && m->nf_runs < runs0 + 1; i++)
				pause("kwqham", 1);
			if (m->nf_runs != runs0 + 1)
				kt_fail("nf[1]: %ju runs after the hammer, expected 1",
				    (uintmax_t)(m->nf_runs - runs0));
		}
	}
	kt_r_in = cancels;
	kt_r_coalesced = kt_mix_hammer_signals;
	kt_r_runs = m->nf_runs;
	kt_r_calls = m->calls;
	kt_r_wcpu = m->wcpu;
	kt_mix_fini(m);
}

/*
 * mixrequeue: K items with a notifier after every third, built behind the
 * gate, delivered by a handler that requeues after `batch' elements: a
 * remainder that contains notifiers must still deliver each exactly once
 * and in arrival order.
 */
static void
kt_run_mixrequeue(void)
{
	struct kt_mix *m;
	struct kt_mixlog *arr;
	u_int rep, i, k, nf, narr, from;

	k = kt_items;
	if (k == 0 || k + 8 > KT_MIX_POOL) {
		kt_fail("items 1..%u", KT_MIX_POOL - 8);
		return;
	}
	if ((m = kt_mix_init(0)) == NULL)
		return;
	m->stop_after = kt_batch;
	arr = malloc(sizeof(*arr) * (k + KT_MIX_NNF), M_KWQ_TEST, M_WAITOK);
	for (rep = 0; rep < kt_reps; rep++) {
		from = m->nlog;
		if (!kt_mix_gate_open(m))
			break;
		from++;
		narr = nf = 0;
		for (i = 0; i < k; i++) {
			if (kt_mix_enq(m, i) == 0) {
				arr[narr].kind = 0; arr[narr++].idx = i;
			} else
				kt_r_rejected++;
			kt_r_in++;
			if (i % 3 == 2 && nf < KT_MIX_NNF) {
				if (kwq_notify(m->q, &m->nf[nf])) {
					arr[narr].kind = 1; arr[narr++].idx = nf;
				}
				nf++;
				kt_r_in++;
			}
		}
		m->gate_hold = 0;
		if (!kt_mix_wait_log(m, from + narr)) {
			kt_fail("rep %u: %u of %u delivered", rep, m->nlog - from, narr);
			break;
		}
		kt_mix_check(m, from, arr, narr, &kt_r_inversions, &kt_r_exact_errors);
		if (m->nlog > KT_MIX_LOG - k - KT_MIX_NNF - 8)
			m->nlog = 0;
	}
	free(arr, M_KWQ_TEST);
	kt_r_out = m->item_runs;
	kt_r_runs = m->nf_runs;
	kt_r_calls = m->calls;
	kt_r_requeues = m->requeues;
	kt_r_wcpu = m->wcpu;
	kt_mix_fini(m);
}

/*
 * mixdrain: pending notifiers among items when the queue is drained, run
 * or discarded; afterwards every notifier is idle and a signal is refused.
 * Four shapes per rep: [N0 I N1 I I N2], [N0], [I N0], [N0 N1 N2].
 */
static void
kt_run_mixdrain(uint32_t flags)
{
	static const char *shapes[] = { "NININ", "N", "IN", "NNN" };
	struct kt_mix *m;
	u_int rep, s, i, nf, nit;
	const char *p;

	for (rep = 0; rep < kt_reps && kt_state[0] != 'f'; rep++) {
		for (s = 0; s < nitems(shapes) && kt_state[0] != 'f'; s++) {
			if ((m = kt_mix_init(flags)) == NULL)
				return;
			if (!kt_mix_gate_open(m)) {
				kt_mix_fini(m);
				return;
			}
			nf = nit = 0;
			for (p = shapes[s]; *p != '\0'; p++) {
				if (*p == 'I') {
					if (kt_mix_enq(m, nit++) != 0)
						kt_r_rejected++;
				} else if (kwq_notify(m->q, &m->nf[nf++]))
					;
				kt_r_in++;
			}
			m->gate_hold = 0;
			kwq_drain(m->q);
			for (i = 0; i < nf; i++)
				if ((m->nf[i].kn_state & 3) != 0) {	/* low bits: KWQ_NF_IDLE */
					kt_fail("rep %u shape %s: nf[%u] state %u after drain",
					    rep, shapes[s], i, m->nf[i].kn_state);
					kt_r_exact_errors++;
				}
			if (kwq_notify(m->q, &m->nf[0]))
				kt_fail("signal accepted after drain");
			kt_r_out += m->item_runs;
			kt_r_discarded += m->discarded;
			kt_r_runs += m->nf_runs;
			kt_r_calls += m->calls;
			kt_r_wcpu = m->wcpu;
			kwq_destroy(m->q);
			free(m->log, M_KWQ_TEST);
			free(m->items, M_KWQ_TEST);
			free(m, M_KWQ_TEST);
		}
	}
}


/* ================================================================ P6 */

/*
 * BULK stealing and scatter (../SCHED.md S16).  The queue (kern.kwq.bulk.
 * queue.kts) and its items are kept until the next run or the unload, so
 * the ATF cases can read kwq's steal counters afterwards.
 */
struct kt_sgroup {
	struct kwq_group g;
	volatile u_int	handled;	/* items of this call run so far */
	volatile u_int	done;		/* done() calls: must end at 1 */
	u_int		n;
	cpuset_t	cpus;		/* CPUs that ran its items */
	struct mtx	mtx;		/* for cpus */
};

static struct kwq *kt_bq;
static struct titem *kt_bitems;
static struct kt_sgroup *kt_sg;
static uint64_t kt_sdone, kt_sdone_errors;

static void
kt_bulk_destroy(void)
{
	if (kt_bq != NULL) {
		kwq_drain(kt_bq);
		kwq_destroy(kt_bq);
		kt_bq = NULL;
	}
	free(kt_bitems, M_KWQ_TEST);
	kt_bitems = NULL;
	if (kt_sg != NULL) {
		mtx_destroy(&kt_sg->mtx);
		free(kt_sg, M_KWQ_TEST);
		kt_sg = NULL;
	}
	free(kt_cpu_hits, M_KWQ_TEST);
	kt_cpu_hits = NULL;
}

static int
kt_bulk_target(void)
{
	int cpu = kt_cpu >= 0 ? kt_cpu : 1;

	if (cpu > (int)mp_maxid || CPU_ABSENT(cpu))
		cpu = 0;
	return (cpu);
}

static void
kt_bind(int cpu)
{
	thread_lock(curthread);
	if (cpu >= 0)
		sched_bind(curthread, cpu);
	else
		sched_unbind(curthread);
	thread_unlock(curthread);
}

/*
 * steal: kt_items items of cost_us each onto one CPU's list of a stealable
 * queue, from a producer on another CPU, as fast as the limit admits.  The
 * target's worker falls behind; the steal wake and the idle workers' own
 * attempts spread the backlog.  Passes when every admitted item ran exactly
 * once; result_offcpu and result_cpus say how far it spread, and the queue's
 * steals_in/steals_out say how.
 */
static void
kt_run_steal(void)
{
	struct kwq_params p;
	struct titem *ti;
	uint64_t in = 0, rejected = 0;
	u_int i;
	int tcpu, c, error, deadline;

	tcpu = kt_bulk_target();
	kt_bind(kt_other_cpu(tcpu));
	memset(&p, 0, sizeof(p));
	p.limit = kt_limit;
	p.domain = -1;
	kt_cpu_hits = malloc(sizeof(*kt_cpu_hits) * (mp_maxid + 1), M_KWQ_TEST,
	    M_WAITOK | M_ZERO);
	kt_bitems = malloc(sizeof(*kt_bitems) * kt_items, M_KWQ_TEST,
	    M_WAITOK | M_ZERO);
	kt_bq = kwq_create("kts", KWQ_BULK, KWQ_F_STEALABLE, &p, kt_item_handler,
	    NULL);
	if (kt_bq == NULL) {
		kt_fail("kwq_create(kts) failed");
		kt_bind(-1);
		return;
	}
	kt_nofifo = true;
	kt_coop = true;
	kt_dups = 0;
	kt_rep_tag = 1;
	for (i = 0; i < kt_items; i++) {
		ti = &kt_bitems[i];
		ti->ti_magic = TITEM_MAGIC;
		ti->ti_cpu = tcpu;
		ti->ti_seq = i + 1;
		KWQ_ITEM_INIT(&ti->ti_item);
		in++;
		error = kwq_enqueue(kt_bq, tcpu, &ti->ti_item);
		if (error == ENOBUFS)
			rejected++;
		else if (error != 0) {
			kt_fail("enqueue error %d", error);
			break;
		}
	}
	deadline = ticks + 60 * hz;
	while (atomic_load_64(&kt_handled) + rejected < in &&
	    ticks - deadline < 0)
		pause("ktstl", 1);
	kt_r_in = in;
	kt_r_out = kt_handled;
	kt_r_rejected = rejected;
	kt_r_dups = kt_dups;
	kt_r_wcpu = tcpu;
	CPU_FOREACH(c) {
		if (kt_cpu_hits[c] != 0)
			kt_r_cpus++;
		if (c != tcpu)
			kt_r_offcpu += kt_cpu_hits[c];
	}
	if (kt_state[0] != 'f' && kt_handled + rejected != in)
		kt_fail("in %ju != run %ju + rejected %ju after 60 s",
		    (uintmax_t)in, (uintmax_t)kt_handled, (uintmax_t)rejected);
	else if (kt_dups != 0)
		kt_fail("%ju items run twice", (uintmax_t)kt_dups);
	kt_nofifo = false;
	kt_coop = false;
	kt_bind(-1);
}

/*
 * The P6g scenarios' common part: a stealable BULK queue of kt_items items
 * of cost_us each, enqueued onto `cpu' (or KWQ_CPU_ANY) from the calling
 * thread as fast as the limit admits; waits until every admitted item ran
 * and fills the results as kt_run_steal() does.  Returns the time from
 * the first enqueue to the last item run.
 */
static sbintime_t
kt_bulk_flood(int tcpu, int cpu)
{
	struct kwq_params p;
	struct titem *ti;
	uint64_t in = 0, rejected = 0;
	sbintime_t t0;
	u_int i;
	int c, error, deadline;

	memset(&p, 0, sizeof(p));
	p.limit = kt_limit;
	p.domain = -1;
	kt_cpu_hits = malloc(sizeof(*kt_cpu_hits) * (mp_maxid + 1), M_KWQ_TEST,
	    M_WAITOK | M_ZERO);
	kt_bitems = malloc(sizeof(*kt_bitems) * kt_items, M_KWQ_TEST,
	    M_WAITOK | M_ZERO);
	kt_bq = kwq_create("kts", KWQ_BULK, KWQ_F_STEALABLE, &p, kt_item_handler,
	    NULL);
	if (kt_bq == NULL) {
		kt_fail("kwq_create(kts) failed");
		return (0);
	}
	kt_nofifo = true;
	kt_coop = true;
	kt_dups = 0;
	kt_rep_tag = 1;
	t0 = sbinuptime();
	for (i = 0; i < kt_items; i++) {
		ti = &kt_bitems[i];
		ti->ti_magic = TITEM_MAGIC;
		ti->ti_cpu = tcpu;
		ti->ti_seq = i + 1;
		KWQ_ITEM_INIT(&ti->ti_item);
		in++;
		error = kwq_enqueue(kt_bq, cpu, &ti->ti_item);
		if (error == ENOBUFS)
			rejected++;
		else if (error != 0) {
			kt_fail("enqueue error %d", error);
			break;
		}
	}
	deadline = ticks + 60 * hz;
	while (atomic_load_64(&kt_handled) + rejected < in &&
	    ticks - deadline < 0)
		pause("ktfld", 1);
	t0 = sbinuptime() - t0;
	kt_r_in = in;
	kt_r_out = kt_handled;
	kt_r_rejected = rejected;
	kt_r_dups = kt_dups;
	kt_r_wcpu = tcpu;
	CPU_FOREACH(c) {
		if (kt_cpu_hits[c] != 0)
			kt_r_cpus++;
		if (c != tcpu)
			kt_r_offcpu += kt_cpu_hits[c];
	}
	if (kt_state[0] != 'f' && kt_handled + rejected != in)
		kt_fail("in %ju != run %ju + rejected %ju after 60 s",
		    (uintmax_t)in, (uintmax_t)kt_handled, (uintmax_t)rejected);
	else if (kt_dups != 0)
		kt_fail("%ju items run twice", (uintmax_t)kt_dups);
	kt_nofifo = false;
	kt_coop = false;
	return (t0);
}

/*
 * anyburst (../SPEC-p6g.md S4, ATF island): KWQ_CPU_ANY items from a
 * producer bound to the target CPU: its neighbourhood fills, and under
 * place=2 with groups of one CPU (kern.kwq.bulk.group=1) the far probe
 * reaches the CPUs beyond the near groups.  result_maxlat_a_us is the
 * time until every item ran.
 */
static void
kt_run_anyburst(void)
{
	int tcpu = kt_bulk_target();

	kt_bind(tcpu);
	kt_r_maxlat_a_us = kt_sbt2us(kt_bulk_flood(tcpu, KWQ_CPU_ANY));
	kt_bind(-1);
}

/*
 * starved (ATF starved): a kernel thread at PI_NET, above BULK's PI_SOFT,
 * spins on the target CPU for spin_ms while a producer on another CPU puts
 * the items on that CPU's list.  The target's BULK worker gets no time;
 * result_maxlat_a_us is the time until every item ran, which under
 * place=2 the starved tick bounds (kwq_shard.c), and result_maxlat_b_us
 * is 1 if the spinner was still running when the last item ran.
 */
static volatile u_int kt_spin_state;	/* 0 starting, 1 spinning, 2 done */

static void
kt_spinner(void *arg)
{
	int cpu = (int)(intptr_t)arg;
	sbintime_t end;

	thread_lock(curthread);
	sched_prio(curthread, PI_NET);
	sched_bind(curthread, cpu);
	thread_unlock(curthread);
	atomic_store_rel_int(&kt_spin_state, 1);
	end = sbinuptime() + (sbintime_t)kt_spin_ms * SBT_1MS;
	while (sbinuptime() < end)
		cpu_spinwait();
	atomic_store_rel_int(&kt_spin_state, 2);
	kthread_exit();
}

static void
kt_run_starved(void)
{
	struct thread *td;
	int tcpu = kt_bulk_target(), error, n;

	kt_bind(kt_other_cpu(tcpu));
	kt_spin_state = 0;
	error = kthread_add(kt_spinner, (void *)(intptr_t)tcpu, NULL, &td, 0, 0,
	    "kt_spin/%d", tcpu);
	if (error != 0) {
		kt_fail("kthread_add(kt_spin): %d", error);
		kt_bind(-1);
		return;
	}
	for (n = 0; atomic_load_acq_int(&kt_spin_state) == 0 && n < 10 * hz; n++)
		pause("ktspn", 1);
	kt_r_maxlat_a_us = kt_sbt2us(kt_bulk_flood(tcpu, tcpu));
	kt_r_maxlat_b_us = atomic_load_acq_int(&kt_spin_state) == 1;	/* still spinning */
	while (atomic_load_acq_int(&kt_spin_state) != 2)
		pause("ktspn", 1);
	pause("ktspn", hz / 10);	/* the spinner out of kthread_exit()'s path */
	kt_bind(-1);
}

static void
kt_scatter_handler(struct kwq *q, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;
	struct titem *ti;
	struct kt_sgroup *sg;
	int cpu = curcpu;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		KWQ_ITEM_INIT(it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC)
			panic("kwq_test: bad item %p", ti);
		if (ti->ti_seen == kt_rep_tag)
			atomic_add_64(&kt_dups, 1);
		ti->ti_seen = kt_rep_tag;
		sg = ti->ti_grp;
		if (n > 0 && kt_cost_us != 0)
			DELAY(kt_cost_us);
		mtx_lock(&sg->mtx);
		CPU_SET(cpu, &sg->cpus);
		mtx_unlock(&sg->mtx);
		atomic_add_int(&sg->handled, 1);
		atomic_add_64(&kt_handled, 1);
		kwq_group_leave(&sg->g, 1);
	}
}

static void
kt_scatter_done(void *arg)
{
	struct kt_sgroup *sg = arg;

	/* Every item's handled++ came before its leave, so all are counted. */
	if (sg->handled != sg->n)
		atomic_add_64(&kt_sdone_errors, 1);
	if (atomic_fetchadd_int(&sg->done, 1) != 0)
		atomic_add_64(&kt_sdone_errors, 1);
	atomic_add_64(&kt_sdone, 1);
}

/*
 * scatter: kt_reps calls of kt_items items each (cost_us apiece), one at a
 * time from a producer bound to the target CPU: each must complete exactly
 * once, after all of its items, and result_cpus reports the most CPUs one
 * call reached.  A refused call (ENOBUFS) is counted and retried.
 */
static void
kt_run_scatter(void)
{
	struct kwq_params p;
	struct kt_sgroup *sg;
	struct titem *ti;
	u_int rep, i, ncpu;
	int error, deadline, tcpu;

	tcpu = kt_bulk_target();
	kt_bind(tcpu);
	memset(&p, 0, sizeof(p));
	p.limit = kt_limit;
	p.domain = -1;
	kt_bitems = malloc(sizeof(*kt_bitems) * kt_items, M_KWQ_TEST,
	    M_WAITOK | M_ZERO);
	kt_sg = sg = malloc(sizeof(*sg), M_KWQ_TEST, M_WAITOK | M_ZERO);
	mtx_init(&sg->mtx, "kt scatter", NULL, MTX_DEF);
	kt_bq = kwq_create("kts", KWQ_BULK, 0, &p, kt_scatter_handler, NULL);
	if (kt_bq == NULL) {
		kt_fail("kwq_create(kts) failed");
		kt_bind(-1);
		return;
	}
	kt_sdone = kt_sdone_errors = 0;
	kt_dups = 0;
	for (rep = 0; rep < kt_reps && kt_state[0] != 'f'; rep++) {
		kt_rep_tag = rep + 1;
		sg->handled = 0;
		sg->done = 0;
		sg->n = kt_items;
		CPU_ZERO(&sg->cpus);
		for (i = 0; i < kt_items; i++) {
			ti = &kt_bitems[i];
			ti->ti_magic = TITEM_MAGIC;
			ti->ti_grp = sg;
			ti->ti_item.kwi_link.stqe_next = i + 1 < kt_items ?
			    &kt_bitems[i + 1].ti_item : NULL;
		}
		while ((error = kwq_scatter(kt_bq, &sg->g, &kt_bitems[0].ti_item,
		    kt_items, kt_scatter_done, sg)) == ENOBUFS) {
			kt_r_rejected++;
			pause("ktscr", 1);
		}
		if (error != 0) {
			kt_fail("kwq_scatter: %d", error);
			break;
		}
		kt_r_in += kt_items;
		deadline = ticks + 30 * hz;
		while (atomic_load_int(&sg->done) == 0 && ticks - deadline < 0)
			pause("ktscw", 1);
		if (sg->done == 0) {
			kt_fail("rep %u: no completion after 30 s (%u of %u run)",
			    rep, sg->handled, sg->n);
			break;
		}
		ncpu = CPU_COUNT(&sg->cpus);
		if (ncpu > kt_r_cpus)
			kt_r_cpus = ncpu;
	}
	/* a second done() would come after the first: give it a moment */
	pause("ktscz", hz / 10);
	kt_r_out = kt_handled;
	kt_r_runs = kt_sdone;
	kt_r_exact_errors = kt_sdone_errors;
	kt_r_dups = kt_dups;
	if (kt_state[0] != 'f') {
		if (kt_sdone != kt_r_in / (kt_items ? kt_items : 1))
			kt_fail("%ju completions for %ju calls", (uintmax_t)kt_sdone,
			    (uintmax_t)(kt_r_in / (kt_items ? kt_items : 1)));
		else if (kt_sdone_errors != 0)
			kt_fail("%ju completions early or repeated",
			    (uintmax_t)kt_sdone_errors);
		else if (kt_dups != 0)
			kt_fail("%ju items run twice", (uintmax_t)kt_dups);
	}
	kt_bind(-1);
}

/*
 * stealdrain: the P0 create/activate/enqueue/drain/destroy cycle on a
 * stealable BULK queue, every item onto one CPU so that stealing runs while
 * the drain sweeps: nothing may be lost or run twice (with `discard', the
 * drain hands the rest back with n < 0 instead of running it).
 */
static void
kt_run_stealdrain(uint32_t flags)
{
	int cpu = kt_cpu;

	kt_cpu = kt_bulk_target();
	kt_class = KWQ_BULK;
	kt_nofifo = true;
	kt_coop = true;
	kt_run_items(KWQ_F_STEALABLE | flags, kt_item_handler);
	kt_coop = false;
	kt_nofifo = false;
	kt_class = KWQ_NET;
	kt_cpu = cpu;
}

static void
kt_thread(void *arg __unused)
{
	struct timespec t0, t1;

	nanouptime(&t0);
	kt_floods_destroy();
	kt_bulk_destroy();
	kt_handled = kt_discarded = kt_nf_runs = kt_nf_seen = kt_nf_state = 0;
	kt_fifo_errors = 0;
	kt_r_in = kt_r_out = kt_r_discarded = kt_r_rejected = kt_r_coalesced = 0;
	kt_r_runs = kt_r_fifo_errors = 0;
	kt_r_cycles_a = kt_r_cycles_b = kt_r_maxlat_a_us = kt_r_maxlat_b_us = 0;
	kt_r_ns_per_item = kt_r_prod_ns_per_item = kt_r_glitches = kt_r_dups = 0;
	kt_r_lat_ns = 0;
	kt_clocks[0] = '\0';
	kt_r_enq_slow = kt_r_enq_pct = kt_r_cons_pct = kt_r_pool_waits = 0;
	kt_r_inversions = kt_r_exact_errors = kt_r_calls = kt_r_requeues = 0;
	kt_r_patterns = kt_r_with = kt_r_without = kt_r_wcpu = 0;
	kt_r_bursts = kt_r_first_p50_us = kt_r_first_p99_us = kt_r_first_max_us = 0;
	kt_r_last_p50_us = kt_r_last_p99_us = kt_r_last_max_us = 0;
	kt_r_cpus = kt_r_offcpu = 0;
	kt_msg[0] = '\0';

	if (strcmp(kt_scenario, "lifecycle") == 0 ||
	    strcmp(kt_scenario, "fifo") == 0 ||
	    strcmp(kt_scenario, "reject") == 0)
		kt_run_items(0, kt_item_handler);
	else if (strcmp(kt_scenario, "discard") == 0)
		kt_run_items(KWQ_F_DISCARD, kt_item_handler);
	else if (strcmp(kt_scenario, "notify") == 0)
		kt_run_notify();
	else if (strcmp(kt_scenario, "sleep") == 0) {
		if (!kt_allow_panic)
			kt_fail("sleep scenario needs allow_panic=1");
		else
			kt_run_items(0, kt_sleep_handler);
	} else if (strcmp(kt_scenario, "fairness") == 0)
		kt_run_fairness();
	else if (strcmp(kt_scenario, "latency") == 0)
		kt_run_latency();
	else if (strcmp(kt_scenario, "gaming") == 0)
		kt_run_gaming();
	else if (strcmp(kt_scenario, "overrun") == 0)
		kt_run_overrun();
	else if (strcmp(kt_scenario, "yield") == 0)
		kt_run_yield();
	else if (strcmp(kt_scenario, "scale") == 0)
		kt_run_scale();
	else if (strcmp(kt_scenario, "fanin") == 0)
		kt_run_fanin();
	else if (strcmp(kt_scenario, "ifpair") == 0)
		kt_run_ifpair();
	else if (strcmp(kt_scenario, "mixorder") == 0)
		kt_run_mixorder();
	else if (strcmp(kt_scenario, "mixpattern") == 0)
		kt_run_mixpattern();
	else if (strcmp(kt_scenario, "mixbudget") == 0)
		kt_run_mixbudget();
	else if (strcmp(kt_scenario, "mixstarve") == 0)
		kt_run_mixstarve();
	else if (strcmp(kt_scenario, "mixrearm") == 0)
		kt_run_mixrearm();
	else if (strcmp(kt_scenario, "mixcancel") == 0)
		kt_run_mixcancel();
	else if (strcmp(kt_scenario, "mixrequeue") == 0)
		kt_run_mixrequeue();
	else if (strcmp(kt_scenario, "mixdrain") == 0)
		kt_run_mixdrain(0);
	else if (strcmp(kt_scenario, "mixdrain_discard") == 0)
		kt_run_mixdrain(KWQ_F_DISCARD);
	else if (strcmp(kt_scenario, "cost") == 0)
		kt_run_cost();
	else if (strcmp(kt_scenario, "burst") == 0)
		kt_run_burst();
	else if (strcmp(kt_scenario, "tq_baseline") == 0)
		kt_run_tq_baseline();
	else if (strcmp(kt_scenario, "switch_baseline") == 0)
		kt_run_switch_baseline();
	else if (strcmp(kt_scenario, "pingpong") == 0)
		kt_run_pingpong(false);
	else if (strcmp(kt_scenario, "pingpong_tq") == 0)
		kt_run_pingpong(true);
	else if (strcmp(kt_scenario, "clocks") == 0)
		kt_run_clocks();
	else if (strcmp(kt_scenario, "steal") == 0)
		kt_run_steal();
	else if (strcmp(kt_scenario, "scatter") == 0)
		kt_run_scatter();
	else if (strcmp(kt_scenario, "stealdrain") == 0)
		kt_run_stealdrain(0);
	else if (strcmp(kt_scenario, "stealdrain_discard") == 0)
		kt_run_stealdrain(KWQ_F_DISCARD);
	else if (strcmp(kt_scenario, "anyburst") == 0)
		kt_run_anyburst();
	else if (strcmp(kt_scenario, "starved") == 0)
		kt_run_starved();
	else
		kt_fail("unknown scenario %s", kt_scenario);

	nanouptime(&t1);
	kt_r_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
	    (t1.tv_nsec - t0.tv_nsec);
	if (kt_state[0] != 'f')
		strlcpy(kt_state, "done", sizeof(kt_state));
	printf("kwq_test: %s %s: in %ju out %ju discarded %ju rejected %ju "
	    "coalesced %ju runs %ju fifo_errors %ju cycles %ju/%ju maxlat %ju/%ju us "
	    "ns/item %ju prod %ju in %ju us\n", kt_scenario,
	    kt_state, (uintmax_t)kt_r_in, (uintmax_t)kt_r_out,
	    (uintmax_t)kt_r_discarded, (uintmax_t)kt_r_rejected,
	    (uintmax_t)kt_r_coalesced, (uintmax_t)kt_r_runs,
	    (uintmax_t)kt_r_fifo_errors, (uintmax_t)kt_r_cycles_a,
	    (uintmax_t)kt_r_cycles_b, (uintmax_t)kt_r_maxlat_a_us,
	    (uintmax_t)kt_r_maxlat_b_us, (uintmax_t)kt_r_ns_per_item,
	    (uintmax_t)kt_r_prod_ns_per_item, (uintmax_t)kt_r_ns / 1000);
	mtx_lock(&kt_mtx);
	kt_running = false;
	mtx_unlock(&kt_mtx);
	kthread_exit();
}

static int
kt_sysctl_run(SYSCTL_HANDLER_ARGS)
{
	struct thread *td;
	int error, val;

	val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val == 0)
		return (0);
	mtx_lock(&kt_mtx);
	if (kt_running) {
		mtx_unlock(&kt_mtx);
		return (EBUSY);
	}
	kt_running = true;
	strlcpy(kt_state, "running", sizeof(kt_state));
	mtx_unlock(&kt_mtx);
	error = kthread_add(kt_thread, NULL, NULL, &td, 0, 0, "kwq_test");
	if (error != 0) {
		mtx_lock(&kt_mtx);
		kt_running = false;
		strlcpy(kt_state, "idle", sizeof(kt_state));
		mtx_unlock(&kt_mtx);
	}
	return (error);
}
SYSCTL_PROC(_kern_kwq_test, OID_AUTO, run,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, kt_sysctl_run, "I",
    "write 1 to start the selected scenario");

static int
kt_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		kt_calibrate_ticks();
		mtx_init(&kt_mtx, "kwq_test", NULL, MTX_DEF);
		callout_init(&kt_intr_co, 1);
		kt_last_seq = malloc(sizeof(*kt_last_seq) * (mp_maxid + 1),
		    M_KWQ_TEST, M_WAITOK | M_ZERO);
		return (0);
	case MOD_UNLOAD:
		mtx_lock(&kt_mtx);
		if (kt_running) {
			mtx_unlock(&kt_mtx);
			return (EBUSY);
		}
		mtx_unlock(&kt_mtx);
		kt_intr_stop();
		kt_floods_destroy();
		kt_bulk_destroy();
		free(kt_last_seq, M_KWQ_TEST);
		mtx_destroy(&kt_mtx);
		return (0);
	case MOD_QUIESCE:
		return (kt_running ? EBUSY : 0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t kt_mod = { "kwq_test", kt_modevent, NULL };
DECLARE_MODULE(kwq_test, kt_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(kwq_test, 1);
MODULE_DEPEND(kwq_test, kwq, 1, 1, 1);
