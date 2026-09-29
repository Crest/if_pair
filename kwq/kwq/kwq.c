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
#include <sys/proc.h>
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
 * sysctl tree (../KWQ.md S10.7): kern.kwq.{version,ncpu,nqueues};
 * kern.kwq.<class>.{knobs,priority,limit}; kern.kwq.<class>.cpu.<N>.* per
 * worker; kern.kwq.<class>.queue.<name>.* and .cpu.<N>.* per queue.  User
 * chosen names live only under "queue", one level of their own.
 */
SYSCTL_NODE(_kern, OID_AUTO, kwq, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "kernel work queues");
SYSCTL_NODE(_kern_kwq, OID_AUTO, net, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "NET class: packet-latency work at PI_NET");
SYSCTL_NODE(_kern_kwq, OID_AUTO, bulk, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "BULK class: CPU-heavy non-sleeping work at PI_SOFT");

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

static u_int kwq_limit[KWQ_NCLASS] = { 4096, 4096, 4096 };
SYSCTL_UINT(_kern_kwq_net, OID_AUTO, limit, CTLFLAG_RWTUN, &kwq_limit[KWQ_NET],
    0, "default per-CPU item limit for queues created with limit = 0");
SYSCTL_UINT(_kern_kwq_bulk, OID_AUTO, limit, CTLFLAG_RWTUN,
    &kwq_limit[KWQ_BULK], 0,
    "default per-CPU item limit for queues created with limit = 0");
SYSCTL_UINT(_kern_kwq_blocking, OID_AUTO, limit, CTLFLAG_RWTUN,
    &kwq_limit[KWQ_BLOCKING], 0,
    "default per-CPU item limit for queues created with limit = 0");

static int kwq_priority[KWQ_NCLASS] = { PI_NET, PI_SOFT, PUSER };
SYSCTL_INT(_kern_kwq_net, OID_AUTO, priority, CTLFLAG_RD,
    &kwq_priority[KWQ_NET], 0, "scheduler priority of the class's workers");
SYSCTL_INT(_kern_kwq_bulk, OID_AUTO, priority, CTLFLAG_RD,
    &kwq_priority[KWQ_BULK], 0, "scheduler priority of the class's workers");
SYSCTL_INT(_kern_kwq_blocking, OID_AUTO, priority, CTLFLAG_RD,
    &kwq_priority[KWQ_BLOCKING], 0,
    "scheduler priority of the class's workers");

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
static u_int kwq_quantum_us[KWQ_NCLASS] = { 200, 1000, 5000 };
static u_int kwq_cap_pct[KWQ_NCLASS] = { 100, 100, 100 };
static u_int kwq_cap_sleep_us[KWQ_NCLASS] = { 100, 100, 100 };
static u_int kwq_cap_window_us[KWQ_NCLASS] = { 10000, 10000, 10000 };
static u_int kwq_penalty_rounds[KWQ_NCLASS] = { 32, 32, 32 };
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
	k->grace_rounds = 1;
	k->cap_pct = kwq_cap_pct[cls];
	k->cap_window_ns = (uint64_t)kwq_cap_window_us[cls] * 1000;
	k->cap_sleep_ns = (uint64_t)kwq_cap_sleep_us[cls] * 1000;
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

static struct kwq_knob kwq_knob_desc[KWQ_NCLASS][6];

static void
kwq_knobs_register(void)
{
	static const struct { const char *name, *descr; u_int lo, hi; } d[6] = {
		{ "quantum_us", "CPU time a queue of weight 1 may consume per round", 10, 1000000 },
		{ "cap_pct", "CPU-share cap in percent of a window; 100 or more = off", 1, 1000 },
		{ "cap_sleep_us", "sleep when over the cap", 1, 1000000 },
		{ "cap_window_us", "busy-fraction window for the cap", 100, 10000000 },
		{ "penalty_rounds", "debt clamp in quanta: rounds an overrunning queue is parked at most", 1, 1024 },
		{ "budget_check_every", "kwq_budget_left() reads the clock at least every this many calls; 1 = every call", 1, 64 },
	};
	u_int *vars[6];
	int cls, i;

	sysctl_ctx_init(&kwq_knob_sysctl);
	for (cls = 0; cls < KWQ_NCLASS; cls++) {
		vars[0] = &kwq_quantum_us[cls]; vars[1] = &kwq_cap_pct[cls];
		vars[2] = &kwq_cap_sleep_us[cls]; vars[3] = &kwq_cap_window_us[cls];
		vars[4] = &kwq_penalty_rounds[cls]; vars[5] = &kwq_budget_check_every[cls];
		for (i = 0; i < 6; i++) {
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
 */
int
kwq_cpu_for_hash(uint32_t hash)
{
	int cpu;

	cpu = hash % (mp_maxid + 1);
	if (__predict_false(CPU_ABSENT(cpu)))
		cpu = curcpu;
	return (cpu);
}

/*
 * KWQ_CPU_ANY: least loaded CPU in the caller's memory domain, ties to the
 * caller's CPU.  P0 uses the queue's own depth as the load measure and the
 * NUMA domain as the neighbourhood; the cache-domain refinement is P6.
 */
#define	KWQ_PICK_PROBES	4	/* other CPUs' shared lines read per KWQ_CPU_ANY pick */

static int
kwq_pick_any(struct kwq *q)
{
	struct kwq_cpu *kc;
	int cpu, best, domain, probes;
	u_int depth, bestdepth;

	best = curcpu;
	bestdepth = q->kwq_pcpu[best]->kc_depth;
	if (bestdepth == 0)
		return (best);
	/*
	 * Bounded probe (KWQ.md S17): the next KWQ_PICK_PROBES CPUs of the
	 * caller's domain, each a read of another CPU's shared line, never
	 * the whole domain (128 lines on a07).  P6 refines the neighbourhood.
	 */
	domain = pcpu_find(best)->pc_domain;
	probes = 0;
	for (cpu = best + 1; cpu != best && probes < KWQ_PICK_PROBES; cpu++) {
		if (cpu > (int)mp_maxid)
			cpu = -1;	/* the ++ wraps to 0 */
		else if (!CPU_ABSENT(cpu) &&
		    pcpu_find(cpu)->pc_domain == domain) {
			probes++;
			kc = q->kwq_pcpu[cpu];
			depth = kc->kc_depth;	/* unlocked read: a hint */
			if (depth < bestdepth) {
				best = cpu;
				bestdepth = depth;
				if (depth == 0)
					break;
			}
		}
	}
	return (best);
}

static inline int
kwq_target_cpu(struct kwq *q, int cpu)
{
	if (cpu == KWQ_CPU_ANY) {
		if (q->kwq_class != KWQ_BULK)
			return (-1);
		return (kwq_pick_any(q));
	}
	if (__predict_false(cpu < 0 || cpu > (int)mp_maxid || CPU_ABSENT(cpu)))
		return (curcpu);
	return (cpu);
}

/*
 * Called with kc locked after something was added: stamp the start of a
 * burst and ring the doorbell if the worker is not already scheduled for
 * this (queue, CPU).  One lock hold covers publication and wakeup, so a
 * wakeup can never be lost (../NOTES.md, the if_pair invariant).
 */
static inline bool
kwq_kc_added(struct kwq_cpu *kc, u_int added)
{
	if (kc->kc_depth + kc->kc_nnotify == added)
		kc->kc_empty_since = sbinuptime();
	if (kc->kc_state == KWQ_CPU_IDLE) {
		kc->kc_state = KWQ_CPU_WAKING;
		kwq_doorbell(kc);
		return (true);
	}
	return (false);
}

/*
 * Data path.
 */
int
kwq_enqueue(struct kwq *q, int cpu, struct kwq_item *it)
{
	struct kwq_cpu *kc;
	u_int depth __unused;	/* SDT arguments */
	bool woke __unused;

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
	STAILQ_INSERT_TAIL(&kc->kc_list, it, kwi_link);
	depth = ++kc->kc_depth;
	woke = kwq_kc_added(kc, 1);
	kwq_kc_unlock(kc);
	SDT_PROBE4(kwq, , , enqueue, q, cpu, depth, woke);
	return (0);
}

int
kwq_enqueue_list(struct kwq *q, int cpu, struct kwq_item *head,
    struct kwq_item *tail, int n)
{
	STAILQ_HEAD(, kwq_item) tmp;
	struct kwq_cpu *kc;
	u_int depth __unused;	/* SDT arguments */
	bool woke __unused;

	KASSERT(n > 0 && head != NULL && tail != NULL,
	    ("kwq_enqueue_list: bad list %p %p %d", head, tail, n));
	KASSERT(tail->kwi_link.stqe_next == NULL,
	    ("kwq_enqueue_list: tail %p not terminated", tail));
	cpu = kwq_target_cpu(q, cpu);
	if (__predict_false(cpu < 0))
		return (EINVAL);
	kc = q->kwq_pcpu[cpu];

	tmp.stqh_first = head;
	tmp.stqh_last = &tail->kwi_link.stqe_next;

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
	STAILQ_CONCAT(&kc->kc_list, &tmp);
	kc->kc_depth += n;
	depth = kc->kc_depth;
	woke = kwq_kc_added(kc, n);
	kwq_kc_unlock(kc);
	SDT_PROBE4(kwq, , , enqueue, q, cpu, depth, woke);	/* once per call */
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
	tail->kwi_link.stqe_next = kc->kc_list.stqh_first;
	if (kc->kc_list.stqh_first == NULL)
		kc->kc_list.stqh_last = &tail->kwi_link.stqe_next;
	kc->kc_list.stqh_first = head;
	kc->kc_depth += n;
	kc->kc_requeued += n;
	kwq_kc_unlock(kc);
}

/*
 * Handler only: nanoseconds of this pass's budget still unspent, 0 when
 * the quantum is exhausted or a higher class waits for the CPU (SCHED.md
 * S4.4, S6.4), telling the handler to requeue its remainder.
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
	if (!ks_budget_need_clock(kw, &left))	/* S4.4: the estimate */
		return (left);
	left = ks_budget_left(kw, kwq_cputime_ns(kw));
	if (left == 0)
		SDT_PROBE2(kwq, , , budget__hit, q, curcpu);
	return (left);
}

int
kwq_scatter(struct kwq *q __unused, struct kwq_item *list __unused,
    int n __unused, void (*done)(void *) __unused, void *arg __unused)
{
	return (EOPNOTSUPP);	/* P6 */
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
	if (nf->kn_state != KWQ_NF_IDLE) {
		if (nf->kn_state == KWQ_NF_PENDING)
			kc->kc_coalesced++;
		kwq_kc_unlock(kc);
		return (false);
	}
	nf->kn_state = KWQ_NF_PENDING;
	STAILQ_INSERT_TAIL(&kc->kc_notify, &nf->kn_item, kwi_link);
	kc->kc_nnotify++;
	kwq_kc_added(kc, 1);
	kwq_kc_unlock(kc);
	return (true);
}

/*
 * Detach-time: after return the notifier is neither queued nor about to
 * run, and stays inert until kwq_notifier_init() again.  A pass that
 * already holds it in its batch skips it (kwq_worker.c) and we wait for
 * that pass to end.
 */
void
kwq_notify_cancel(struct kwq *q, struct kwq_notifier *nf)
{
	struct kwq_cpu *kc;
	struct kwq_item *it;

	KASSERT(THREAD_CAN_SLEEP(), ("kwq_notify_cancel: non-sleepable"));
	kc = q->kwq_pcpu[nf->kn_cpu];

	kwq_kc_lock(kc);
	if (nf->kn_state == KWQ_NF_PENDING) {
		STAILQ_FOREACH(it, &kc->kc_notify, kwi_link) {
			if (it == &nf->kn_item) {
				STAILQ_REMOVE(&kc->kc_notify, it, kwq_item,
				    kwi_link);
				kc->kc_nnotify--;
				KWQ_ITEM_INIT(it);
				break;
			}
		}
		/* Not on the live list: in a batch being handled. */
	}
	nf->kn_state = KWQ_NF_CANCELLED;
	while (kc->kc_state == KWQ_CPU_RUNNING)
		kwq_kc_wait(kc, "kwqncl");
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
	SYSCTL_ADD_UINT(&q->kwq_sysctl, SYSCTL_CHILDREN(qoid), OID_AUTO,
	    "limit", CTLFLAG_RD, &q->kwq_limit, 0,
	    "per-CPU item limit in effect (INT_MAX for KWQ_LIMIT_NONE)");
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
		KC_U64(kc_items, "items", "items and notifiers handed to the handler");
		KC_U64(kc_passes, "passes", "handler invocations");
		KC_U64(kc_cycles, "cycles", "cpu_ticks spent in the handler");
		KC_U64(kc_rejected, "rejected",
		    "enqueues refused (ENOBUFS, or ENXIO after drain began)");
		KC_U64(kc_coalesced, "coalesced",
		    "kwq_notify() calls that found the notifier pending");
		KC_U64(kc_requeued, "requeued", "items requeued by the handler");
		KC_U64(kc_maxdepth, "maxdepth", "high-water mark of depth at pass start");
		KC_U64(kc_maxlat_ns, "maxlat_ns", "largest oldest-item age seen at pass start");
		KC_U64(kc_overruns, "overruns", "passes that exceeded their budget by more than one quantum x weight");
		KC_U64(kc_parks, "parks", "rounds skipped for a deficit <= 0");
		KC_U64(kc_debts, "debts", "doorbells sent to the ring because the queue went idle owing time");
		KC_U64(kc_glitches, "glitches", "passes whose CPU-time delta was negative or > 1 s (ticker fault; charged one quantum)");
		KC_U64(kc_boosts, "boosts", "passes served from the new list");
		KC_U64(kc_grace, "grace", "doorbells sent to the ring by the grace rule");
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
		STAILQ_INIT(&kc->kc_notify);
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
	CPU_FOREACH(cpu) {
		kc = q->kwq_pcpu[cpu];
		kwq_kc_lock(kc);
		for (;;) {
			if (kc->kc_state == KWQ_CPU_IDLE) {
				if (STAILQ_EMPTY(&kc->kc_list) &&
				    STAILQ_EMPTY(&kc->kc_notify))
					break;
				kc->kc_state = KWQ_CPU_WAKING;
				kwq_doorbell(kc);
			}
			kwq_kc_wait(kc, "kwqdrn");
		}
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
		    STAILQ_EMPTY(&kc->kc_list) && STAILQ_EMPTY(&kc->kc_notify),
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
		error = kwq_workers_start(kwq_cpu_oids);
		if (error != 0)
			sysctl_ctx_free(&kwq_knob_sysctl);
		return (error);
	case MOD_UNLOAD:
		sx_xlock(&kwq_sx);
		if (!LIST_EMPTY(&kwq_all)) {
			sx_xunlock(&kwq_sx);
			return (EBUSY);
		}
		sx_xunlock(&kwq_sx);
		kwq_workers_stop();
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
