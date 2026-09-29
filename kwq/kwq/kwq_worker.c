/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq(9) - workers: the kernel glue around the scheduler core
 * (kwq_sched.c, ../SCHED.md).
 *
 * One worker thread per (class, CPU), created at module load, bound to
 * its CPU by itself once smp_started (the GELI pattern, g_eli.c), at the
 * class's fixed priority.  The worker asks the core what to do next
 * (ks_next), runs passes (swap the lists out under the list lock, run the
 * handler with no service lock held, charge the pass in the worker's own
 * CPU time), and acts on the core's yield decisions: kern_yield() at the
 * class's fixed yield priority, or pause_sbt() for the CPU-share cap.
 * Between passes the tick guard yields once a hardclock tick has passed
 * since the worker last gave the CPU up voluntarily.
 *
 * Hand-back between classes on one CPU (SCHED.md S6.4): a class sets its
 * bit in the per-CPU kwq_waiting word around a yield; lower classes on
 * that CPU see it in kwq_higher_waiting() (ks_next ends their round after
 * the current pass) and in kwq_budget_left() (returns 0).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/unistd.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/domainset.h>
#include <sys/epoch.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <net/vnet.h>

#include "kwq_sched.h"

struct kwq_worker **kwq_workers[KWQ_NCLASS];
static struct sysctl_ctx_list kwq_worker_sysctl_ctx;

/*
 * Wait channels: one 8-byte slot per (class, CPU) in a 256-byte-aligned
 * array, so consecutive workers hash to distinct sleepqueue chains
 * (subr_sleepqueue.c SC_HASH uses the low two address bytes; with an
 * 8-byte stride from a 256-byte-aligned base the low byte contributes
 * bits 3-7 and the second byte bits 0-2, disjoint for 256 slots).  The
 * channel memory itself is never touched, only hashed.
 */
static uint64_t *kwq_chans[KWQ_NCLASS];

/* Per-CPU hand-back flags: bit c set while class c's worker is yielding. */
DPCPU_DEFINE_STATIC(volatile u_int, kwq_waiting);

static void kwq_worker_main(void *arg);

bool
kwq_higher_waiting(enum kwq_class cls)
{
	u_int w = DPCPU_GET(kwq_waiting);

	return ((w & ((1u << cls) - 1)) != 0);
}

/*
 * The worker's own CPU time in ns (SCHED.md S1a.4): what mi_switch() has
 * accounted plus the time since the last switch-in, read without a
 * context switch in between.
 */
uint64_t
kwq_cputime_ns(const struct kwq_worker *kw)
{
	uint64_t t;

	return (kwq_cputime_ns_tick(kw, &t));
}

/*
 * Same, and also hand back the raw cpu_ticks() reading so a caller that
 * wants both (the pass accounting) reads the clock once: on arm64 a
 * generic-timer read costs an isb() and showed at 16 % of the worker's
 * cycles on a07 with five reads per pass (SCHED.md S15.7).
 */
uint64_t
kwq_cputime_ns_tick(const struct kwq_worker *kw, uint64_t *tick)
{
	uint64_t t;

	critical_enter();
	*tick = cpu_ticks();
	t = curthread->td_runtime + (*tick - PCPU_GET(switchtime));
	critical_exit();
	return (ks_ticks2ns(t, kw->kw_ns_scale));
}

/*
 * Ring the doorbell for kc: place it with the core (new list or, by the
 * grace rule, the ring) and wake the worker if it sleeps.  Called with kc
 * locked; kc_mtx (sleep or spin) -> kw_mtx (spin) is the lock order.
 */
void
kwq_doorbell(struct kwq_cpu *kc)
{
	struct kwq_worker *kw;

	kw = kwq_workers[kc->kc_q->kwq_class][kc->kc_cpu];
	mtx_lock_spin(&kw->kw_mtx);
	KASSERT(kc->kc_onlist == KWQ_ON_NONE, ("kwq_doorbell: %s cpu%d on list %u",
	    kc->kc_q->kwq_name, kc->kc_cpu, kc->kc_onlist));
	ks_doorbell(kw, kc);
	if (kw->kw_sleeping) {
		kw->kw_sleeping = false;
		wakeup_one(kw->kw_chan);
	}
	mtx_unlock_spin(&kw->kw_mtx);
}

/*
 * Give the CPU away for one scheduling decision at the class's fixed
 * yield priority, then re-assert the class priority (kern_yield() rewrote
 * td_base_pri, and a kthread has no userret to restore it).  The hand-back
 * bit is set for the duration.
 */
static void
kwq_yield(struct kwq_worker *kw)
{
	u_int bit = 1u << kw->kw_class;

	DPCPU_SET(kwq_waiting, DPCPU_GET(kwq_waiting) | bit);
	kern_yield(kwq_yield_prio[kw->kw_class]);
	thread_lock(curthread);
	sched_prio(curthread, kw->kw_pri);
	thread_unlock(curthread);
	DPCPU_SET(kwq_waiting, DPCPU_GET(kwq_waiting) & ~bit);
}

/*
 * One pass over one (queue, CPU): swap the lists out, run the handler with
 * no service lock held, charge the pass, re-check emptiness.
 *
 * Notifiers are released one at a time: under the lock, read the next
 * pointer, drop the pending state, unlock, call.  From that moment a
 * re-notify may legally relink the notifier into the live list, which is
 * why its link must not be part of a list the handler is walking.
 * Cancelled notifiers are unlinked and skipped.  Items travel as one list.
 */
static void
kwq_pass(struct kwq_worker *kw, struct kwq_cpu *kc)
{
	struct kwq *q;
	struct kwq_item *items, *nfs, *it, *next;
	struct kwq_notifier *nf;
	struct epoch_tracker et;
	sbintime_t now;
	uint64_t t0, age;
	u_int n, nnf;
	int sign;
	bool net, has_work;
	uint64_t t1, cpu_now;

	q = kc->kc_q;
	net = (q->kwq_class == KWQ_NET);

	kwq_kc_lock(kc);
	items = STAILQ_FIRST(&kc->kc_list);
	n = kc->kc_depth;
	STAILQ_INIT(&kc->kc_list);
	kc->kc_depth = 0;
	nfs = STAILQ_FIRST(&kc->kc_notify);
	nnf = kc->kc_nnotify;
	STAILQ_INIT(&kc->kc_notify);
	kc->kc_nnotify = 0;
	/* Discard only ever happens on a drained queue. */
	sign = (!q->kwq_active && (q->kwq_flags & KWQ_F_DISCARD)) ? -1 : 1;
	if (n + nnf > 0) {
		now = sbinuptime();
		if (now > kc->kc_empty_since) {
			age = kwq_sbt2ns(now - kc->kc_empty_since);
			if (age > kc->kc_maxlat_ns)
				kc->kc_maxlat_ns = age;
		}
	}
	ks_pass_begin(kw, kc, kwq_cputime_ns_tick(kw, &t0));
	kwq_kc_unlock(kc);

	if (n > kc->kc_maxdepth)
		kc->kc_maxdepth = n;
	THREAD_NO_SLEEPING();
	if (net)
		NET_EPOCH_ENTER(et);
#ifdef VIMAGE
	if (q->kwq_flags & KWQ_F_VNET)
		CURVNET_SET_QUIET(q->kwq_vnet);
#endif

	for (it = nfs; it != NULL; it = next) {
		nf = __containerof(it, struct kwq_notifier, kn_item);
		kwq_kc_lock(kc);
		next = STAILQ_NEXT(it, kwi_link);
		KWQ_ITEM_INIT(it);
		if (nf->kn_state == KWQ_NF_PENDING)
			nf->kn_state = KWQ_NF_IDLE;
		else
			it = NULL;	/* cancelled: skip */
		kwq_kc_unlock(kc);
		if (it != NULL)
			q->kwq_fn(q, it, sign, q->kwq_ctx);
	}
	if (items != NULL)
		q->kwq_fn(q, items, sign * (int)n, q->kwq_ctx);

#ifdef VIMAGE
	if (q->kwq_flags & KWQ_F_VNET)
		CURVNET_RESTORE();
#endif
	if (net)
		NET_EPOCH_EXIT(et);
	THREAD_SLEEPING_OK();
	kc->kc_items += n + nnf;

	/* Emptiness re-check under the lock; the core re-places the queue. */
	kwq_kc_lock(kc);
	has_work = !STAILQ_EMPTY(&kc->kc_list) || !STAILQ_EMPTY(&kc->kc_notify);
	mtx_lock_spin(&kw->kw_mtx);
	cpu_now = kwq_cputime_ns_tick(kw, &t1);
	kc->kc_cycles += t1 - t0;
	ks_pass_end(kw, kc, cpu_now, has_work);
	mtx_unlock_spin(&kw->kw_mtx);
	if (kc->kc_waiters != 0)
		wakeup(kc);
	kwq_kc_unlock(kc);

	/*
	 * Tick guard (SCHED.md S6.1): unconditional once a tick has passed
	 * since the last voluntary switch.  It costs at most hz switches per
	 * second and does not depend on what sched_runnable() can see, which
	 * is what makes the callout-lateness bound (B7) hold by construction,
	 * as if_pair's pair_ticked() did.
	 */
	if (ks_ticked((u_int)ticks, (u_int)curthread->td_swvoltick)) {
		kw->kw_tick_yields++;
		kwq_yield(kw);
	}
}

static void
kwq_worker_main(void *arg)
{
	struct kwq_worker *kw = arg;
	struct kwq_cpu *kc;
	enum ks_action a;
	enum ks_yield y;
	sbintime_t t0;
	uint64_t rate;

#ifdef SMP
	/* Before sched_bind() to a CPU, wait for all CPUs to go on-line. */
	while (!smp_started)
		tsleep(kw, 0, "kwqsmp", hz / 4);
#endif
	thread_lock(curthread);
	sched_prio(curthread, kw->kw_pri);
	sched_bind(curthread, kw->kw_cpu);
	thread_unlock(curthread);
	kw->kw_rate = cpu_tickrate();
	kw->kw_ns_scale = ks_ns_scale(kw->kw_rate);
	kw->kw_win_start = kwq_sbt2ns(sbinuptime());

	for (;;) {
		mtx_lock_spin(&kw->kw_mtx);
		a = ks_next(kw, kwq_higher_waiting(kw->kw_class), &kc);
		if (a == KS_IDLE) {
			if (kw->kw_exit)
				goto out;
			kw->kw_sleeping = true;
			t0 = sbinuptime();
			msleep_spin(kw->kw_chan, &kw->kw_mtx, "kwqidle", 0);
			kw->kw_idle_ns += kwq_sbt2ns(sbinuptime() - t0);
			kw->kw_wakeups++;
			mtx_unlock_spin(&kw->kw_mtx);
			continue;
		}
		mtx_unlock_spin(&kw->kw_mtx);

		switch (a) {
		case KS_SERVE:
			kwq_pass(kw, kc);
			break;
		case KS_ROUND_END:
		case KS_HANDBACK:
			/* Re-derive the tick scale if the ticker was recalibrated. */
			rate = cpu_tickrate();
			if (rate != kw->kw_rate) {
				kw->kw_rate = rate;
				kw->kw_ns_scale = ks_ns_scale(rate);
			}
			y = ks_round_end(kw, kwq_sbt2ns(sbinuptime()),
			    sched_runnable());
			if (y == KS_Y_YIELD) {
				kwq_yield(kw);
			} else if (y == KS_Y_PAUSE) {
				pause_sbt("kwqcap",
				    (sbintime_t)kw->kw_knobs->cap_sleep_ns * SBT_1NS,
				    0, C_PREL(1));
			}
			break;
		default:
			break;
		}
	}
out:
	/*
	 * Announce the exit under the lock, then leave through kernel text.
	 * The instructions between the unlock and kthread_exit() are the
	 * same window every module with its own threads has (g_eli.c's
	 * workers, taskqueue_terminate()); the unloader also has to get
	 * through kldunload(2), so the window has never been observed.
	 */
	kw->kw_td = NULL;
	wakeup(&kw->kw_td);
	mtx_unlock_spin(&kw->kw_mtx);
	kthread_exit();
}

#ifdef INVARIANTS
/* Report sleepqueue chain collisions between the wait channels of a class. */
static u_int
kwq_sc_hash(const void *wc)
{
	return ((((uintptr_t)wc >> 8) ^ (uintptr_t)wc) & 255);
}

static void
kwq_workers_check_chains(void)
{
	int cls, i, j, coll;

	coll = 0;
	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		CPU_FOREACH(i) {
			CPU_FOREACH(j) {
				if (j <= i)
					continue;
				if (kwq_sc_hash(&kwq_chans[cls][i]) ==
				    kwq_sc_hash(&kwq_chans[cls][j]))
					coll++;
			}
		}
	}
	if (coll != 0)
		printf("kwq: %d sleepqueue chain collisions between workers\n",
		    coll);
}
#endif

/* kern.kwq.<class>.cpu<N>.* worker statistics (../KWQ.md S10.7). */
static void
kwq_worker_sysctl(struct kwq_worker *kw, struct sysctl_oid_list *parent)
{
	struct sysctl_oid *oid;
	struct sysctl_oid_list *ch;
	char name[16];

	snprintf(name, sizeof(name), "cpu%d", kw->kw_cpu);
	oid = SYSCTL_ADD_NODE(&kwq_worker_sysctl_ctx, parent, OID_AUTO, name,
	    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "class worker on this CPU");
	if (oid == NULL)
		return;
	ch = SYSCTL_CHILDREN(oid);
#define	KW_U64(field, nm, descr)					\
	SYSCTL_ADD_U64(&kwq_worker_sysctl_ctx, ch, OID_AUTO, nm, CTLFLAG_RD,	\
	    &kw->field, 0, descr)
	KW_U64(kw_rounds, "rounds", "DRR rounds completed");
	KW_U64(kw_passes, "passes", "handler invocations");
	KW_U64(kw_wakeups, "wakeups", "times the worker was woken from idle");
	KW_U64(kw_yields, "yields", "end-of-round yields");
	KW_U64(kw_tick_yields, "tick_yields", "yields forced by the tick guard between passes");
	KW_U64(kw_cap_sleeps, "cap_sleeps", "pause_sbt() sleeps taken by the CPU-share cap");
	KW_U64(kw_handbacks, "handbacks", "rounds ended early because a higher class waited");
	KW_U64(kw_busy_ns, "busy_ns", "worker CPU time inside handlers");
	KW_U64(kw_idle_ns, "idle_ns", "wall time asleep with nothing queued");
	KW_U64(kw_round, "round", "current round number");
#undef KW_U64
	SYSCTL_ADD_UINT(&kwq_worker_sysctl_ctx, ch, OID_AUTO, "nactive",
	    CTLFLAG_RD, &kw->kw_nactive, 0, "(queue, CPU) entries on the DRR ring");
	SYSCTL_ADD_UINT(&kwq_worker_sysctl_ctx, ch, OID_AUTO, "nnew",
	    CTLFLAG_RD, &kw->kw_nnew, 0, "(queue, CPU) entries on the new list, waiting for a boosted pass");
}

int
kwq_workers_start(struct sysctl_oid_list **class_oids)
{
	struct kwq_worker *kw;
	struct thread *td;
	int cls, cpu, error;

	sysctl_ctx_init(&kwq_worker_sysctl_ctx);
	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		kwq_workers[cls] = malloc(sizeof(kw) * (mp_maxid + 1), M_KWQ,
		    M_WAITOK | M_ZERO);
		kwq_chans[cls] = malloc_aligned(sizeof(uint64_t) * (mp_maxid + 1),
		    256, M_KWQ, M_WAITOK | M_ZERO);
	}
	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		CPU_FOREACH(cpu) {
			kw = malloc_domainset_aligned(sizeof(*kw), KWQ_LINE, M_KWQ,
			    DOMAINSET_PREF(pcpu_find(cpu)->pc_domain),
			    M_WAITOK | M_ZERO);
			mtx_init(&kw->kw_mtx, "kwq worker", NULL, MTX_SPIN);
			ks_worker_init(kw, &kwq_knobs[cls], 0);
			kw->kw_class = cls;
			kw->kw_cpu = cpu;
			kw->kw_pri = kwq_class_priority(cls);
			kw->kw_chan = &kwq_chans[cls][cpu];
			kw->kw_ns_scale = ks_ns_scale(cpu_tickrate());
			kw->kw_rate = cpu_tickrate();
			kwq_workers[cls][cpu] = kw;
			kwq_worker_sysctl(kw, class_oids[cls]);
			error = kthread_add(kwq_worker_main, kw, NULL, &kw->kw_td,
			    RFSTOPPED, 0, "kwq_%s/%d", kwq_class_names[cls], cpu);
			if (error != 0) {
				printf("kwq: kthread_add(kwq_%s/%d): %d\n",
				    kwq_class_names[cls], cpu, error);
				kwq_workers_stop();
				return (error);
			}
			/*
			 * sched_add() releases the thread lock (as in
			 * _taskqueue_start_threads() and kthread_add()).
			 */
			td = kw->kw_td;
			thread_lock(td);
			sched_prio(td, kw->kw_pri);
			sched_add(td, SRQ_BORING);
		}
	}
#ifdef INVARIANTS
	kwq_workers_check_chains();
#endif
	return (0);
}

/*
 * Stop and join every worker; only called with no queues in existence,
 * so no list can be non-empty.
 */
void
kwq_workers_stop(void)
{
	struct kwq_worker *kw;
	int cls, cpu;

	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		if (kwq_workers[cls] == NULL)
			continue;
		CPU_FOREACH(cpu) {
			kw = kwq_workers[cls][cpu];
			if (kw == NULL)
				continue;
			mtx_lock_spin(&kw->kw_mtx);
			kw->kw_exit = true;
			if (kw->kw_sleeping) {
				kw->kw_sleeping = false;
				wakeup_one(kw->kw_chan);
			}
			while (kw->kw_td != NULL)
				msleep_spin(&kw->kw_td, &kw->kw_mtx, "kwqexit",
				    0);
			mtx_unlock_spin(&kw->kw_mtx);
			mtx_destroy(&kw->kw_mtx);
			free(kw, M_KWQ);
			kwq_workers[cls][cpu] = NULL;
		}
		free(kwq_workers[cls], M_KWQ);
		kwq_workers[cls] = NULL;
		free(kwq_chans[cls], M_KWQ);
		kwq_chans[cls] = NULL;
	}
	sysctl_ctx_free(&kwq_worker_sysctl_ctx);
}
