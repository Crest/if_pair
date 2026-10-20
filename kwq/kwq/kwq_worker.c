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
 * class's fixed yield priority, and for the share limit (SCHED.md S6.3) a
 * demotion to idle priority bounded by a callout at the period's end.
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
#include "kwq_clock.h"

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
kwq_cputime_ns(struct kwq_worker *kw)
{
	uint64_t t;

	return (kwq_cputime_ns_tick(kw, &t));
}

/*
 * Same, and also hand back the raw tick so a caller that wants both (the
 * pass accounting) reads the clock once.
 *
 * NET (S1a.4 as amended 2026-10-20): wall time on the bound CPU.  A NET
 * worker runs at the top interrupt-thread priority and never sleeps in a
 * handler, so it leaves the CPU during a pass only when preempted by an
 * ithread of equal or higher priority or blocked on a contended sleep
 * mutex, both rare and short; a pass during which that happened is
 * charged the wall time anyway and counted in kc_switched (td_runtime
 * changes at every switch-out).  Two bare counter reads per pass, nothing
 * from the kernel's ticker.
 *
 * BULK: CPU time.  NET workers preempt BULK workers as a matter of course,
 * so wall time would charge NET's work to BULK queues.  The thread's CPU
 * time is td_runtime (accounted at each switch-out) plus the time since
 * the last switch-in; the latter needs the kernel's ticker, which on
 * arm64 is a 32-bit extension that misfires when read with interrupts
 * enabled (tools/clockprobe).  So the exact sum is taken once as a base,
 * with interrupts off, and advanced with kwq_ticks() while td_runtime has
 * not changed; a changed td_runtime re-bases: one ticker read per
 * preemption or stat tick, not per pass (kwq_clock.h).
 */
uint64_t
kwq_cputime_ns_tick(struct kwq_worker *kw, uint64_t *tick)
{
	uint64_t k, rt, c;

	if (kw->kw_class == KWQ_NET) {
		k = kwq_ticks();
		*tick = k;
		return (ks_ticks2ns(k, kw->kw_ns_scale));
	}
	rt = curthread->td_runtime;
	if (__predict_false(rt != kw->kw_seg_rt)) {
		register_t s;

		s = intr_disable();
		c = cpu_ticks();
		kw->kw_seg_cpu = rt + (c - PCPU_GET(switchtime));
		intr_restore(s);
		kw->kw_seg_rt = rt;
		kw->kw_seg_k = kwq_ticks();
		*tick = kw->kw_seg_k;
		return (ks_ticks2ns(kw->kw_seg_cpu, kw->kw_ns_scale));
	}
	k = kwq_ticks();
	*tick = k;
	return (ks_ticks2ns(kw->kw_seg_cpu + (k - kw->kw_seg_k), kw->kw_ns_scale));
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
	SDT_PROBE3(kwq, , , doorbell, kc->kc_q->kwq_class, kc->kc_cpu,
	    kw->kw_sleeping ? KWQ_DB_ASLEEP : KWQ_DB_AWAKE);
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
kwq_yield(struct kwq_worker *kw, int reason)
{
	u_int bit = 1u << kw->kw_class;
	struct thread *td = curthread;

	SDT_PROBE3(kwq, , , yield, kw->kw_class, kw->kw_cpu, reason);
	DPCPU_SET(kwq_waiting, DPCPU_GET(kwq_waiting) | bit);
	/*
	 * Over the allowance the worker is an idle-priority thread for the
	 * rest of the period (S6.3): yield as one and stay one, unless the
	 * period's deadline has already raised it.
	 */
	kern_yield(kw->kw_cap_over ? PRI_UNCHANGED : kwq_yield_prio[kw->kw_class]);
	thread_lock(td);
	sched_prio(td, (kw->kw_cap_over && !kw->kw_cap_fired) ?
	    PRI_MIN_IDLE : kw->kw_pri);
	thread_unlock(td);
	DPCPU_SET(kwq_waiting, DPCPU_GET(kwq_waiting) & ~bit);
}

/*
 * The share limit (SCHED.md S6.3): the allowance of this period is spent
 * and something else is runnable.  Become an idle-priority thread for the
 * rest of the period and switch once, so that ULE re-picks.  A thread at
 * PRI_MIN_IDLE or worse runs only when nothing else is runnable and is
 * preempted at once by anything that wakes (sched_shouldpreempt(), the
 * cpri >= PRI_MIN_IDLE rule), so from here on ULE arbitrates: the worker
 * runs on at idle priority whenever the CPU would otherwise be idle, and
 * is raised back to its class priority by the period's deadline callout
 * while it waits on the run queue (ULE requeues and preempts:
 * sched_thread_priority(), the TD_ON_RUNQ branch), or at the period's end
 * by RESTORE.  The `fired' flag and the priority are written under the
 * thread lock on both sides, so a deadline that strikes between the
 * core's decision and this switch is not undone by it.
 */
static void
kwq_cap_demote(struct kwq_worker *kw)
{
	struct thread *td = curthread;
	uint64_t t0;

	SDT_PROBE3(kwq, , , yield, kw->kw_class, kw->kw_cpu, KWQ_YIELD_CAP);
	t0 = kwq_ticks();
	thread_lock(td);
	if (kw->kw_cap_fired) {
		thread_unlock(td);
		return;
	}
	sched_prio(td, PRI_MIN_IDLE);
	mi_switch(SW_VOL | SWT_RELINQUISH);	/* releases the thread lock */
	kw->kw_cap_ns += ks_ticks2ns(kwq_ticks() - t0, kw->kw_ns_scale);
}

/* The period's deadline, run on the worker's CPU from the timer interrupt. */
static void
kwq_cap_deadline(void *arg)
{
	struct kwq_worker *kw = arg;
	struct thread *td;

	mtx_lock_spin(&kw->kw_mtx);
	td = kw->kw_td;
	if (td != NULL) {
		thread_lock(td);
		kw->kw_cap_fired = true;
		sched_prio(td, kw->kw_pri);
		thread_unlock(td);
		kw->kw_cap_restores++;
		SDT_PROBE6(kwq, , , cap, kw->kw_class, kw->kw_cpu,
		    KWQ_CAP_DEADLINE, 0, kw->kw_cap_used, 0);
	}
	mtx_unlock_spin(&kw->kw_mtx);
}

/* Act on ks_cap_check(); `wall' is the clock the core was given. */
static void
kwq_cap_act(struct kwq_worker *kw, enum ks_cap cap, uint64_t wall)
{
	const struct kwq_sched_knobs *k = kw->kw_knobs;
	uint64_t left;

	SDT_PROBE6(kwq, , , cap, kw->kw_class, kw->kw_cpu, (int)cap,
	    wall - kw->kw_cap_start, kw->kw_cap_used, 0);
	switch (cap) {
	case KS_CAP_DEMOTE:
		/*
		 * nstosbt(), not SBT_1NS: the macro is the integer 4 where the
		 * factor is 4.29, and a deadline 7 % early lands the worker in
		 * a period that has not ended by its own clock, where the
		 * first pass runs on the full quantum (a07, 2026-10-20).
		 */
		left = k->cap_period_ns - (wall - kw->kw_cap_start);
		kw->kw_cap_fired = false;
		callout_reset_sbt_on(&kw->kw_cap_co, sbinuptime() + nstosbt(left),
		    nstosbt(k->cap_period_ns / 32), kwq_cap_deadline, kw,
		    kw->kw_cpu, C_ABSOLUTE | C_DIRECT_EXEC);
		kwq_cap_demote(kw);
		break;
	case KS_CAP_YIELD:
		/* Already an idle-priority thread: ULE hands the CPU over. */
		break;
	case KS_CAP_RESTORE:
		callout_stop(&kw->kw_cap_co);
		kw->kw_cap_fired = false;
		thread_lock(curthread);
		sched_prio(curthread, kw->kw_pri);
		thread_unlock(curthread);
		break;
	default:
		break;
	}
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
	struct kwq_item *items;
	struct epoch_tracker et;
	sbintime_t now;
	uint64_t t0, age = 0, requeued0;
	u_int head_word = 0, tail_word = 0;
	bool head_nf = false, tail_nf = false;
	u_int n;
	int sign;
	bool net, has_work;
	uint64_t t1, cpu_now, wall;
	enum ks_cap cap;

	q = kc->kc_q;
	net = (q->kwq_class == KWQ_NET);

	kwq_kc_lock(kc);
	items = STAILQ_FIRST(&kc->kc_list);
	n = kc->kc_depth;
	/*
	 * Remember the tail so a handler that stops early can hand the rest
	 * back without walking to it: with a deep backlog that walk cost
	 * more than the pass's work and, charged to the queue, shrank its
	 * next budget until one CPU held every item in flight (SCHED.md
	 * S10.3, the ifpair layouts).
	 */
	kw->kw_pass_tail = STAILQ_LAST(&kc->kc_list, kwq_item, kwi_link);
	/*
	 * The handler consumes a notifier by fetching its successor with
	 * KWQ_ITEM_NEXT(); a handler that does not walk, because it only
	 * wanted the signal, would leave it pending for ever.  The head and
	 * the tail are known here without a walk, so remember their state
	 * words and settle them after the pass (below).
	 */
	if (items != NULL) {
		head_nf = KWQ_TAGBIT(items->kwi_link.stqe_next) != 0;
		if (head_nf)
			head_word = __containerof(items, struct kwq_notifier, kn_item)->kn_state;
		tail_nf = KWQ_TAGBIT(kw->kw_pass_tail->kwi_link.stqe_next) != 0;
		if (tail_nf)
			tail_word = __containerof(kw->kw_pass_tail, struct kwq_notifier, kn_item)->kn_state;
	}
	STAILQ_INIT(&kc->kc_list);
	kc->kc_depth = 0;
	/* Discard only ever happens on a drained queue. */
	sign = (!q->kwq_active && (q->kwq_flags & KWQ_F_DISCARD)) ? -1 : 1;
	/*
	 * First pass since the doorbell: sample the doorbell-to-service
	 * latency and mark the stamp used, so later passes over the same
	 * backlog do not report its age as a latency (SCHED.md S4.3, S9).
	 */
	if (n > 0 && kc->kc_empty_since != 0) {
		now = sbinuptime();
		if (now > kc->kc_empty_since) {
			age = kwq_sbt2ns(now - kc->kc_empty_since);
			if (age > kc->kc_maxlat_ns)
				kc->kc_maxlat_ns = age;
		}
		kc->kc_empty_since = 0;
	}
	kw->kw_pass_rt = curthread->td_runtime;
	cpu_now = kwq_cputime_ns_tick(kw, &t0);
	cap = ks_pass_begin(kw, kc, cpu_now,
	    net ? cpu_now : ks_ticks2ns(t0, kw->kw_ns_scale));
	kwq_kc_unlock(kc);
	if (__predict_false(kw->kw_knobs->cap_permille < 1000)) {
		if (cap == KS_CAP_RESTORE)
			kwq_cap_act(kw, cap, 0);
		SDT_PROBE6(kwq, , , cap, kw->kw_class, kw->kw_cpu, KWQ_CAP_PASS,
		    (net ? cpu_now : ks_ticks2ns(t0, kw->kw_ns_scale)) - kw->kw_cap_start,
		    kw->kw_cap_used, kw->kw_pass_budget);
	}
	SDT_PROBE4(kwq, , , pass__start, q, kc->kc_cpu, n, age);
	requeued0 = kc->kc_requeued;

	if (n > kc->kc_maxdepth)
		kc->kc_maxdepth = n;
	THREAD_NO_SLEEPING();
	if (net)
		NET_EPOCH_ENTER(et);
#ifdef VIMAGE
	if (q->kwq_flags & KWQ_F_VNET)
		CURVNET_SET_QUIET(q->kwq_vnet);
#endif

	if (items != NULL)
		q->kwq_fn(q, items, sign * (int)n, q->kwq_ctx);
	/*
	 * Settle a head or tail notifier the handler left unconsumed: its
	 * generation is still the one read at swap-out (every consume bumps
	 * it, a re-arm needs a consume first, a cancel changes only the state
	 * bits) and nothing was requeued, so it is neither re-armed nor back
	 * on the list.  A bug in the handler under INVARIANTS; consumed on
	 * its behalf otherwise, so a notifier-only client that ignores its
	 * list keeps working.
	 */
	if (items != NULL && kc->kc_requeued == requeued0) {
		if (head_nf && KWQ_NF_GEN(__containerof(items, struct kwq_notifier,
		    kn_item)->kn_state) == KWQ_NF_GEN(head_word)) {
			KASSERT(0, ("kwq: %s: handler left the head notifier "
			    "unconsumed (walk with KWQ_ITEM_NEXT)", q->kwq_name));
			kwq_notifier_consumed(q, items);
		}
		if (tail_nf && kw->kw_pass_tail != items &&
		    KWQ_NF_GEN(__containerof(kw->kw_pass_tail, struct kwq_notifier,
		    kn_item)->kn_state) == KWQ_NF_GEN(tail_word)) {
			KASSERT(0, ("kwq: %s: handler left the tail notifier "
			    "unconsumed (walk with KWQ_ITEM_NEXT)", q->kwq_name));
			kwq_notifier_consumed(q, kw->kw_pass_tail);
		}
	}

#ifdef VIMAGE
	if (q->kwq_flags & KWQ_F_VNET)
		CURVNET_RESTORE();
#endif
	if (net)
		NET_EPOCH_EXIT(et);
	THREAD_SLEEPING_OK();
	kc->kc_items += n;

	/* Emptiness re-check under the lock; the core re-places the queue. */
	kwq_kc_lock(kc);
	has_work = !STAILQ_EMPTY(&kc->kc_list);
	mtx_lock_spin(&kw->kw_mtx);
	cpu_now = kwq_cputime_ns_tick(kw, &t1);
	kc->kc_cycles += t1 - t0;
	if (__predict_false(curthread->td_runtime != kw->kw_pass_rt)) {
		/* The worker left the CPU during this pass (S1a.4). */
		kc->kc_switched++;
		kw->kw_switched++;
	}
	SDT_PROBE5(kwq, , , pass__end, q, kc->kc_cpu, n,
	    cpu_now - kw->kw_pass_start, (int)(kc->kc_requeued - requeued0));
	ks_pass_end(kw, kc, cpu_now, has_work);
	/* S6.3 on the pass's own wall stamp: NET's cpu_now is already wall. */
	wall = net ? cpu_now : ks_ticks2ns(t1, kw->kw_ns_scale);
	cap = ks_cap_check(kw, wall, sched_runnable());
	mtx_unlock_spin(&kw->kw_mtx);
	if (kc->kc_waiters != 0)
		wakeup(kc);
	kwq_kc_unlock(kc);
	if (__predict_false(cap != KS_CAP_NONE))
		kwq_cap_act(kw, cap, wall);

	/*
	 * Tick guard (SCHED.md S6.1): unconditional once a tick has passed
	 * since the last voluntary switch.  It costs at most hz switches per
	 * second and does not depend on what sched_runnable() can see, which
	 * is what makes the callout-lateness bound (B7) hold by construction,
	 * as if_pair's pair_ticked() did.
	 */
	if (ks_ticked((u_int)ticks, (u_int)curthread->td_swvoltick)) {
		kw->kw_tick_yields++;
		kwq_yield(kw, KWQ_YIELD_TICK);
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
	uint64_t wall;

#ifdef SMP
	/* Before sched_bind() to a CPU, wait for all CPUs to go on-line. */
	while (!smp_started)
		tsleep(kw, 0, "kwqsmp", hz / 4);
#endif
	thread_lock(curthread);
	sched_prio(curthread, kw->kw_pri);
	sched_bind(curthread, kw->kw_cpu);
	thread_unlock(curthread);
	kw->kw_rate = kwq_tickrate();
	kw->kw_ns_scale = ks_ns_scale(kw->kw_rate);
	kw->kw_seg_rt = ~0ULL;		/* the first CPU-time read takes a base */
	kw->kw_round_wall = kwq_sbt2ns(sbinuptime());
	kw->kw_cap_start = ks_ticks2ns(kwq_ticks(), kw->kw_ns_scale);

	for (;;) {
		mtx_lock_spin(&kw->kw_mtx);
		a = ks_next(kw, kwq_higher_waiting(kw->kw_class), &kc);
		if (a == KS_IDLE) {
			if (kw->kw_exit)
				goto out;
			kw->kw_sleeping = true;
			SDT_PROBE3(kwq, , , idle, kw->kw_class, kw->kw_cpu,
			    kw->kw_busy_ns - kw->kw_idle_busy);
			t0 = sbinuptime();
			msleep_spin(kw->kw_chan, &kw->kw_mtx, "kwqidle", 0);
			kw->kw_idle_ns += kwq_sbt2ns(sbinuptime() - t0);
			kw->kw_wakeups++;
			kw->kw_idle_busy = kw->kw_busy_ns;
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
			rate = kwq_tickrate();
			if (rate != kw->kw_rate) {
				kw->kw_rate = rate;
				kw->kw_ns_scale = ks_ns_scale(rate);
			}
			wall = kwq_sbt2ns(sbinuptime());
			SDT_PROBE4(kwq, , , round__end, kw->kw_class, kw->kw_cpu,
			    kw->kw_nactive, wall - kw->kw_round_wall);
			kw->kw_round_wall = wall;
			y = ks_round_end(kw, wall, sched_runnable());
			if (y == KS_Y_YIELD)
				kwq_yield(kw, KWQ_YIELD_ROUND);
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

/* kern.kwq.<class>.cpu.<N>.* worker statistics (../KWQ.md S10.7). */
static void
kwq_worker_sysctl(struct kwq_worker *kw, struct sysctl_oid_list *parent)
{
	struct sysctl_oid *oid;
	struct sysctl_oid_list *ch;
	char name[16];

	snprintf(name, sizeof(name), "%d", kw->kw_cpu);
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
	KW_U64(kw_cap_demotions, "cap_demotions", "periods in which the share limit was reached with other threads runnable (SCHED.md S6.3)");
	KW_U64(kw_cap_yields, "cap_yields", "times the worker gave the CPU away under the share limit");
	KW_U64(kw_cap_restores, "cap_restores", "period deadlines that raised the worker back to its class priority");
	KW_U64(kw_cap_ns, "cap_ns", "wall time given away under the share limit");
	KW_U64(kw_cap_runon, "cap_runon", "passes ended over the allowance with nothing else runnable (the limit did not bind)");
	KW_U64(kw_handbacks, "handbacks", "rounds ended early because a higher class waited");
	KW_U64(kw_switched, "switched", "passes during which the worker left the CPU (preempted or blocked); NET charges them wall time");
	KW_U64(kw_busy_ns, "busy_ns", "worker CPU time inside handlers");
	KW_U64(kw_idle_ns, "idle_ns", "wall time asleep with nothing queued");
	KW_U64(kw_round, "round", "current round number");
	KW_U64(kw_budget_calls, "budget_calls", "kwq_budget_left() calls by handlers on this worker");
	KW_U64(kw_budget_reads, "budget_reads", "of those, the ones that read the clock (SCHED.md S4.4)");
#undef KW_U64
	SYSCTL_ADD_UINT(&kwq_worker_sysctl_ctx, ch, OID_AUTO, "nactive",
	    CTLFLAG_RD, &kw->kw_nactive, 0, "(queue, CPU) entries on the DRR ring");
	SYSCTL_ADD_UINT(&kwq_worker_sysctl_ctx, ch, OID_AUTO, "nnew",
	    CTLFLAG_RD, &kw->kw_nnew, 0, "(queue, CPU) entries on the new list, waiting for a boosted pass");
}

int
kwq_workers_start(struct sysctl_oid_list **cpu_oids)
{
	struct kwq_worker *kw;
	struct thread *td;
	int cls, cpu, error;

	kwq_clock_init();
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
			callout_init(&kw->kw_cap_co, 1);
			ks_worker_init(kw, &kwq_knobs[cls], 0);
			kw->kw_class = cls;
			kw->kw_cpu = cpu;
			kw->kw_pri = kwq_class_priority(cls);
			kw->kw_chan = &kwq_chans[cls][cpu];
			kw->kw_ns_scale = ks_ns_scale(kwq_tickrate());
			kw->kw_rate = kwq_tickrate();
			kwq_workers[cls][cpu] = kw;
			kwq_worker_sysctl(kw, cpu_oids[cls]);
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
			callout_drain(&kw->kw_cap_co);
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
