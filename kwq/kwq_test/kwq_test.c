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
 *                                             # P4 shape: ifpair
 *   sysctl kern.kwq_test.items=1000 kern.kwq_test.reps=1000
 *   sysctl kern.kwq_test.run=1
 *   sysctl kern.kwq_test                      # poll result_state until
 *                                             # "done" or "fail"
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

/* Results. */
static char	kt_state[16] = "idle";
static char	kt_msg[256] = "";
static uint64_t	kt_r_in, kt_r_out, kt_r_discarded, kt_r_rejected, kt_r_coalesced,
		kt_r_runs, kt_r_ns, kt_r_fifo_errors;
static uint64_t	kt_r_glitches;	/* cpu_ticks() deltas discarded, see kt_tickdelta() */
static uint64_t	kt_r_dups;	/* P0: items run or discarded twice in a rep */
static uint64_t	kt_r_cycles_a, kt_r_cycles_b, kt_r_maxlat_a_us, kt_r_maxlat_b_us,
		kt_r_ns_per_item, kt_r_prod_ns_per_item;
/* ifpair */
static uint64_t	kt_r_enq_slow;		/* enqueues over 2 us */
static uint64_t	kt_r_enq_pct;		/* share of producer CPU time inside kwq_enqueue */
static uint64_t	kt_r_cons_pct;		/* share of consumer CPU time in packet work */
static uint64_t	kt_r_pool_waits;	/* producers found their window empty */

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
static uint64_t	*kt_last_seq;		/* [mp_maxid + 1] */

SYSCTL_NODE(_kern, OID_AUTO, kwq_test, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "kwq test harness");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, scenario, CTLFLAG_RW, kt_scenario,
    sizeof(kt_scenario), "lifecycle fifo notify reject discard sleep | "
    "fairness latency gaming overrun yield cost scale fanin tq_baseline switch_baseline | ifpair");
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
SYSCTL_INT(_kern_kwq_test, OID_AUTO, allow_panic, CTLFLAG_RW, &kt_allow_panic,
    0, "allow scenarios that are expected to panic");
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
    "cpu_ticks() deltas over 40 ms discarded from the cost sums (ticker glitches)");
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
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_enq_slow, CTLFLAG_RD,
    &kt_r_enq_slow, 0, "ifpair: enqueue calls that took over 2 us");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_enq_pct, CTLFLAG_RD,
    &kt_r_enq_pct, 0, "ifpair: percent of producer CPU time spent inside kwq_enqueue");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_cons_pct, CTLFLAG_RD,
    &kt_r_cons_pct, 0, "ifpair: percent of the worker CPUs' wall time spent in packet work");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_pool_waits, CTLFLAG_RD,
    &kt_r_pool_waits, 0, "ifpair: times a sender found its whole window in flight");

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

/*
 * A cpu_ticks() delta for the cost sums.  On a07 (Ampere Altra, ARM
 * generic timer at 25 MHz) cpu_ticks() reads a few ticks backwards now and
 * then, and tc_cpu_ticks() takes that for a 32-bit wrap: +2^32 ticks =
 * 171.8 s.  Anything over 40 ms in one handler item or one enqueue is
 * such a glitch; count it and leave it out.
 */
static inline uint64_t
kt_tickdelta(uint64_t t0)
{
	uint64_t d = cpu_ticks() - t0;

	if (d > cpu_tickrate() / 25) {
		kt_r_glitches++;
		return (0);
	}
	return (d);
}

/*
 * Busy work standing in for the network stack: spin for `ticks' of
 * cpu_ticks().  A glitch (SCHED.md S13, kt_tickdelta above) can only end
 * the spin early.  The loop reads the clock, which on arm64 is a
 * serialising instruction; that is the only memory traffic real stack
 * work would have and this does not, so the model is optimistic about
 * cache pressure and pessimistic about nothing.
 */
static inline void
kt_spin_ticks(uint64_t ticks)
{
	uint64_t t0 = cpu_ticks();

	while (cpu_ticks() - t0 < ticks)
		cpu_spinwait();
}

static inline uint64_t
kt_ns2ticks(uint64_t ns)
{
	return (ns * cpu_tickrate() / 1000000000ULL);
}

static int
kt_other_cpu(int cpu)
{
	int c = cpu;

	do {
		c = (c + 1) % (mp_maxid + 1);
	} while (CPU_ABSENT(c) && c != cpu);
	return (c);
}

/* ================================================================ P0 */

static void
kt_item_handler(struct kwq *q __unused, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;
	struct titem *ti;
	int cpu;
	u_int cnt;

	cpu = curcpu;
	cnt = 0;
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(it);
		KWQ_ITEM_INIT(it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC)
			panic("kwq_test: bad item %p", ti);
		/* Run or discarded, each item exactly once per rep. */
		if (ti->ti_seen == kt_rep_tag)
			atomic_add_64(&kt_dups, 1);
		ti->ti_seen = kt_rep_tag;
		if (n > 0) {
			if (ti->ti_cpu != cpu ||
			    ti->ti_seq <= kt_last_seq[cpu])
				atomic_add_64(&kt_fifo_errors, 1);
			kt_last_seq[cpu] = ti->ti_seq;
			if (kt_cost_us != 0)
				DELAY(kt_cost_us);
		}
		cnt++;
	}
	if (cnt != (u_int)(n < 0 ? -n : n))
		panic("kwq_test: handler count %u != n %d", cnt, n);
	if (n > 0)
		atomic_add_64(&kt_handled, cnt);
	else
		atomic_add_64(&kt_discarded, cnt);
}

static void
kt_sleep_handler(struct kwq *q __unused, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(it);
		KWQ_ITEM_INIT(it);
	}
	if (n > 0)
		pause("kwqtst", 1);	/* must panic under INVARIANTS */
	atomic_add_64(&kt_handled, n < 0 ? -n : n);
}

static void
kt_notify_handler(struct kwq *q __unused, struct kwq_item *head __unused,
    int n, void *ctx __unused)
{
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
	return (kwq_create(name, KWQ_NET, flags | KWQ_F_INACTIVE, &p, fn, ctx));
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
	enum { F_FLOOD, F_BURSTPAUSE, F_IFPAIR, F_NONE } mode;
	u_int		burst;
	u_int		pause_us;
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
			next = KWQ_ITEM_NEXT(it);
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
	t0 = cpu_ticks();
	now = (uint64_t)sbinuptime();
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(it);
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
	t0 = cpu_ticks();
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(it);
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

	t0 = cpu_ticks();
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(it);
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

static void
kt_flood_producer(void *arg)
{
	struct kt_flood *f = arg;
	struct kwq_item *it;
	struct titem *ti;
	uint64_t t0;
	u_int k;
	int error;

	thread_lock(curthread);
	sched_bind(curthread, f->pcpu);
	thread_unlock(curthread);
	if (f->mode == F_IFPAIR) {
		kt_ifpair_producer(f);
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
			KWQ_ITEM_NEXT(tail) = NULL;
			t0 = cpu_ticks();
			error = kwq_enqueue_list(f->q, f->cpu, it, tail, (int)k);
			f->prod_ticks += kt_tickdelta(t0);
			if (error != 0) {
				struct kt_itemq back;

				STAILQ_INIT(&back);
				for (; it != NULL; it = nx) {
					nx = KWQ_ITEM_NEXT(it);
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
		t0 = cpu_ticks();
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
		KWQ_ITEM_NEXT(tail) = NULL;
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
		for (nx = it; nx != NULL; nx = KWQ_ITEM_NEXT(nx)) {
			ti = __containerof(nx, struct titem, ti_item);
			ti->ti_cpu = target;
			ti->ti_seq = now;	/* enqueue stamp: queue latency */
		}
		d = cpu_ticks();
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
				nx = KWQ_ITEM_NEXT(it);
				KWQ_ITEM_INIT(it);
				STAILQ_INSERT_TAIL(&back, it, kwi_link);
			}
			kt_flood_return_list(f, &back, k);
			f->rejected += k;
		} else
			f->produced += k;
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
	wall_ticks = wall_us * cpu_tickrate() / 1000000ULL;
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
	    prod_ticks * 1000000000ULL / cpu_tickrate() / produced : 0;
	/* Senders run (or sleep on their window) for the whole wall time. */
	kt_r_enq_pct = wall_ticks ? prod_ticks * 100 / (wall_ticks * nsend) : 0;
	kt_r_cons_pct = wall_ticks ? spin_ticks * 100 / (wall_ticks * ncons) : 0;
	kt_r_cycles_a = spin_ticks * 1000000000ULL / cpu_tickrate();	/* packet work, ns */
	kt_r_cycles_b = hand_ticks * 1000000000ULL / cpu_tickrate();	/* handler calls, ns */
	snprintf(kt_msg, sizeof(kt_msg), "%u ifaces x %u workers x %u senders, spin %u/%u ns, batch %u: "
	    "%ju pkt/s (%ju..%ju per sender); enqueue %ju ns = %ju%% of sender CPU, %ju over 2 us; "
	    "workers %ju%% in packet work, %ju passes; queue latency mean %ju max %ju us; %ju rejects, %ju window waits",
	    nif, spread, nprod, kt_prod_ns, kt_cons_ns, kt_batch,
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
	    (a->prod_ticks * 1000000000ULL / cpu_tickrate()) / a->produced : 0;
}

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
		next = KWQ_ITEM_NEXT(it);
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

	thread_lock(curthread);
	sched_bind(curthread, f->pcpu);
	thread_unlock(curthread);
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
		t0 = cpu_ticks();
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
	    (f->prod_ticks * 1000000000ULL / cpu_tickrate()) / f->produced : 0;
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

/* ================================================================ runner */

static void
kt_thread(void *arg __unused)
{
	struct timespec t0, t1;

	nanouptime(&t0);
	kt_floods_destroy();
	kt_handled = kt_discarded = kt_nf_runs = kt_nf_seen = kt_nf_state = 0;
	kt_fifo_errors = 0;
	kt_r_in = kt_r_out = kt_r_discarded = kt_r_rejected = kt_r_coalesced = 0;
	kt_r_runs = kt_r_fifo_errors = 0;
	kt_r_cycles_a = kt_r_cycles_b = kt_r_maxlat_a_us = kt_r_maxlat_b_us = 0;
	kt_r_ns_per_item = kt_r_prod_ns_per_item = kt_r_glitches = kt_r_dups = 0;
	kt_r_enq_slow = kt_r_enq_pct = kt_r_cons_pct = kt_r_pool_waits = 0;
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
	else if (strcmp(kt_scenario, "cost") == 0)
		kt_run_cost();
	else if (strcmp(kt_scenario, "tq_baseline") == 0)
		kt_run_tq_baseline();
	else if (strcmp(kt_scenario, "switch_baseline") == 0)
		kt_run_switch_baseline();
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
		mtx_init(&kt_mtx, "kwq_test", NULL, MTX_DEF);
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
		kt_floods_destroy();
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
