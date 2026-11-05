/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq(9) - kernel work queues: queue objects, lifecycle, data path.
 * Workers and the pass live in kwq_worker.c.
 *
 * This is the P0 subset (../PLAN.txt): NET and BULK classes, create/
 * activate/enqueue/notify/drain/destroy, module load/unload.  DRR,
 * quantum and yield are P1; probes, DDB and the full sysctl set are P2.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/rmlock.h>
#include <sys/cpuset.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/pcpu.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/domainset.h>
#include <sys/priority.h>
#include <sys/limits.h>

#include <net/vnet.h>

#include "kwq_sched.h"
#include "kwq_sdt.h"

MALLOC_DEFINE(M_KWQ, "kwq", "kernel work queues");

const char *kwq_class_names[KWQ_NCLASS] = { "net", "bulk", "blocking" };

/* All queues, for unload refusal, name uniqueness and DDB. */
struct kwq_list kwq_all = LIST_HEAD_INITIALIZER(kwq_all);
static struct sx kwq_sx;
SX_SYSINIT(kwq_sx, &kwq_sx, "kwq queues");

/*
 * BULK stealing and scatter (../SCHED.md S16).  The read side of
 * kwq_steal_rm is taken by every steal attempt and every kwq_scatter(),
 * the write side by kwq_create() and kwq_drain() of BULK queues only.
 */
struct kwq_steal_head kwq_steal_list = LIST_HEAD_INITIALIZER(kwq_steal_list);
struct rmlock kwq_steal_rm;
RM_SYSINIT(kwq_steal_rm, &kwq_steal_rm, "kwq steal");
volatile u_int kwq_nstealable;
u_int kwq_steal_on = 1;
u_int kwq_steal_scope = KWQ_TOPO_RINGS;
u_int kwq_steal_scan = 16;
u_int kwq_steal_max = 64;
u_int kwq_steal_wake_at = 2;
u_int kwq_steal_batch = 16;

/*
 * sysctl tree (../KWQ.md S10.7): kern.kwq.{version,ncpu,nqueues,clock,
 * clock_fast}; kern.kwq.<class>.{knobs,priority,limit}; kern.kwq.<class>.cpu.<N>.* per
 * worker; kern.kwq.<class>.queue.<name>.* and .cpu.<N>.* per queue.  User
 * chosen names live only under "queue", one level of their own.
 */
SYSCTL_NODE(_kern, OID_AUTO, kwq, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "kernel work queues");
SYSCTL_NODE(_kern_kwq, OID_AUTO, net, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "NET class: packet-latency work at PI_NET");
SYSCTL_NODE(_kern_kwq, OID_AUTO, bulk, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "BULK class: CPU-heavy non-sleeping work at PI_SOFT");

/* The stealing knobs (../SCHED.md S16), a range each. */
static int
kwq_sysctl_steal_knob(SYSCTL_HANDLER_ARGS)
{
	u_int *var = arg1, lo = (u_int)arg2 >> 16, hi = (u_int)arg2 & 0xffff;
	u_int val = *var;
	int error;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val < lo || val > hi)
		return (EINVAL);
	*var = val;
	return (0);
}
#define	KWQ_RANGE(lo, hi)	(((lo) << 16) | (hi))
SYSCTL_PROC(_kern_kwq_bulk, OID_AUTO, steal,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_steal_on,
    KWQ_RANGE(0, 1), kwq_sysctl_steal_knob, "IU",
    "1: a BULK worker with nothing of its own takes batches of KWQ_F_STEALABLE queues from other CPUs before it sleeps, and producers wake sleeping workers to do so (SCHED.md S16); 0: off");
SYSCTL_PROC(_kern_kwq_bulk, OID_AUTO, steal_scope,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_steal_scope,
    KWQ_RANGE(0, KWQ_TOPO_RINGS), kwq_sysctl_steal_knob, "IU",
    "rings of the cache topology that stealing, the steal wake, kwq_scatter() and KWQ_CPU_ANY look at, nearest first (1: the CPUs sharing the smallest group with this one, the SMT sibling or L2; 8: all); 0: none, every item stays on its CPU");
SYSCTL_PROC(_kern_kwq_bulk, OID_AUTO, steal_scan,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_steal_scan,
    KWQ_RANGE(1, 4096), kwq_sysctl_steal_knob, "IU",
    "other CPUs probed at most by one steal attempt, steal wake, scatter or KWQ_CPU_ANY pick, nearest first");
SYSCTL_PROC(_kern_kwq_bulk, OID_AUTO, steal_max,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_steal_max,
    KWQ_RANGE(1, 4096), kwq_sysctl_steal_knob, "IU",
    "items one steal takes at most (half of the victim's list otherwise); the victim's lock is held while the stealer walks them");
SYSCTL_PROC(_kern_kwq_bulk, OID_AUTO, steal_wake,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_steal_wake_at,
    KWQ_RANGE(0, 4096), kwq_sysctl_steal_knob, "IU",
    "an enqueue that leaves a stealable list this deep, or at a doubling of it, wakes the nearest sleeping BULK worker to steal; 0: never");
SYSCTL_PROC(_kern_kwq_bulk, OID_AUTO, steal_batch,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_steal_batch,
    KWQ_RANGE(1, 4096), kwq_sysctl_steal_knob, "IU",
    "items a pass of a stealable queue takes at most while stealing is on; the rest stays on the live list, where idle workers can take it (a pass's own batch cannot be stolen)");
SYSCTL_UINT(_kern_kwq_bulk, OID_AUTO, nstealable, CTLFLAG_RD,
    __DEVOLATILE(u_int *, &kwq_nstealable), 0, "KWQ_F_STEALABLE queues in existence");

static int kwq_version = 1;
SYSCTL_INT(_kern_kwq, OID_AUTO, version, CTLFLAG_RD, &kwq_version, 0,
    "KPI version (MODULE_VERSION(kwq))");
SYSCTL_INT(_kern_kwq, OID_AUTO, ncpu, CTLFLAG_RD, &mp_ncpus, 0,
    "CPUs with workers");

static int
kwq_sysctl_nqueues(SYSCTL_HANDLER_ARGS)
{
	struct kwq *q;
	int n = 0;

	sx_slock(&kwq_sx);
	LIST_FOREACH(q, &kwq_all, kwq_all)
		n++;
	sx_sunlock(&kwq_sx);
	return (sysctl_handle_int(oidp, &n, 0, req));
}

/*
 * kern.kwq.<class>.queue.<name>.limit: the per-CPU item limit, writable
 * at run time (1..INT_MAX) so a queue's depth can be tuned or swept
 * without recreating it.  A plain store: the admission test reads the
 * value unlocked and any interleaving is one packet admitted or refused
 * under the old bound.
 */
static int
kwq_sysctl_limit(SYSCTL_HANDLER_ARGS)
{
	struct kwq *q = arg1;
	u_int v = q->kwq_limit;
	int error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v == 0 || v > INT_MAX)
		return (EINVAL);
	q->kwq_limit = v;
	return (0);
}
SYSCTL_PROC(_kern_kwq, OID_AUTO, nqueues,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, kwq_sysctl_nqueues,
    "I", "queues currently created (active or not)");
SYSCTL_NODE(_kern_kwq, OID_AUTO, blocking, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "BLOCKING class: work that may sleep (not yet available)");
/* One level each for the workers and for the user-chosen queue names. */
SYSCTL_NODE(_kern_kwq_net, OID_AUTO, cpu, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "workers by CPU");
SYSCTL_NODE(_kern_kwq_net, OID_AUTO, queue, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "queues by name");
SYSCTL_NODE(_kern_kwq_bulk, OID_AUTO, cpu, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "workers by CPU");
SYSCTL_NODE(_kern_kwq_bulk, OID_AUTO, queue, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "queues by name");
SYSCTL_NODE(_kern_kwq_blocking, OID_AUTO, cpu, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "workers by CPU");
SYSCTL_NODE(_kern_kwq_blocking, OID_AUTO, queue, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "queues by name");

/*
 * kern.kwq.<class>.limit: the default per-CPU item limit for queues
 * created with limit = 0, a loader tunable and writable at run time.  0 is
 * refused, as by the per-queue knob: every queue created afterwards would
 * refuse every item.  A refused tunable leaves the default and the kernel
 * prints "Setting sysctl ... failed: 22".  Values above INT_MAX stay
 * accepted and are clamped at kwq_create(), as a client's are.
 */
static u_int kwq_limit[KWQ_NCLASS] = { 4096, 4096, 4096 };

static int
kwq_sysctl_class_limit(SYSCTL_HANDLER_ARGS)
{
	u_int *lp = arg1;
	u_int v = *lp;
	int error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v == 0)
		return (EINVAL);
	*lp = v;
	return (0);
}
SYSCTL_PROC(_kern_kwq_net, OID_AUTO, limit,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_limit[KWQ_NET], 0,
    kwq_sysctl_class_limit, "IU",
    "default per-CPU item limit for queues created with limit = 0; 1..UINT_MAX, clamped to INT_MAX at creation");
SYSCTL_PROC(_kern_kwq_bulk, OID_AUTO, limit,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_limit[KWQ_BULK], 0,
    kwq_sysctl_class_limit, "IU",
    "default per-CPU item limit for queues created with limit = 0; 1..UINT_MAX, clamped to INT_MAX at creation");
SYSCTL_PROC(_kern_kwq_blocking, OID_AUTO, limit,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &kwq_limit[KWQ_BLOCKING],
    0, kwq_sysctl_class_limit, "IU",
    "default per-CPU item limit for queues created with limit = 0; 1..UINT_MAX, clamped to INT_MAX at creation");

/*
 * The classes' worker priorities, loader tunables (kern.kwq.<class>.priority
 * in loader.conf or kenv(1) before the module loads).  Defaults: NET at
 * PI_NET, the hardware interrupt threads' level; BULK at PI_SOFT, where
 * netisr, iflib's queue threads and other software interrupt threads run;
 * BLOCKING at PUSER.  kwq_priority_check() at load confines NET and BULK
 * to [PRI_MIN_ITHD, PRI_MAX_KERN]: their workers hold the class priority
 * for whole passes, and the 4BSD scheduler rewrites the priority of a
 * kernel thread in the timeshare range from its own CPU use
 * (resetpriority_thread()), which ULE never does.  BLOCKING may go up to
 * PRI_MAX_TIMESHARE; at its PUSER default 4BSD schedules its workers as
 * processes, which a sleeping class tolerates.  The share limit's waiting
 * priority (cap_prio) is validated against the class priority when it is
 * set, after this check.  The class order
 * NET <= BULK <= BLOCKING is what the hand-back between classes (SCHED.md
 * S6.4) relies on; a setting that breaks either rule resets all three.
 */
static const int kwq_priority_default[KWQ_NCLASS] = { PI_NET, PI_SOFT, PUSER };
static const int kwq_priority_max[KWQ_NCLASS] =
    { PRI_MAX_KERN, PRI_MAX_KERN, PRI_MAX_TIMESHARE };
static int kwq_priority[KWQ_NCLASS] = { PI_NET, PI_SOFT, PUSER };
SYSCTL_INT(_kern_kwq_net, OID_AUTO, priority, CTLFLAG_RDTUN,
    &kwq_priority[KWQ_NET], 0,
    "scheduler priority of the class's workers (tunable; default PI_NET)");
SYSCTL_INT(_kern_kwq_bulk, OID_AUTO, priority, CTLFLAG_RDTUN,
    &kwq_priority[KWQ_BULK], 0,
    "scheduler priority of the class's workers (tunable; default PI_SOFT)");
SYSCTL_INT(_kern_kwq_blocking, OID_AUTO, priority, CTLFLAG_RDTUN,
    &kwq_priority[KWQ_BLOCKING], 0,
    "scheduler priority of the class's workers (tunable; default PUSER)");

static void
kwq_priority_check(void)
{
	int cls, bad = 0;

	for (cls = 0; cls < KWQ_NCLASS; cls++) {
		if (kwq_priority[cls] < PRI_MIN_ITHD ||
		    kwq_priority[cls] > kwq_priority_max[cls])
			bad = 1;
		if (cls > 0 && kwq_priority[cls] < kwq_priority[cls - 1])
			bad = 1;
	}
	if (!bad)
		return;
	printf("kwq: class priorities net %d bulk %d blocking %d rejected "
	    "(net and bulk in [%d, %d], blocking in [%d, %d], "
	    "net <= bulk <= blocking); using the defaults\n",
	    kwq_priority[KWQ_NET], kwq_priority[KWQ_BULK],
	    kwq_priority[KWQ_BLOCKING], PRI_MIN_ITHD, PRI_MAX_KERN,
	    PRI_MIN_ITHD, PRI_MAX_TIMESHARE);
	for (cls = 0; cls < KWQ_NCLASS; cls++)
		kwq_priority[cls] = kwq_priority_default[cls];
}

static struct sysctl_oid_list *kwq_class_oids[KWQ_NCLASS];
static struct sysctl_oid_list *kwq_cpu_oids[KWQ_NCLASS];	/* <class>.cpu */
static struct sysctl_oid_list *kwq_queue_oids[KWQ_NCLASS];	/* <class>.queue */

/*
 * Scheduler knobs (../SCHED.md S8).  The sysctl handlers validate a write
 * and re-derive the class's struct kwq_sched_knobs, which the workers read
 * directly (plain stores of independent fields; a worker sees each new
 * value at its next round).
 */
struct kwq_sched_knobs kwq_knobs[KWQ_NCLASS];
int kwq_yield_prio[KWQ_NCLASS] = { PUSER, PUSER, PUSER };
/*
 * The share limit's waiting priority (SCHED.md S6.3): where a worker over
 * its allowance waits between passes.  PRI_MIN_IDLE: ULE runs it only when
 * the CPU would otherwise idle.  PRI_MAX_TIMESHARE is the other useful
 * value: the last batch slot, which sched_pickcpu() does not take for an
 * idle CPU.  Anything below the class priority is accepted for experiments.
 */
int kwq_cap_prio[KWQ_NCLASS] = { PRI_MIN_IDLE, PRI_MIN_IDLE, PRI_MIN_IDLE };
static u_int kwq_quantum_us[KWQ_NCLASS] = { 200, 1000, 5000 };
/*
 * The share limit (SCHED.md S6.3) is on by default at an equal share:
 * half of each millisecond in passes at the class priority while anything
 * else is runnable on the CPU, the other half waiting between passes at
 * cap_prio (by default an idle priority).  The
 * knobs are expert overrides; 1000 turns it off.
 */
static u_int kwq_cap_permille[KWQ_NCLASS] = { 500, 500, 500 };
static u_int kwq_cap_period_us[KWQ_NCLASS] = { 1000, 1000, 1000 };
static u_int kwq_penalty_rounds[KWQ_NCLASS] = { 32, 32, 32 };
/*
 * The tick regime's bound L (../KWQ.md S16, PLAN P4e): with it set, a
 * worker whose recent sleeps were denser than L sleeps with L as its
 * timeout and producers place their lists without waking it.  0 = off,
 * the doorbell regime, which is the default because an isolated item must
 * pay one sleep-wake and nothing else (SCHED.md B5a).
 */
u_int kwq_tick_us[KWQ_NCLASS] = { 0, 0, 0 };
/*
 * The idle-class wait for same-CPU producers (../KWQ.md S16, PLAN P4e):
 * with it set, an enqueue from the worker's own CPU does not preempt the
 * producer; the worker is made runnable in the idle class at the class's
 * cap_prio (the share limit's waiting priority, SCHED.md S6.3 - the same
 * knob, not a second one) and is promoted to the class priority when it
 * gets the CPU by itself or when this deadline strikes.  An enqueue from
 * another CPU rings the plain doorbell, because the measured cost of
 * waiting on a cross-CPU wake fell on the sparse sender (a07 +77 %
 * enqueue, WAKE.md).  0 = off, today's behaviour.
 */
u_int kwq_wake_delay_us[KWQ_NCLASS] = { 0, 0, 0 };
u_int kwq_tick_urgent_us[KWQ_NCLASS] = { 0, 0, 0 };
static u_int kwq_budget_check_every[KWQ_NCLASS] = { 8, 8, 8 };
static struct sysctl_ctx_list kwq_knob_sysctl;

struct kwq_knob {
	u_int		*var;
	u_int		lo, hi;
	int		cls;
	const char	*name, *descr;
};

static void
kwq_knobs_apply(int cls)
{
	struct kwq_sched_knobs *k = &kwq_knobs[cls];

	k->quantum_ns = (uint64_t)kwq_quantum_us[cls] * 1000;
	k->cap_permille = kwq_cap_permille[cls];
	k->cap_period_ns = (uint64_t)kwq_cap_period_us[cls] * 1000;
	k->cap_allow_ns = k->cap_period_ns * k->cap_permille / 1000;
	k->boost_weighted = true;
	k->penalty_rounds = kwq_penalty_rounds[cls];
	k->budget_check_every = kwq_budget_check_every[cls];
}

static int
kwq_sysctl_knob(SYSCTL_HANDLER_ARGS)
{
	struct kwq_knob *kn = arg1;
	u_int val;
	int error;

	val = *kn->var;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val < kn->lo || val > kn->hi)
		return (EINVAL);
	*kn->var = val;
	kwq_knobs_apply(kn->cls);
	return (0);
}

static int
kwq_sysctl_yield_prio(SYSCTL_HANDLER_ARGS)
{
	int cls = (int)(intptr_t)arg1, val, error;

	val = kwq_yield_prio[cls];
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	/* Anything from the class's own priority down to the last timeshare. */
	if (val < kwq_class_priority(cls) || val > PRI_MAX_TIMESHARE)
		return (EINVAL);
	kwq_yield_prio[cls] = val;
	return (0);
}

static int
kwq_sysctl_cap_prio(SYSCTL_HANDLER_ARGS)
{
	int cls = (int)(intptr_t)arg1, val, error;

	val = kwq_cap_prio[cls];
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	/* Strictly below the class priority, down to the last idle slot. */
	if (val <= kwq_class_priority(cls) || val > PRI_MAX_IDLE)
		return (EINVAL);
	kwq_cap_prio[cls] = val;
	return (0);
}

#define	KWQ_NKNOBS	8
static struct kwq_knob kwq_knob_desc[KWQ_NCLASS][KWQ_NKNOBS];

static void
kwq_knobs_register(void)
{
	static const struct { const char *name, *descr; u_int lo, hi; } d[KWQ_NKNOBS] = {
		{ "quantum_us", "CPU time a queue of weight 1 may consume per round", 10, 1000000 },
		{ "cap_permille", "share of its CPU the worker may spend in passes per cap_period_us while other threads are runnable, in 1/1000; the rest of the period it waits between passes at cap_prio; 1000 = no limit (SCHED.md S6.3)", 1, 1000 },
		{ "cap_period_us", "period over which cap_permille is taken", 100, 1000000 },
		{ "penalty_rounds", "debt clamp in quanta: rounds an overrunning queue is parked at most", 1, 1024 },
		{ "budget_check_every", "kwq_budget_left() reads the clock at least every this many calls; 1 = every call", 1, 64 },
		{ "wake_delay_us", "the idle-class wait for same-CPU producers (KWQ.md S16): an enqueue from the worker's own CPU makes the worker runnable in the idle class at cap_prio and this is the deadline by which it is promoted to the class priority; an enqueue from another CPU rings the plain doorbell; 0 = off", 0, 1000000 },
		{ "tick_us", "the tick regime's latency bound L (KWQ.md S16): after three consecutive sleeps shorter than L the worker sleeps with L as its timeout and producers place their lists without waking it, until a tick serves one element or none; 0 = off, the doorbell regime", 0, 100000 },
		{ "tick_urgent_us", "inside the tick regime, an urgent item (kwq_enqueue_urgent()) that finds the worker asleep wakes it this much later instead of at once, so that urgent items within this bound share one wake; at or above tick_us the tick serves them; 0 = at once", 0, 100000 },
	};
	u_int *vars[KWQ_NKNOBS];
	int cls, i;

	sysctl_ctx_init(&kwq_knob_sysctl);
	for (cls = 0; cls < KWQ_NCLASS; cls++) {
		vars[0] = &kwq_quantum_us[cls]; vars[1] = &kwq_cap_permille[cls];
		vars[2] = &kwq_cap_period_us[cls];
		vars[3] = &kwq_penalty_rounds[cls]; vars[4] = &kwq_budget_check_every[cls];
		vars[5] = &kwq_wake_delay_us[cls]; vars[6] = &kwq_tick_us[cls];
		vars[7] = &kwq_tick_urgent_us[cls];
		for (i = 0; i < KWQ_NKNOBS; i++) {
			struct kwq_knob *kn = &kwq_knob_desc[cls][i];

			kn->var = vars[i]; kn->lo = d[i].lo; kn->hi = d[i].hi;
			kn->cls = cls; kn->name = d[i].name; kn->descr = d[i].descr;
			SYSCTL_ADD_PROC(&kwq_knob_sysctl, kwq_class_oids[cls], OID_AUTO,
			    kn->name, CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
			    kn, 0, kwq_sysctl_knob, "IU", kn->descr);
		}
		SYSCTL_ADD_PROC(&kwq_knob_sysctl, kwq_class_oids[cls], OID_AUTO,
		    "yield_prio", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
		    (void *)(intptr_t)cls, 0, kwq_sysctl_yield_prio, "I",
		    "priority the worker yields at after each round (PUSER=56; 223 lets every user thread run a full slice)");
		SYSCTL_ADD_PROC(&kwq_knob_sysctl, kwq_class_oids[cls], OID_AUTO,
		    "cap_prio", CTLTYPE_INT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
		    (void *)(intptr_t)cls, 0, kwq_sysctl_cap_prio, "I",
		    "priority the worker waits at between passes while over its share limit allowance, never the priority of a pass (PRI_MIN_IDLE=224: runs only when the CPU would otherwise idle; PRI_MAX_TIMESHARE=223: last batch slot, no idle-affinity shortcut); class priority+1..255 (SCHED.md S6.3)");
		kwq_knobs_apply(cls);
	}
}

int
kwq_class_priority(enum kwq_class cls)
{
	return (kwq_priority[cls]);
}

const char *
kwq_name(const struct kwq *q)
{
	return (q->kwq_name);
}

enum kwq_class
kwq_class(const struct kwq *q)
{
	return (q->kwq_class);
}

/*
 * CPU selection (../KWQ.md S3 "Choosing the CPU").
 *
 * hash % (mp_maxid + 1) is TCP's per-CPU timer mapping (inp_to_cpuid()),
 * so on a densely numbered machine a connection's kwq worker and its
 * timers share a CPU.  CPU ids may have holes (hint.lapic.N.disabled,
 * firmware that skips a core): TCP then falls back to the caller's CPU,
 * which for a queue would spread one flow over CPUs and reorder it.
 * kwq instead remaps every absent id to a fixed present one, the next
 * present id upwards with wrap-around, through a table built at load.
 * Present ids map to themselves, so dense machines see exactly TCP's
 * mapping and sparse ones keep every flow on one CPU.  The sparse path
 * is untested as of 2026-09-30 (no such machine at hand); the KASSERTs
 * in kwq_cpu_remap_init() are what stands behind it.
 */
static int kwq_cpu_remap[MAXCPU];

static void
kwq_cpu_remap_init(void)
{
	int cpu, first, next;

	first = -1;
	CPU_FOREACH(cpu) {
		first = cpu;
		break;
	}
	KASSERT(first >= 0, ("kwq: no CPU present"));
	next = first;
	for (cpu = (int)mp_maxid; cpu >= 0; cpu--) {
		if (!CPU_ABSENT(cpu))
			next = cpu;
		kwq_cpu_remap[cpu] = next;	/* itself, or the next present one */
	}
	for (cpu = 0; cpu <= (int)mp_maxid; cpu++)
		KASSERT(!CPU_ABSENT(kwq_cpu_remap[cpu]),
		    ("kwq: cpu %d remapped to absent %d", cpu, kwq_cpu_remap[cpu]));
}

int
kwq_cpu_for_hash(uint32_t hash)
{
	return (kwq_cpu_remap[hash % (mp_maxid + 1)]);
}

/*
 * KWQ_CPU_ANY (../KWQ.md S3): unordered CPU-heavy work goes where it starts
 * soonest.  First the nearest CPU in cache-topology order whose BULK worker
 * sleeps with this queue's list empty there: the item starts at once and
 * the caller keeps running, which is what a producer of many items wants
 * (on its own CPU the worker would preempt it at the first item and the
 * work would be serial).  Then the caller's own CPU if its list is empty,
 * then the shallowest of the caller's list and the next KWQ_PICK_PROBES in
 * topology order.  The idle mask and the depths are read unlocked, as
 * hints; a wrong guess is what stealing corrects.  The search reaches as
 * far as steal_scope and steal_scan allow (kwq_topo_reach()); the P0 pick
 * probed four CPUs of the NUMA domain by id.
 */
#define	KWQ_PICK_PROBES	4	/* other CPUs' depths read when no worker is idle */

static int
kwq_pick_any(struct kwq *q)
{
	u_int i, reach, depth, bestdepth;
	int me, c, best;

	me = curcpu;
	reach = kwq_topo_reach(me);
	for (i = 0; i < reach; i++) {
		c = kwq_topo_order[me][i];
		if (CPU_ISSET(c, &kwq_bulk_idle) && q->kwq_pcpu[c]->kc_depth == 0)
			return (c);
	}
	best = me;
	bestdepth = q->kwq_pcpu[me]->kc_depth;
	for (i = 0; i < reach && i < KWQ_PICK_PROBES && bestdepth != 0; i++) {
		c = kwq_topo_order[me][i];
		depth = q->kwq_pcpu[c]->kc_depth;
		if (depth < bestdepth) {
			best = c;
			bestdepth = depth;
		}
	}
	return (best);
}

/*
 * The steal wake's trigger (../SCHED.md S16): did a stealable list grow
 * from `old' to `new' across steal_wake or one of its doublings?  A burst
 * onto one busy CPU then wakes a helper at 2, 4, 8, ... items, a handful
 * of wakes however long the burst, each helper stealing half of what it
 * finds.
 */
static inline bool
kwq_steal_crossed(u_int old, u_int new)
{
	u_int t = kwq_steal_wake_at;

	if (t == 0 || new < t || !kwq_steal_on)
		return (false);
	while (t <= old && t < (1u << 31))
		t <<= 1;
	return (t > old && t <= new);
}

static inline int
kwq_target_cpu(struct kwq *q, int cpu)
{
	if (cpu == KWQ_CPU_ANY) {
		if (q->kwq_class != KWQ_BULK)
			return (-1);
		if (kwq_bulk_place == KWQ_PLACE_SHARD)
			return (kwq_pick_shard(q));
		if (kwq_bulk_place == KWQ_PLACE_GOSSIP)
			return (kwq_pick_gossip(q));
		return (kwq_pick_any(q));
	}
	if (__predict_false(cpu < 0 || cpu > (int)mp_maxid || CPU_ABSENT(cpu)))
		return (curcpu);
	if (__predict_false(kwq_bulk_place != KWQ_PLACE_TODAY) &&
	    (q->kwq_flags & KWQ_F_STEALABLE) != 0)
		return (kwq_bulk_place == KWQ_PLACE_SHARD ? kwq_shard_spill(q, cpu) :
		    kwq_gossip_spill(q, cpu));	/* the spill (S2.6, K11) */
	return (cpu);
}

/*
 * Called with kc locked after something was added: stamp the start of a
 * burst and ring the doorbell if the worker is not already scheduled for
 * this (queue, CPU).  One lock hold covers publication and wakeup, so a
 * wakeup can never be lost (../../NOTES.md, the if_pair invariant).
 */
static inline bool
kwq_kc_added(struct kwq_cpu *kc, u_int added, int ring)
{
	if (kc->kc_depth == added)
		kc->kc_empty_since = sbinuptime();
	if (kc->kc_state == KWQ_CPU_IDLE) {
		kc->kc_state = KWQ_CPU_WAKING;
		kwq_doorbell(kc, ring);
		return (true);
	}
	/*
	 * The list is already placed, so no doorbell is owed; an urgent item
	 * added behind another one is owed a wake all the same, which is the
	 * whole point of the exemption (../KWQ.md S16).
	 */
	if (__predict_false(ring != KWQ_RING_PLAIN) &&
	    kc->kc_state == KWQ_CPU_WAKING)
		return (kwq_urgent_nudge(kc, ring));
	return (false);
}

/* kwq_kc_added() for kwq_worker.c's steal: kc locked, `added' appended. */
bool
kwq_kc_place(struct kwq_cpu *kc, u_int added)
{
	return (kwq_kc_added(kc, added, KWQ_RING_PLAIN));
}

/*
 * Data path.
 */
/*
 * One item.  `urgent' marks an item whose latency the producer will not
 * trade for the opt-in regimes of ../KWQ.md S16: it rings the doorbell
 * past the tick regime and past the idle-class wait, as a notifier, a
 * drain and a cancel do, except that in the tick regime tick_urgent_us,
 * when set, delays its wake by at most that bound so that the urgent
 * items within it share one.  It is for a small, latency-bearing item among
 * big ones (if_pair's net.link.pair.urgent_bytes), never for the common
 * case: an urgent item costs a wake, which is what the regimes save.
 */
static inline int
kwq_enqueue_one(struct kwq *q, int cpu, struct kwq_item *it, bool urgent)
{
	struct kwq_cpu *kc;
	uint64_t mw;
	u_int depth;
	bool sw, woke __unused;	/* SDT argument */

	KASSERT(it->kwi_link.stqe_next == NULL,
	    ("kwq_enqueue: item %p already linked", it));
	cpu = kwq_target_cpu(q, cpu);
	if (__predict_false(cpu < 0))
		return (EINVAL);
	kc = q->kwq_pcpu[cpu];

	kwq_kc_lock(kc);
	if (__predict_false(!q->kwq_active)) {
		kc->kc_rejected++;
		kwq_kc_unlock(kc);
		SDT_PROBE3(kwq, , , reject, q, cpu, ENXIO);
		return (ENXIO);
	}
	if (__predict_false(kc->kc_depth >= q->kwq_limit)) {
		kc->kc_rejected++;
		kwq_kc_unlock(kc);
		SDT_PROBE3(kwq, , , reject, q, cpu, ENOBUFS);
		return (ENOBUFS);
	}
	kwq_list_append(kc, it, false);
	depth = ++kc->kc_depth;
	woke = kwq_kc_added(kc, 1, urgent ? KWQ_RING_ITEM : KWQ_RING_PLAIN);
	/*
	 * The steal wake is decided, and its mailbox word built, under the
	 * list lock: after the unlock a drain and destroy of q may complete
	 * (the items just added run elsewhere), so nothing of q is read past
	 * it.
	 */
	sw = __predict_false(q->kwq_flags & KWQ_F_STEALABLE) &&
	    kwq_steal_crossed(depth - 1, depth);
	mw = sw ? kwq_steal_word(kc) : 0;
	kwq_kc_unlock(kc);
	SDT_PROBE4(kwq, , , enqueue, q, cpu, depth, woke);
	if (__predict_false(sw))
		kwq_steal_wake(cpu, mw);
	return (0);
}

int
kwq_enqueue(struct kwq *q, int cpu, struct kwq_item *it)
{
	return (kwq_enqueue_one(q, cpu, it, false));
}

int
kwq_enqueue_urgent(struct kwq *q, int cpu, struct kwq_item *it)
{
	return (kwq_enqueue_one(q, cpu, it, true));
}

int
kwq_enqueue_list(struct kwq *q, int cpu, struct kwq_item *head,
    struct kwq_item *tail, int n)
{
	struct kwq_cpu *kc;
	uint64_t mw;
	u_int depth;
	bool sw, woke __unused;	/* SDT argument */

	KASSERT(n > 0 && head != NULL && tail != NULL,
	    ("kwq_enqueue_list: bad list %p %p %d", head, tail, n));
	KASSERT(tail->kwi_link.stqe_next == NULL,
	    ("kwq_enqueue_list: tail %p not terminated", tail));
	cpu = kwq_target_cpu(q, cpu);
	if (__predict_false(cpu < 0))
		return (EINVAL);
	kc = q->kwq_pcpu[cpu];


	kwq_kc_lock(kc);
	if (__predict_false(!q->kwq_active)) {
		kc->kc_rejected += n;
		kwq_kc_unlock(kc);
		SDT_PROBE3(kwq, , , reject, q, cpu, ENXIO);
		return (ENXIO);
	}
	/* Overflow-safe form of depth + n > limit; also guards LIMIT_NONE. */
	if (__predict_false((u_int)n > q->kwq_limit - kc->kc_depth)) {
		kc->kc_rejected += n;
		kwq_kc_unlock(kc);
		SDT_PROBE3(kwq, , , reject, q, cpu, ENOBUFS);
		return (ENOBUFS);
	}
	kwq_list_concat(kc, head, tail);
	kc->kc_depth += n;
	depth = kc->kc_depth;
	woke = kwq_kc_added(kc, n, KWQ_RING_PLAIN);
	sw = __predict_false(q->kwq_flags & KWQ_F_STEALABLE) &&
	    kwq_steal_crossed(depth - n, depth);	/* under the lock, as above */
	mw = sw ? kwq_steal_word(kc) : 0;
	kwq_kc_unlock(kc);
	SDT_PROBE4(kwq, , , enqueue, q, cpu, depth, woke);	/* once per call */
	if (__predict_false(sw))
		kwq_steal_wake(cpu, mw);
	return (0);
}

/*
 * Handler only: put already-admitted leftovers back at the HEAD of the
 * current CPU's list so FIFO holds against items enqueued meanwhile.
 * Never counts against the limit.  A NULL tail means "up to the end of
 * the list this pass handed me": the worker recorded that tail, so the
 * handler need not walk the remainder to find it.
 */
void
kwq_requeue(struct kwq *q, struct kwq_item *head, struct kwq_item *tail,
    int n)
{
	struct kwq_worker *kw;
	struct kwq_cpu *kc;

	KASSERT(n > 0 && head != NULL,
	    ("kwq_requeue: bad list %p %p %d", head, tail, n));
	kw = kwq_workers[q->kwq_class][curcpu];
	KASSERT(kw != NULL && kw->kw_td == curthread && kw->kw_cur != NULL &&
	    kw->kw_cur->kc_q == q,
	    ("kwq_requeue: not called from a handler of %s", q->kwq_name));
	kc = kw->kw_cur;
	if (tail == NULL) {
		tail = kw->kw_pass_tail;
		KASSERT(tail != NULL, ("kwq_requeue: %s: no list this pass",
		    q->kwq_name));
	}

	kwq_kc_lock(kc);
	kwq_list_prepend(kc, head, tail);
	kc->kc_depth += n;
	kc->kc_requeued += n;
	kwq_list_check(kc, "requeue");
	kwq_kc_unlock(kc);
}

/*
 * Handler only: nanoseconds of this pass's budget still unspent, 0 when
 * the quantum is exhausted, a higher class waits for the CPU, or the
 * share limit's allowance is spent and something else is runnable
 * (SCHED.md S4.4, S6.4, S6.3), telling the handler to requeue its
 * remainder.  The last is the only way the limit reaches into a pass:
 * the pass itself runs at the class priority to its end.
 */
uint64_t
kwq_budget_left(struct kwq *q)
{
	struct kwq_worker *kw;
	uint64_t left;

	kw = kwq_workers[q->kwq_class][curcpu];
	KASSERT(kw != NULL && kw->kw_td == curthread && kw->kw_cur != NULL &&
	    kw->kw_cur->kc_q == q,
	    ("kwq_budget_left: not called from a handler of %s", q->kwq_name));
	if (kwq_higher_waiting(q->kwq_class)) {
		SDT_PROBE2(kwq, , , budget__hit, q, curcpu);
		return (0);
	}
	if (__predict_false(ks_cap_spent(kw)) && sched_runnable()) {
		kw->kw_cap_cuts++;
		SDT_PROBE2(kwq, , , budget__hit, q, curcpu);
		return (0);
	}
	if (!ks_budget_need_clock(kw, &left))	/* S4.4: the estimate */
		return (left);
	left = ks_budget_left(kw, kwq_cputime_ns(kw));
	if (left == 0)
		SDT_PROBE2(kwq, , , budget__hit, q, curcpu);
	return (left);
}

/*
 * kwq_scatter() (../KWQ.md S3, ../SCHED.md S16): the dispatch_apply and
 * dispatch_group_notify pair.  The targets are the caller's CPU and the
 * idle BULK workers near it, at most KWQ_SCATTER_MAX and never more than
 * there are items; the items are cut in order into one chunk per target,
 * sizes differing by one at most, and the caller's own chunk goes last,
 * since its worker preempts the caller at that doorbell.  All or nothing:
 * the room on every target is checked first (unlocked; the appends are
 * exempt from the limit, as a requeue is, so the bound holds as checked,
 * not to the item), and the read lock on kwq_steal_rm from the activity
 * check to the last append keeps a drain from sweeping between two
 * chunks.  The group is armed before the first chunk can run.
 */
#define	KWQ_SCATTER_MAX	64

int
kwq_scatter(struct kwq *q, struct kwq_group *g, struct kwq_item *list, int n,
    void (*done)(void *), void *arg)
{
	struct rm_priotracker tr;
	struct kwq_cpu *kc;
	struct kwq_item *head, *tail, *next;
	int tgt[KWQ_SCATTER_MAX];
	int ntgt, me, c, i, j, k, cnt, chunk, extra;
	u_int reach, depth;

	if (q->kwq_class != KWQ_BULK || g == NULL || done == NULL ||
	    list == NULL || n <= 0)
		return (EINVAL);
	me = curcpu;
	tgt[0] = me;
	ntgt = 1;
	reach = kwq_topo_reach(me);
	for (i = 0; i < (int)reach && ntgt < n && ntgt < KWQ_SCATTER_MAX; i++) {
		c = kwq_topo_order[me][i];
		if (kwq_bulk_idle_hint(c))	/* the mode's idle view */
			tgt[ntgt++] = c;
	}
	chunk = n / ntgt;
	extra = n % ntgt;
	for (i = 0; i < ntgt; i++) {
		cnt = chunk + (i < extra ? 1 : 0);
		kc = q->kwq_pcpu[tgt[i]];
		depth = kc->kc_depth;
		if (depth > q->kwq_limit || (u_int)cnt > q->kwq_limit - depth) {
			kwq_kc_lock(kc);
			kc->kc_rejected += n;
			kwq_kc_unlock(kc);
			SDT_PROBE3(kwq, , , reject, q, tgt[i], ENOBUFS);
			return (ENOBUFS);
		}
	}
	g->kg_n = n;
	g->kg_done = done;
	g->kg_arg = arg;
	atomic_store_rel_int(&g->kg_left, n);

	rm_rlock(&kwq_steal_rm, &tr);
	if (!atomic_load_acq_int(&q->kwq_active)) {
		rm_runlock(&kwq_steal_rm, &tr);
		kc = q->kwq_pcpu[me];
		kwq_kc_lock(kc);
		kc->kc_rejected += n;
		kwq_kc_unlock(kc);
		SDT_PROBE3(kwq, , , reject, q, me, ENXIO);
		return (ENXIO);
	}
	head = list;
	for (j = 1; j <= ntgt; j++) {
		i = j % ntgt;		/* the others first, the caller's own last */
		cnt = chunk + (i < extra ? 1 : 0);
		tail = head;
		for (k = 1; k < cnt; k++)
			tail = tail->kwi_link.stqe_next;
		KASSERT(tail != NULL, ("kwq_scatter: %s: list shorter than %d",
		    q->kwq_name, n));
		next = tail->kwi_link.stqe_next;
		tail->kwi_link.stqe_next = NULL;
		kc = q->kwq_pcpu[tgt[i]];
		kwq_kc_lock(kc);
		kwq_list_concat(kc, head, tail);
		kc->kc_depth += cnt;
		kc->kc_scattered += cnt;
		kwq_list_check(kc, "scatter");
		depth = kc->kc_depth;
		(void)kwq_kc_added(kc, cnt, KWQ_RING_PLAIN);
		kwq_kc_unlock(kc);
		SDT_PROBE4(kwq, , , enqueue, q, tgt[i], depth, false);
		head = next;
	}
	rm_runlock(&kwq_steal_rm, &tr);
	KASSERT(head == NULL, ("kwq_scatter: %s: list longer than %d",
	    q->kwq_name, n));
	return (0);
}

/*
 * Handler side of a scatter: n of the group's items are finished.  The
 * release fence orders the handler's work before the count, the acquire
 * fence the count before done(), so done() sees every item's results.
 */
void
kwq_group_leave(struct kwq_group *g, int n)
{
	u_int old;

	KASSERT(n > 0 && (u_int)n <= g->kg_left,
	    ("kwq_group_leave: %d of %u left", n, g->kg_left));
	atomic_thread_fence_rel();
	old = atomic_fetchadd_int(&g->kg_left, -(u_int)n);
	if (old == (u_int)n) {
		atomic_thread_fence_acq();
		g->kg_done(g->kg_arg);
	}
}

/*
 * Signals (../KWQ.md S3 kwq_notify).
 */
void
kwq_notifier_init(struct kwq_notifier *nf, int cpu)
{
	KASSERT(cpu != KWQ_CPU_ANY, ("kwq_notifier_init: KWQ_CPU_ANY"));
	if (cpu < 0 || cpu > (int)mp_maxid || CPU_ABSENT(cpu))
		cpu = curcpu;
	KWQ_ITEM_INIT(&nf->kn_item);
	nf->kn_cpu = cpu;
	nf->kn_state = KWQ_NF_IDLE;
}

bool
kwq_notify(struct kwq *q, struct kwq_notifier *nf)
{
	struct kwq_cpu *kc;

	KASSERT((q->kwq_flags & KWQ_F_STEALABLE) == 0,
	    ("kwq_notify: %s is KWQ_F_STEALABLE", q->kwq_name));
	if (__predict_false(q->kwq_flags & KWQ_F_STEALABLE))
		return (false);
	kc = q->kwq_pcpu[nf->kn_cpu];

	kwq_kc_lock(kc);
	if (__predict_false(!q->kwq_active)) {
		kc->kc_rejected++;
		kwq_kc_unlock(kc);
		SDT_PROBE3(kwq, , , reject, q, nf->kn_cpu, ENXIO);
		return (false);
	}
	if (KWQ_NF_ST(nf->kn_state) != KWQ_NF_IDLE) {
		if (KWQ_NF_ST(nf->kn_state) == KWQ_NF_PENDING)
			kc->kc_coalesced++;
		kwq_kc_unlock(kc);
		return (false);
	}
	nf->kn_state = KWQ_NF_SET(nf->kn_state, KWQ_NF_PENDING);
	kwq_list_append(kc, &nf->kn_item, true);
	kc->kc_depth++;
	kc->kc_nnotify++;
	kwq_kc_added(kc, 1, KWQ_RING_SIGNAL);	/* no regime holds it back */
	kwq_kc_unlock(kc);
	return (true);
}

/*
 * Detach-time: after return the notifier is neither queued nor about to
 * run, and stays inert until kwq_notifier_init() again.  A pass that
 * already holds it in its batch skips it (kwq_worker.c) and we wait for
 * that pass to end.
 */
/*
 * Called by KWQ_ITEM_NEXT(q, it) when the handler fetches a notifier's
 * successor: the pending -> idle transition, under the list lock that
 * kwq_notify() and kwq_notify_cancel() use for the same word.  A
 * cancelled notifier becomes dead instead (refusing signals until
 * re-init, as before), and the canceller sleeping on kc is woken.
 */
void
kwq_notifier_consumed(struct kwq *q, struct kwq_item *it)
{
	struct kwq_notifier *nf = __containerof(it, struct kwq_notifier, kn_item);
	struct kwq_worker *kw __diagused;
	struct kwq_cpu *kc;

	kw = kwq_workers[q->kwq_class][curcpu];
	KASSERT(kw != NULL && kw->kw_td == curthread && kw->kw_cur != NULL &&
	    kw->kw_cur->kc_q == q,
	    ("kwq_notifier_consumed: not from a handler of %s", q->kwq_name));
	kc = q->kwq_pcpu[nf->kn_cpu];
	kwq_kc_lock(kc);
	KASSERT(KWQ_NF_ST(nf->kn_state) != KWQ_NF_IDLE,
	    ("kwq_notifier_consumed: %s: notifier %p idle", q->kwq_name, nf));
	KASSERT(kc->kc_nnotify > 0,
	    ("kwq_notifier_consumed: %s: no notifier pending on cpu %d", q->kwq_name, nf->kn_cpu));
	/* Idle again, or dead if it was cancelled: a signal is refused until re-init. */
	nf->kn_state = KWQ_NF_BUMP(nf->kn_state,
	    KWQ_NF_ST(nf->kn_state) == KWQ_NF_CANCELLED ? KWQ_NF_DEAD : KWQ_NF_IDLE);
	kc->kc_nnotify--;
	kc->kc_signals++;
	if (kc->kc_waiters != 0)
		wakeup(kc);
	kwq_kc_unlock(kc);
}

/*
 * Cancel: mark the notifier and wait until the handler has consumed it,
 * which happens in the pass that reaches it.  If its CPU's list is idle
 * no pass is coming on its own, so ring the doorbell; the pass then
 * delivers the whole list, which is real work anyway.  The handler may
 * see the cancelled notifier once more before consuming it; the cancel
 * returns only after that.
 */
void
kwq_notify_cancel(struct kwq *q, struct kwq_notifier *nf)
{
	struct kwq_cpu *kc;

	KASSERT(THREAD_CAN_SLEEP(), ("kwq_notify_cancel: non-sleepable"));
	kc = q->kwq_pcpu[nf->kn_cpu];

	kwq_kc_lock(kc);
	/*
	 * Pending: cancelled now, dead when the handler consumes it.  Idle,
	 * which includes "consumed, handler still running": dead at once.
	 * Either way the notifier refuses signals until re-init, as the
	 * two-list design's cancel did regardless of the state it found.
	 */
	if (KWQ_NF_ST(nf->kn_state) == KWQ_NF_PENDING)
		nf->kn_state = KWQ_NF_SET(nf->kn_state, KWQ_NF_CANCELLED);
	else if (KWQ_NF_ST(nf->kn_state) == KWQ_NF_IDLE)
		nf->kn_state = KWQ_NF_SET(nf->kn_state, KWQ_NF_DEAD);
	/*
	 * Wait for the consumption, then for the pass in progress on this
	 * CPU to end: consumption happens when the handler fetches the
	 * notifier's successor, before it acts on it, so the handler may
	 * still be using the client's state when the word turns idle.  A
	 * run that is under way when we are called is covered by the same
	 * wait, as in the two-list design.
	 */
	while (KWQ_NF_ST(nf->kn_state) == KWQ_NF_CANCELLED ||
	    kc->kc_state == KWQ_CPU_RUNNING) {
		if (kc->kc_state == KWQ_CPU_IDLE) {
			kc->kc_state = KWQ_CPU_WAKING;
			kwq_doorbell(kc, KWQ_RING_SIGNAL);	/* a cancel waits for no regime */
		}
		kwq_kc_wait(kc, "kwqncl");
	}
	kwq_kc_unlock(kc);
}

/*
 * Lifecycle (../KWQ.md S2).
 */
/*
 * Queue names: 1 .. KWQ_NAMELEN-1 characters from [A-Za-z0-9_-].  They
 * become sysctl node names verbatim under kern.kwq.<class>.queue, so no
 * '.', '/', spaces or anything else that reads as structure or is not
 * printable ASCII (../KWQ.md S2, S10.7).
 */
bool
kwq_name_valid(const char *name)
{
	size_t i;
	char c;

	if (name == NULL || name[0] == '\0')
		return (false);
	for (i = 0; name[i] != '\0'; i++) {
		if (i >= KWQ_NAMELEN - 1)
			return (false);
		c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return (false);
	}
	return (true);
}

static int
kwq_sysctl_qstate(SYSCTL_HANDLER_ARGS)
{
	static const char *names[] = { "inactive", "active", "draining", "drained" };
	struct kwq *q = arg1;
	char buf[12];

	strlcpy(buf, names[q->kwq_state & 3], sizeof(buf));
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* write 1: zero maxlat_ns and maxdepth on every CPU (KWQ.md S10.7) */
static int
kwq_sysctl_reset(SYSCTL_HANDLER_ARGS)
{
	struct kwq *q = arg1;
	struct kwq_cpu *kc;
	int cpu, val = 0, error;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val != 1)
		return (EINVAL);
	CPU_FOREACH(cpu) {
		kc = q->kwq_pcpu[cpu];
		kc->kc_maxlat_ns = 0;
		kc->kc_maxdepth = 0;
	}
	return (0);
}

/*
 * Register kern.kwq.<class>.queue.<name> and its cpu.<N> children.  The
 * "queue" level holds nothing but queue names, and kwq_create() has
 * already refused a duplicate, so a collision cannot happen; the checks
 * stay because sysctl(9) would otherwise merge two nodes of one name or
 * silently drop leaves, which is exactly what hid the first version's
 * flat tree problem (KWQ.md S10.7, 2026-09-29).
 */
static int
kwq_sysctl_register(struct kwq *q)
{
	struct sysctl_oid *qoid, *cpuoid, *coid;
	struct sysctl_oid_list *cchildren;
	struct kwq_cpu *kc;
	char cname[16];
	int cpu;

	sysctl_ctx_init(&q->kwq_sysctl);
	qoid = SYSCTL_ADD_NODE(&q->kwq_sysctl, kwq_queue_oids[q->kwq_class],
	    OID_AUTO, q->kwq_name, CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "kwq queue");
	if (qoid == NULL || qoid->oid_refcnt > 1) {
		printf("kwq: %s: sysctl node kern.kwq.%s.queue.%s already exists\n",
		    q->kwq_name, kwq_class_names[q->kwq_class], q->kwq_name);
		sysctl_ctx_free(&q->kwq_sysctl);
		return (EEXIST);
	}
	SYSCTL_ADD_PROC(&q->kwq_sysctl, SYSCTL_CHILDREN(qoid), OID_AUTO,
	    "limit", CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, q, 0,
	    kwq_sysctl_limit, "IU",
	    "per-CPU item limit in effect (INT_MAX for KWQ_LIMIT_NONE); writable, 1..INT_MAX");
	SYSCTL_ADD_UINT(&q->kwq_sysctl, SYSCTL_CHILDREN(qoid), OID_AUTO,
	    "weight", CTLFLAG_RD, &q->kwq_weight, 0, "DRR weight");
	SYSCTL_ADD_U32(&q->kwq_sysctl, SYSCTL_CHILDREN(qoid), OID_AUTO,
	    "flags", CTLFLAG_RD, &q->kwq_flags, 0, "KWQ_F_* flags");
	SYSCTL_ADD_PROC(&q->kwq_sysctl, SYSCTL_CHILDREN(qoid), OID_AUTO,
	    "state", CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, q, 0,
	    kwq_sysctl_qstate, "A", "inactive, active, draining, drained");
	SYSCTL_ADD_PROC(&q->kwq_sysctl, SYSCTL_CHILDREN(qoid), OID_AUTO,
	    "reset", CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, q, 0,
	    kwq_sysctl_reset, "I", "write 1 to zero maxlat_ns and maxdepth on every CPU");
	cpuoid = SYSCTL_ADD_NODE(&q->kwq_sysctl, SYSCTL_CHILDREN(qoid), OID_AUTO,
	    "cpu", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "the queue's per-CPU lists");
	if (cpuoid == NULL)
		return (0);
	CPU_FOREACH(cpu) {
		kc = q->kwq_pcpu[cpu];
		snprintf(cname, sizeof(cname), "%d", cpu);
		coid = SYSCTL_ADD_NODE(&q->kwq_sysctl, SYSCTL_CHILDREN(cpuoid),
		    OID_AUTO, cname, CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
		    "per-CPU list");
		if (coid == NULL)
			continue;
		cchildren = SYSCTL_CHILDREN(coid);
#define	KC_U64(field, name, descr)					\
		SYSCTL_ADD_U64(&q->kwq_sysctl, cchildren, OID_AUTO, name,	\
		    CTLFLAG_RD, &kc->field, 0, descr)
		SYSCTL_ADD_UINT(&q->kwq_sysctl, cchildren, OID_AUTO, "depth",
		    CTLFLAG_RD, &kc->kc_depth, 0, "items queued now");
		SYSCTL_ADD_UINT(&q->kwq_sysctl, cchildren, OID_AUTO, "state",
		    CTLFLAG_RD, &kc->kc_state, 0,
		    "0 idle, 1 waking, 2 running, 3 parked");
		KC_U64(kc_items, "items", "elements handed to the handler, notifiers included");
		KC_U64(kc_signals, "signals", "notifiers consumed by the handler (a subset of items)");
		KC_U64(kc_passes, "passes", "handler invocations");
		KC_U64(kc_cycles, "cycles", "cpu_ticks spent in the handler");
		KC_U64(kc_rejected, "rejected",
		    "enqueues refused (ENOBUFS, or ENXIO after drain began)");
		KC_U64(kc_coalesced, "coalesced",
		    "kwq_notify() calls that found the notifier pending");
		KC_U64(kc_requeued, "requeued", "elements requeued by the handler, notifiers included");
		KC_U64(kc_maxdepth, "maxdepth", "high-water mark of depth at pass start");
		KC_U64(kc_maxlat_ns, "maxlat_ns", "largest oldest-element age seen at pass start");
		KC_U64(kc_overruns, "overruns", "passes that exceeded their budget by more than one quantum x weight");
		KC_U64(kc_overruns_switched, "overruns_switched", "of those, passes during which the worker left the CPU (preempted or blocked): the system's excess, not the handler's");
		KC_U64(kc_doorbells, "doorbells", "wakeups performed for this list");
		KC_U64(kc_doorbells_skipped, "doorbells_skipped", "lists placed while the worker's tick was armed: no wakeup now, the tick or tick_urgent_us serves them (KWQ.md S16)");
		KC_U64(kc_parks, "parks", "rounds skipped for a deficit <= 0");
		KC_U64(kc_debts, "debts", "doorbells sent to the ring because the queue went idle owing time");
		KC_U64(kc_glitches, "glitches", "passes whose CPU-time delta was negative or > 1 s (ticker fault; charged one quantum)");
		KC_U64(kc_switched, "switched", "passes during which the worker left the CPU (preempted or blocked); NET charges them wall time");
		KC_U64(kc_boosts, "boosts", "passes served from the new list");
		KC_U64(kc_grace, "grace", "doorbells sent to the ring by the grace rule");
		if (q->kwq_class == KWQ_BULK) {
			KC_U64(kc_steals_in, "steals_in", "batches this CPU's worker took from other CPUs' lists of the queue (SCHED.md S16)");
			KC_U64(kc_stolen_in, "stolen_in", "items in those batches");
			KC_U64(kc_steals_out, "steals_out", "batches other CPUs' workers took from this list");
			KC_U64(kc_stolen_out, "stolen_out", "items in those batches");
			KC_U64(kc_scattered, "scattered", "items kwq_scatter() put on this list");
			KC_U64(kc_cost_ns, "cost_ns", "place=2: an item's cost on this list, ns, averaged over passes (1/8 per pass): the hint's cost");
		}
#undef KC_U64
	}
	return (0);
}

struct kwq *
kwq_create(const char *name, enum kwq_class cls, uint32_t flags,
    const struct kwq_params *p, kwq_handler_t *fn, void *ctx)
{
	struct kwq *q, *o;
	struct kwq_cpu *kc;
	struct kwq_params np;
	int cpu, domain;

	if (p == NULL) {
		memset(&np, 0, sizeof(np));
		np.domain = -1;
		p = &np;
	}
	if (name == NULL || fn == NULL) {
		KASSERT(0, ("kwq_create: no name or handler"));
		return (NULL);
	}
	if (!kwq_name_valid(name)) {
		printf("kwq: refusing queue name \"%.*s\": 1..%d characters from "
		    "[A-Za-z0-9_-] only\n", KWQ_NAMELEN, name, KWQ_NAMELEN - 1);
		return (NULL);
	}
	if (cls == KWQ_BLOCKING) {
		printf("kwq: %s: BLOCKING class not implemented\n", name);
		return (NULL);
	}
	if (cls != KWQ_NET && cls != KWQ_BULK) {
		KASSERT(0, ("kwq_create: %s: bad class %d", name, cls));
		return (NULL);
	}
	if ((flags & ~KWQ_F_ALL) != 0 || (flags & KWQ_F_RESERVE) != 0 ||
	    ((flags & KWQ_F_STEALABLE) != 0 && cls != KWQ_BULK) ||
	    p->weight > 8 || p->nreserve != 0) {
		KASSERT(0, ("kwq_create: %s: bad flags %#x or params", name,
		    flags));
		return (NULL);
	}

	q = malloc_aligned(sizeof(*q), KWQ_LINE, M_KWQ, M_WAITOK | M_ZERO);
	strlcpy(q->kwq_name, name, sizeof(q->kwq_name));
	snprintf(q->kwq_lockname, sizeof(q->kwq_lockname), "kwq %s", name);
	q->kwq_class = cls;
	q->kwq_flags = flags;
	q->kwq_limit = p->limit != 0 ? p->limit : kwq_limit[cls];
	if (q->kwq_limit > INT_MAX)	/* a batch's n must fit an int */
		q->kwq_limit = INT_MAX;
	q->kwq_weight = p->weight != 0 ? p->weight : 1;
	q->kwq_fn = fn;
	q->kwq_ctx = ctx;
#ifdef VIMAGE
	if (flags & KWQ_F_VNET)
		q->kwq_vnet = curvnet;
#endif
	q->kwq_pcpu = malloc(sizeof(*q->kwq_pcpu) * (mp_maxid + 1), M_KWQ,
	    M_WAITOK | M_ZERO);
	CPU_FOREACH(cpu) {
		domain = p->domain >= 0 ? p->domain : pcpu_find(cpu)->pc_domain;
		kc = malloc_domainset_aligned(sizeof(*kc), KWQ_LINE, M_KWQ,
		    DOMAINSET_PREF(domain), M_WAITOK | M_ZERO);
		mtx_init(&kc->kc_mtx, q->kwq_lockname, NULL,
		    (flags & KWQ_F_SPIN) ? MTX_SPIN : MTX_DEF);
		STAILQ_INIT(&kc->kc_list);
		ks_queue_init(kc);
		if (flags & KWQ_F_SPIN)
			kc->kc_flags |= KWQ_KC_SPIN;
		kc->kc_q = q;
		kc->kc_cpu = cpu;
		q->kwq_pcpu[cpu] = kc;
	}

	sx_xlock(&kwq_sx);
	LIST_FOREACH(o, &kwq_all, kwq_all) {
		if (o->kwq_class == cls && strcmp(o->kwq_name, name) == 0)
			break;
	}
	if (o != NULL) {
		sx_xunlock(&kwq_sx);
		printf("kwq: %s: name already registered in class %s\n", name,
		    kwq_class_names[cls]);
		q->kwq_drained = 1;
		kwq_destroy(q);
		return (NULL);
	}
	LIST_INSERT_HEAD(&kwq_all, q, kwq_all);
	sx_xunlock(&kwq_sx);
	q->kwq_steal_slot = -1;
	if (flags & KWQ_F_STEALABLE) {
		rm_wlock(&kwq_steal_rm);
		LIST_INSERT_HEAD(&kwq_steal_list, q, kwq_steal);
		(void)kwq_shard_slot_add(q);	/* a slot for P8's donor hints */
		q->kwq_stealing = true;
		atomic_add_int(&kwq_nstealable, 1);
		rm_wunlock(&kwq_steal_rm);
	}

	q->kwq_state = KWQ_ST_INACTIVE;
	if (kwq_sysctl_register(q) != 0) {
		q->kwq_drained = 1;	/* destroy unlinks and frees it */
		kwq_destroy(q);
		return (NULL);
	}
	SDT_PROBE1(kwq, , , create, q);
	if ((flags & KWQ_F_INACTIVE) == 0)
		kwq_activate(q);
	return (q);
}

void
kwq_activate(struct kwq *q)
{
	struct kwq_cpu *kc;
	int cpu;

	KASSERT(!q->kwq_active && !q->kwq_drained,
	    ("kwq_activate: %s active or drained", q->kwq_name));
	/* One store under every list lock: no producer can be half-way. */
	CPU_FOREACH(cpu) {
		kc = q->kwq_pcpu[cpu];
		kwq_kc_lock(kc);
		kwq_kc_unlock(kc);
	}
	atomic_store_rel_int(&q->kwq_active, 1);
	q->kwq_state = KWQ_ST_ACTIVE;
	SDT_PROBE1(kwq, , , activate, q);
}

/*
 * Deactivate, then let each CPU's worker run (or discard) what is left,
 * then wait for every pass to finish.  Producers check kwq_active under
 * the list lock, so one lock/unlock per CPU after the store is a barrier
 * against producers that had passed the check.
 */
/* Take q off the steal list, or just fence; write side of kwq_steal_rm. */
static void
kwq_steal_unlist(struct kwq *q)
{
	rm_wlock(&kwq_steal_rm);
	if (q->kwq_stealing) {
		LIST_REMOVE(q, kwq_steal);
		kwq_shard_slot_del(q);
		q->kwq_stealing = false;
		atomic_subtract_int(&kwq_nstealable, 1);
	}
	rm_wunlock(&kwq_steal_rm);
}

void
kwq_drain(struct kwq *q)
{
	struct kwq_worker *kw __diagused;
	struct kwq_cpu *kc;
	int cpu;

	KASSERT(THREAD_CAN_SLEEP(), ("kwq_drain: %s: non-sleepable context",
	    q->kwq_name));
	kw = kwq_workers[q->kwq_class][curcpu];
	KASSERT(kw == NULL || kw->kw_td != curthread,
	    ("kwq_drain: %s: called from a kwq handler", q->kwq_name));

	atomic_store_rel_int(&q->kwq_active, 0);
	q->kwq_state = KWQ_ST_DRAINING;
	SDT_PROBE1(kwq, , , drain__start, q);
	CPU_FOREACH(cpu) {
		kc = q->kwq_pcpu[cpu];
		kwq_kc_lock(kc);
		kwq_kc_unlock(kc);
	}
	/*
	 * BULK (../SCHED.md S16): stop the steals of this queue and wait out
	 * any steal or scatter that passed its activity check before the
	 * store above, so no batch is between two CPUs' lists while the
	 * sweep below looks at them one by one.
	 */
	if (q->kwq_class == KWQ_BULK)
		kwq_steal_unlist(q);
	CPU_FOREACH(cpu) {
		kc = q->kwq_pcpu[cpu];
		kwq_kc_lock(kc);
		for (;;) {
			if (kc->kc_state == KWQ_CPU_IDLE) {
				if (STAILQ_EMPTY(&kc->kc_list))
					break;
				kc->kc_state = KWQ_CPU_WAKING;
				kwq_doorbell(kc, KWQ_RING_SIGNAL);	/* a drain waits for no regime */
			}
			kwq_kc_wait(kc, "kwqdrn");
		}
		KASSERT(kc->kc_nnotify == 0,
		    ("kwq_drain: %s: %u notifiers pending on cpu %d after the drain:"
		    " a handler walked without KWQ_ITEM_NEXT()", q->kwq_name,
		    kc->kc_nnotify, cpu));
		kwq_kc_unlock(kc);
	}
	q->kwq_drained = 1;
	q->kwq_state = KWQ_ST_DRAINED;
	SDT_PROBE1(kwq, , , drain__end, q);
}

void
kwq_destroy(struct kwq *q)
{
	struct kwq_cpu *kc;
	int cpu;

	KASSERT(q->kwq_drained, ("kwq_destroy: %s not drained", q->kwq_name));
	KASSERT(THREAD_CAN_SLEEP(), ("kwq_destroy: non-sleepable context"));

	SDT_PROBE1(kwq, , , destroy, q);
	if (q->kwq_stealing)		/* a create that failed late */
		kwq_steal_unlist(q);
	if (q->kwq_sysctl.tqh_first != NULL || q->kwq_sysctl.tqh_last != NULL)
		sysctl_ctx_free(&q->kwq_sysctl);
	sx_xlock(&kwq_sx);
	if (q->kwq_all.le_prev != NULL)
		LIST_REMOVE(q, kwq_all);
	sx_xunlock(&kwq_sx);
	CPU_FOREACH(cpu) {
		kc = q->kwq_pcpu[cpu];
		if (kc == NULL)
			continue;
		KASSERT(kc->kc_state == KWQ_CPU_IDLE &&
		    STAILQ_EMPTY(&kc->kc_list) && kc->kc_nnotify == 0,
		    ("kwq_destroy: %s cpu%d not idle", q->kwq_name, cpu));
		mtx_destroy(&kc->kc_mtx);
		free(kc, M_KWQ);
	}
	free(q->kwq_pcpu, M_KWQ);
	free(q, M_KWQ);
}

/*
 * Module.
 */
static int
kwq_modevent(module_t mod __unused, int type, void *data __unused)
{
	int error;

	switch (type) {
	case MOD_LOAD:
		kwq_priority_check();
		kwq_cpu_remap_init();
		kwq_class_oids[KWQ_NET] = SYSCTL_STATIC_CHILDREN(_kern_kwq_net);
		kwq_class_oids[KWQ_BULK] =
		    SYSCTL_STATIC_CHILDREN(_kern_kwq_bulk);
		kwq_class_oids[KWQ_BLOCKING] =
		    SYSCTL_STATIC_CHILDREN(_kern_kwq_blocking);
		kwq_cpu_oids[KWQ_NET] = SYSCTL_STATIC_CHILDREN(_kern_kwq_net_cpu);
		kwq_cpu_oids[KWQ_BULK] = SYSCTL_STATIC_CHILDREN(_kern_kwq_bulk_cpu);
		kwq_cpu_oids[KWQ_BLOCKING] =
		    SYSCTL_STATIC_CHILDREN(_kern_kwq_blocking_cpu);
		kwq_queue_oids[KWQ_NET] = SYSCTL_STATIC_CHILDREN(_kern_kwq_net_queue);
		kwq_queue_oids[KWQ_BULK] = SYSCTL_STATIC_CHILDREN(_kern_kwq_bulk_queue);
		kwq_queue_oids[KWQ_BLOCKING] =
		    SYSCTL_STATIC_CHILDREN(_kern_kwq_blocking_queue);
		kwq_knobs_register();
		kwq_topo_init();
		kwq_shard_init();
		kwq_gossip_init();
		error = kwq_workers_start(kwq_cpu_oids);
		if (error != 0) {
			kwq_gossip_fini();
			kwq_shard_fini();
			kwq_topo_fini();
			sysctl_ctx_free(&kwq_knob_sysctl);
		}
		return (error);
	case MOD_UNLOAD:
		sx_xlock(&kwq_sx);
		if (!LIST_EMPTY(&kwq_all)) {
			sx_xunlock(&kwq_sx);
			return (EBUSY);
		}
		sx_xunlock(&kwq_sx);
		kwq_workers_stop();
		kwq_gossip_fini();
		kwq_shard_fini();
		kwq_topo_fini();
		sysctl_ctx_free(&kwq_knob_sysctl);
		return (0);
	case MOD_QUIESCE:
		sx_slock(&kwq_sx);
		error = LIST_EMPTY(&kwq_all) ? 0 : EBUSY;
		sx_sunlock(&kwq_sx);
		return (error);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t kwq_mod = {
	"kwq",
	kwq_modevent,
	NULL
};

DECLARE_MODULE(kwq, kwq_mod, SI_SUB_TASKQ, SI_ORDER_ANY);
MODULE_VERSION(kwq, 1);
