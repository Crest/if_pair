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
 * hand-over between passes: the worker lowers itself to the class's
 * waiting priority (cap_prio, idle by default) and switches once, bounded
 * by a callout at the period's end.  Every pass runs at the class
 * priority: pass begin restores it, and inside a pass the limit acts only
 * through kwq_budget_left() returning 0.  Between passes the tick guard
 * yields once a hardclock tick has passed since the worker last gave the
 * CPU up voluntarily.  With the limit off the worker honours ULE's slice
 * end (TDA_SCHED) at a pass end, which a kernel thread otherwise never
 * does.
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
#include <sys/cpuset.h>
#include <sys/epoch.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <net/vnet.h>

#include "kwq_sched.h"
#include "kwq_clock.h"

struct kwq_worker **kwq_workers[KWQ_NCLASS];
static struct sysctl_ctx_list kwq_worker_sysctl_ctx;

/* BULK stealing (../SCHED.md S16); declared in kwq_internal.h. */
struct _cpuset kwq_bulk_idle;
int **kwq_topo_order;
u_short (*kwq_topo_ring)[KWQ_TOPO_RINGS];

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
 * BULK and BLOCKING: CPU time.  NET workers preempt them as a matter of
 * course and a BLOCKING handler sleeps inside its passes, so wall time
 * would charge NET's work and the sleep to their queues; the share limit
 * charges the same (S6.3).  The thread's CPU time is td_runtime
 * (accounted at each switch-out) plus the time since the last switch-in;
 * the latter needs the kernel's ticker, which on arm64 is a 32-bit
 * extension that misfires when read with interrupts enabled
 * (tools/clockprobe).  So the exact sum is taken once as a base, with
 * interrupts off, and advanced with kwq_ticks() while td_runtime has not
 * changed; a changed td_runtime re-bases: one ticker read per preemption
 * or stat tick, not per pass (kwq_clock.h).
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
 * The idle-class wait for same-CPU producers (../KWQ.md S16, off unless
 * kern.kwq.<class>.wake_delay_us is set).  A wake at the class priority
 * preempts whatever runs on the worker's CPU at its next instruction, so
 * a producer on that CPU never gets to finish its burst (the guest's
 * receiver blocked 9.5 k times a second inside its socket-lock section,
 * ../../NOTES.md 2026-10-09).  For a producer on the worker's own CPU the
 * worker is therefore made runnable in the idle scheduling class at the
 * class's cap_prio instead: ULE runs it when the producer sleeps or when
 * its CPU would otherwise idle, and the deadline below bounds the wait.
 * The idle class, rather than cap_prio in the worker's own class, because
 * ULE rewrites a timeshare sleeper's priority to PRI_MIN_BATCH when it is
 * worse (static_boost), which would make every parking priority 120; in
 * the idle class the priority stands and sched_priority() leaves it
 * alone.  The class is restored with the priority at the promotion.
 *
 * Promote a worker waiting in the idle class.  kw_mtx held, kw_lowpri
 * set.  On a thread on the run queue ULE requeues it and preempts
 * (sched_thread_priority(), the TD_ON_RUNQ branch).
 */
static void
kwq_wake_promote(struct kwq_worker *kw, int reason)
{
	struct thread *td = kw->kw_td;
	uint64_t ns;

	mtx_assert(&kw->kw_mtx, MA_OWNED);
	KASSERT(kw->kw_lowpri, ("kwq_wake_promote: not waiting"));
	/*
	 * A deadline still pending would fire as a no-op, one timer
	 * interrupt per wake; stopping it costs 49-80 ns (tools/schedprobe).
	 * The deadline handler clears kw_wake_armed before it calls here, so
	 * it never stops itself.
	 */
	if (kw->kw_wake_armed) {
		callout_stop(&kw->kw_wake_co);
		kw->kw_wake_armed = false;
	}
	if (td != NULL) {
		thread_lock(td);
		sched_class(td, kw->kw_pri_class);	/* back from the idle class */
		sched_prio(td, kw->kw_pri);
		thread_unlock(td);
	}
	kw->kw_lowpri = false;
	ns = kwq_sbt2ns(sbinuptime() - kw->kw_wake_since);
	kw->kw_wake_low_ns += ns;
	switch (reason) {
	case KWQ_WAKE_DEADLINE:
		kw->kw_wake_prom_deadline++;
		break;
	case KWQ_WAKE_SELF:
		kw->kw_wake_self++;
		break;
	default:
		kw->kw_wake_prom_urgent++;
		break;
	}
	SDT_PROBE4(kwq, , , wake, kw->kw_class, kw->kw_cpu, reason, ns);
}

/* The wait's deadline, on the worker's CPU (the producer's, here). */
static void
kwq_wake_deadline(void *arg)
{
	struct kwq_worker *kw = arg;

	mtx_lock_spin(&kw->kw_mtx);
	kw->kw_wake_armed = false;
	if (kw->kw_lowpri)
		kwq_wake_promote(kw, KWQ_WAKE_DEADLINE);
	mtx_unlock_spin(&kw->kw_mtx);
}

/*
 * Make the sleeping worker runnable in the idle class for a producer on
 * its own CPU and arm the deadline.  kw_mtx held, kw_sleeping true, the
 * worker's thread alive.  There is no idle-CPU shortcut here (the branch
 * this comes from raised a worker at once when its CPU was idle): the
 * producer is running on that CPU, so the CPU is never idle.  The
 * deadline is armed on the worker's CPU, which is the producer's own, so
 * it is a local callout wheel either way and the branch's
 * kern.kwq.wake_co_local knob has nothing to choose between.
 */
static void
kwq_wake_park(struct kwq_worker *kw, struct kwq_cpu *kc)
{
	struct thread *td = kw->kw_td;
	sbintime_t delay;

	mtx_assert(&kw->kw_mtx, MA_OWNED);
	delay = (sbintime_t)kwq_wake_delay_us[kw->kw_class] * SBT_1US;
	kw->kw_sleeping = false;
	kc->kc_doorbells++;
	kw->kw_wakes_low++;
	kw->kw_wake_since = sbinuptime();
	thread_lock(td);
	sched_class(td, PRI_IDLE);
	sched_prio(td, kwq_cap_prio[kw->kw_class]);
	thread_unlock(td);
	kw->kw_lowpri = true;
	wakeup_one(kw->kw_chan);
	kw->kw_wake_armed = true;
	callout_reset_sbt_on(&kw->kw_wake_co, kw->kw_wake_since + delay,
	    delay >> 4, kwq_wake_deadline, kw, kw->kw_cpu,
	    C_ABSOLUTE | C_DIRECT_EXEC);
}

/*
 * The urgent items' bound inside the tick regime (tick_urgent_us): the
 * first urgent item that finds the worker asleep arms a wake that much
 * later, the ones after it until then ride on it.  A bound of L or more
 * is the tick's own, because the sleep's timeout comes first: nothing is
 * armed and the item rides on the tick like a plain one, uncounted.  The
 * arm is called with kw_mtx held; the wake takes it.
 */
static void
kwq_tick_urgent_fire(void *arg)
{
	struct kwq_worker *kw = arg;

	mtx_lock_spin(&kw->kw_mtx);
	kw->kw_turg_armed = false;
	if (kw->kw_sleeping) {
		kw->kw_sleeping = false;
		kw->kw_turg_wakes++;
		wakeup_one(kw->kw_chan);
	}
	mtx_unlock_spin(&kw->kw_mtx);
}

static void
kwq_tick_urgent_arm(struct kwq_worker *kw)
{
	sbintime_t delay;

	mtx_assert(&kw->kw_mtx, MA_OWNED);
	if (kwq_tick_urgent_us[kw->kw_class] >= kwq_tick_us[kw->kw_class])
		return;
	kw->kw_turg_deferred++;
	if (kw->kw_turg_armed)
		return;
	delay = (sbintime_t)kwq_tick_urgent_us[kw->kw_class] * SBT_1US;
	kw->kw_turg_armed = true;
	callout_reset_sbt_on(&kw->kw_turg_co, delay, delay >> 3,
	    kwq_tick_urgent_fire, kw, kw->kw_cpu, C_DIRECT_EXEC);
}

/*
 * Ring the doorbell for kc: place it with the core (new list or, by the
 * grace rule, the ring) and wake the worker if it sleeps.  Called with kc
 * locked; kc_mtx (sleep or spin) -> kw_mtx (spin) is the lock order.
 * An urgent ring is never held back by either regime: a signal (a
 * notifier, a drain, a cancel) wakes at once, and so does an urgent
 * item (kwq_enqueue_urgent()) except in the tick regime with
 * tick_urgent_us set, where it waits at most that bound.
 *
 * Precedence of the two opt-in regimes of ../KWQ.md S16, both decided
 * here: a producer on the worker's own CPU takes the idle-class path
 * when wake_delay_us is set, the tick regime notwithstanding, because
 * the wait serves this list when the producer sleeps, which is sooner
 * than the next tick and is what a same-CPU producer asked for (the
 * other order cost lab's co-located receiver the whole gain of the wait
 * when both knobs were set, WAKE.md S12); otherwise a worker in the
 * tick regime skips the doorbell, because its own timeout serves this
 * list within L; otherwise the plain doorbell wakes the worker at the
 * class priority, as it always has.  An urgent ring takes neither
 * regime.  A producer on another CPU never takes the idle-class wait,
 * which would make it pay for a wake it did not cause (SCHED.md B5a),
 * but the tick regime skips its doorbell as anyone's.
 */
void
kwq_doorbell(struct kwq_cpu *kc, int ring)
{
	struct kwq_worker *kw;
	int state;
	bool urgent = ring != KWQ_RING_PLAIN;

	kw = kwq_workers[kc->kc_q->kwq_class][kc->kc_cpu];
	mtx_lock_spin(&kw->kw_mtx);
	KASSERT(kc->kc_onlist == KWQ_ON_NONE, ("kwq_doorbell: %s cpu%d on list %u",
	    kc->kc_q->kwq_name, kc->kc_cpu, kc->kc_onlist));
	ks_doorbell(kw, kc);
	/* P8: a CPU holding stealable work keeps the starved tick armed (K12). */
	if (__predict_false(kwq_bulk_place == KWQ_PLACE_SHARD) &&
	    (kc->kc_q->kwq_flags & KWQ_F_STEALABLE) != 0)
		kwq_watch_arm(kw);
	if (!kw->kw_sleeping) {
		state = KWQ_DB_AWAKE;
		/*
		 * Awake but still waiting in the idle class for its CPU: an
		 * urgent ring, or an item from another CPU, is owed the
		 * class priority now, so the wait ends here.
		 */
		if (__predict_false(kw->kw_lowpri) &&
		    (urgent || curcpu != kc->kc_cpu))
			kwq_wake_promote(kw, KWQ_WAKE_URGENT);
	} else if (!urgent && kwq_wake_delay_us[kw->kw_class] != 0 &&
	    curcpu == kc->kc_cpu && kw->kw_td != NULL) {
		/*
		 * The idle-class wait (KWQ.md S16), ahead of the tick regime:
		 * the worker becomes runnable now and runs when this producer
		 * sleeps, so it serves the list at least as soon as a tick
		 * would and without preempting the producer.  In the regime
		 * the sleep this ends carried the bound as its timeout; the
		 * park cuts it short and the regime itself stands (the wake
		 * is not a tick, so kw_tick_round stays clear and the round
		 * is not judged for the exit; a park that outlasts the
		 * sleep's timeout returns as the timeout, and is counted and
		 * judged as a tick).
		 */
		kwq_wake_park(kw, kc);
		state = KWQ_DB_LOWPRI;
	} else if (kw->kw_tick && !urgent) {
		/*
		 * The tick regime (KWQ.md S16): the worker sleeps with the
		 * class's bound as its timeout and will serve this list then.
		 * The list is placed, nothing is woken, so the producer pays
		 * the append alone.
		 */
		kc->kc_doorbells_skipped++;
		state = KWQ_DB_TICKED;
	} else if (kw->kw_tick && ring == KWQ_RING_ITEM &&
	    kwq_tick_urgent_us[kw->kw_class] != 0) {
		/*
		 * An urgent item in the tick regime with tick_urgent_us
		 * set: the list is placed, and the worker wakes at the
		 * urgent bound instead of the tick, so that the urgent items
		 * within it share one wake.
		 */
		kwq_tick_urgent_arm(kw);
		kc->kc_doorbells_skipped++;
		state = KWQ_DB_TICKED;
	} else {
		kw->kw_sleeping = false;
		kc->kc_doorbells++;
		wakeup_one(kw->kw_chan);
		state = KWQ_DB_ASLEEP;
	}
	SDT_PROBE3(kwq, , , doorbell, kc->kc_q->kwq_class, kc->kc_cpu, state);
	mtx_unlock_spin(&kw->kw_mtx);
}

/*
 * An urgent item added to a list that is already placed with the core
 * (KWQ.md S16).  kwq_doorbell() rings only when a list goes from idle to
 * placed, so in either regime the first item of a window places the list
 * and every item after it rings nothing: without this, an urgent item
 * that is not the first one still waits for the tick, which is the
 * common case for a small packet behind a bulk segment.  This wakes the
 * worker without touching the placement, which is what the tick would
 * have done, and ends the idle-class wait if one is running; in the tick
 * regime with tick_urgent_us set it arms that bound instead, as
 * kwq_doorbell() does for the first item.  Called
 * with kc locked and kc_state KWQ_CPU_WAKING (placed and not yet
 * served): a running pass needs no wake, and a parked queue cannot be
 * served before its refill.  Returns true if it woke anything, for the
 * enqueue probe.
 */
bool
kwq_urgent_nudge(struct kwq_cpu *kc, int ring)
{
	struct kwq_worker *kw;
	int state = KWQ_DB_ASLEEP;
	bool woke = false;

	kw = kwq_workers[kc->kc_q->kwq_class][kc->kc_cpu];
	mtx_lock_spin(&kw->kw_mtx);
	if (__predict_false(kw->kw_lowpri)) {
		/* Runnable in the idle class, not asleep: as kwq_doorbell(). */
		kwq_wake_promote(kw, KWQ_WAKE_URGENT);
		state = KWQ_DB_AWAKE;
		woke = true;
	} else if (kw->kw_sleeping && kw->kw_tick && ring == KWQ_RING_ITEM &&
	    kwq_tick_urgent_us[kw->kw_class] != 0) {
		kwq_tick_urgent_arm(kw);	/* as kwq_doorbell() */
	} else if (kw->kw_sleeping) {
		kw->kw_sleeping = false;
		kc->kc_doorbells++;
		kw->kw_wakes_urgent++;
		wakeup_one(kw->kw_chan);
		woke = true;
	}
	if (woke)
		SDT_PROBE3(kwq, , , doorbell, kc->kc_q->kwq_class, kc->kc_cpu,
		    state);
	mtx_unlock_spin(&kw->kw_mtx);
	return (woke);
}

/*
 * The tick regime's entry (KWQ.md S16), at a doorbell wake: three
 * consecutive sleeps shorter than the bound L, which is density over more
 * than one burst.  The first build entered on two wakes within L, which
 * every burst of two items satisfies, and so charged a short burst with a
 * long pause one L on its tail for one saved wake (WAKE.md S10.3).  A
 * sleep of L or more resets the count, and the hold-off left by a barren
 * episode bars the entry until it expires.
 */
static void
kwq_tick_entry(struct kwq_worker *kw, sbintime_t tick, sbintime_t slept,
    sbintime_t now)
{
	if (tick == 0 || slept >= tick) {
		kw->kw_tick_short = 0;
		return;
	}
	if (kw->kw_tick || ++kw->kw_tick_short < KWQ_TICK_ENTER ||
	    now < kw->kw_tick_holdoff)
		return;
	kw->kw_tick = true;
	kw->kw_tick_coalesced = false;
	kw->kw_tick_short = 0;
	kw->kw_tick_enters++;
	kw->kw_tick_start = now;
}

/*
 * The exit (KWQ.md S16): a tick whose round served one element or none
 * coalesced nothing, so the next element pays a wake rather than waiting
 * L for nothing, and a silence leaves no timer armed.  An episode in
 * which no tick coalesced anything doubles the entry hold-off, 2L up to
 * 1024L; one that did coalesce clears it, or bursts spaced about twice
 * the bound would enter only every other burst.
 */
static void
kwq_tick_exit(struct kwq_worker *kw, sbintime_t tick, sbintime_t now)
{
	if (kw->kw_tick_items == 0)
		kw->kw_ticks_empty++;
	else
		kw->kw_ticks_single++;
	kw->kw_tick = false;
	kw->kw_tick_short = 0;
	kw->kw_tick_exits++;
	kw->kw_tick_ns += kwq_sbt2ns(now - kw->kw_tick_start);
	if (kw->kw_tick_coalesced) {
		kw->kw_tick_backoff = 2 * tick;
		kw->kw_tick_holdoff = 0;
	} else {
		kw->kw_tick_barren++;
		if (kw->kw_tick_backoff < 2 * tick)
			kw->kw_tick_backoff = 2 * tick;
		else if (kw->kw_tick_backoff < 1024 * tick)
			kw->kw_tick_backoff *= 2;
		kw->kw_tick_holdoff = now + kw->kw_tick_backoff;
	}
}

/*
 * Give the CPU away for one scheduling decision at `prio' (the class's
 * fixed yield priority, or PRI_USER for a slice yield), then re-assert the
 * class priority (kern_yield() rewrote td_base_pri, and a kthread has no
 * userret to restore it).  The hand-back bit is set for the duration: this
 * is a yield in the S6.4 sense, the worker wants the CPU back.
 */
static void
kwq_yield(struct kwq_worker *kw, int reason, int prio)
{
	u_int bit = 1u << kw->kw_class;
	struct thread *td = curthread;

	SDT_PROBE3(kwq, , , yield, kw->kw_class, kw->kw_cpu, reason);
	DPCPU_SET(kwq_waiting, DPCPU_GET(kwq_waiting) | bit);
	kern_yield(prio);
	thread_lock(td);
	sched_prio(td, kw->kw_pri);
	thread_unlock(td);
	DPCPU_SET(kwq_waiting, DPCPU_GET(kwq_waiting) & ~bit);
}

/*
 * The share limit's hand-over (SCHED.md S6.3): the allowance of this
 * period is spent and something else is runnable.  Lower the priority to
 * the class's waiting priority and switch once, so that ULE re-picks.  At
 * the default, PRI_MIN_IDLE, the worker runs again only when nothing else
 * is runnable and is preempted by anything that wakes
 * (sched_shouldpreempt(), the cpri >= PRI_MIN_IDLE rule); at
 * PRI_MAX_TIMESHARE it is the last batch thread.  Either way the worker
 * is at the waiting priority only while it is off the CPU: the moment the
 * switch returns it re-asserts the class priority (and pass begin does so
 * again as a safety net), so what ULE may preempt is one instruction of
 * bookkeeping, never a handler.  Restoring at pass begin alone left a
 * few microseconds of bookkeeping at the waiting priority, enough for a
 * bursty competitor to preempt the worker there ten times a second on
 * the guest (2026-10-22).  The deadline callout
 * raises the worker while it waits on the run queue (ULE requeues and
 * preempts: sched_thread_priority(), the TD_ON_RUNQ branch).  Not a
 * yield in the S6.4 sense: the hand-back bit stays clear, because the
 * worker has forfeited the CPU rather than asked for it back, and a BULK
 * worker on this CPU runs full passes meanwhile.  The voluntary switch
 * satisfies the tick guard (td_swvoltick), and the core skips the
 * round-end yield of a round that ended this way.  The `fired' flag and
 * the priority are written under the thread lock on both sides, so a
 * deadline that strikes between the core's decision and this switch is
 * not undone by it; after the deadline the worker runs on at the class
 * priority until its own clock rolls the period.
 */
static void
kwq_cap_handover(struct kwq_worker *kw)
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
	sched_prio(td, kwq_cap_prio[kw->kw_class]);
	mi_switch(SW_VOL | SWT_RELINQUISH);	/* releases the thread lock */
	kw->kw_cap_ns += ks_ticks2ns(kwq_ticks() - t0, kw->kw_ns_scale);
	thread_lock(td);
	sched_prio(td, kw->kw_pri);
	thread_unlock(td);
}

/* Back to the class priority if a hand-over left the worker below it. */
static inline void
kwq_class_prio_restore(struct kwq_worker *kw)
{
	struct thread *td = curthread;

	if (__predict_true(td->td_base_pri == kw->kw_pri))
		return;
	thread_lock(td);
	sched_prio(td, kw->kw_pri);
	thread_unlock(td);
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
		/*
		 * Both deadlines can be pending at once: end the idle-class
		 * wait first, so that the worker comes out of this with its
		 * own class and the class priority, not the class priority
		 * inside the idle class (KWQ.md S16).
		 */
		if (__predict_false(kw->kw_lowpri))
			kwq_wake_promote(kw, KWQ_WAKE_URGENT);
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
		kwq_cap_handover(kw);
		break;
	case KS_CAP_YIELD:
		kwq_cap_handover(kw);
		break;
	case KS_CAP_RESTORE:
		callout_stop(&kw->kw_cap_co);
		kw->kw_cap_fired = false;
		kwq_class_prio_restore(kw);
		break;
	default:
		break;
	}
}

/*
 * One pass over one (queue, CPU): swap the lists out, run the handler with
 * no service lock held, charge the pass, re-check emptiness.
 *
 * Items and notifiers travel as one list, in arrival order, and the
 * handler is called once with all of it (../KWQ.md S3; notifiers came
 * first, one call each, until 2026-10-08).  A notifier's own link is
 * tagged: the handler's KWQ_ITEM_NEXT() fetches the successor and then
 * flips the notifier from pending to idle, after which a re-notify may
 * relink it into the live list.  A head or tail notifier the handler
 * left unconsumed is settled after the call.
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
	u_int n, depth0;
	int sign;
	bool net, has_work, cut, helper, donor;
	uint64_t t1, cpu_now, wall;
	enum ks_cap cap;

	q = kc->kc_q;
	net = (q->kwq_class == KWQ_NET);

	kwq_kc_lock(kc);
	items = STAILQ_FIRST(&kc->kc_list);
	n = depth0 = kc->kc_depth;
	/*
	 * A stealable queue's pass takes at most steal_batch items and leaves
	 * the rest on the live list (../SCHED.md S16): a pass's batch belongs
	 * to its handler and cannot be stolen, so a pass that took a whole
	 * backlog would keep it out of reach of every idle worker until the
	 * handler requeued it, and the next pass would swap it out again
	 * microseconds later.  While a BULK worker near this CPU sleeps, the
	 * pass takes one item and wakes that worker for the rest, the work
	 * stealing deque's rule: a few large items (four writers' 1 MiB bios
	 * on kwq-geli) otherwise all fit one pass and nobody can help.  The
	 * cut is a walk of at most steal_batch links under the list lock.
	 * Stealable lists hold no notifiers (kwq_notify() refuses them), so
	 * no tag is cut through.
	 */
	cut = helper = donor = false;
	if (__predict_false(q->kwq_flags & KWQ_F_STEALABLE) && kwq_steal_on &&
	    n > 1) {
		u_int k, take = kwq_steal_batch;

		helper = kwq_steal_idle_near(kc->kc_cpu);
		if (helper)
			take = 1;
		if (n > take) {
			struct kwq_item *t = items;

			for (k = 1; k < take; k++)
				t = t->kwi_link.stqe_next;
			kc->kc_list.stqh_first = t->kwi_link.stqe_next;
			t->kwi_link.stqe_next = NULL;
			kc->kc_depth = n - take;
			n = take;
			kw->kw_pass_tail = t;
			cut = true;
			kwq_list_check(kc, "cut");
		}
	}
	if (!cut)
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
	if (!cut) {
		STAILQ_INIT(&kc->kc_list);
		kc->kc_depth = 0;
	}
	/*
	 * P8 (place=2): a decision point, with this pass's n items ahead of
	 * what stays on the list (the lone item's rule, kwq_shard.c).
	 */
	if (__predict_false(kwq_bulk_place == KWQ_PLACE_SHARD) &&
	    (q->kwq_flags & KWQ_F_STEALABLE) != 0) {
		mtx_lock_spin(&kw->kw_mtx);
		if (cut && helper)
			(void)kwq_donor_helper(kw, kc);	/* the rest is the helper's */
		donor = kwq_donor_point(kw, kc, n);
		mtx_unlock_spin(&kw->kw_mtx);
	} else if (__predict_false(kwq_bulk_place == KWQ_PLACE_GOSSIP) &&
	    q->kwq_class == KWQ_BULK) {
		mtx_lock_spin(&kw->kw_mtx);
		kwq_gossip_point(kw, kc, n);	/* the load figure, this pass ahead */
		mtx_unlock_spin(&kw->kw_mtx);
	}
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
	wall = net ? cpu_now : ks_ticks2ns(t0, kw->kw_ns_scale);
	cap = ks_pass_begin(kw, kc, cpu_now, wall,
	    kw->kw_knobs->cap_permille < 1000 && sched_runnable());
	kwq_kc_unlock(kc);
	/*
	 * S6.3 and the promise of ../KWQ.md S3: a pass runs at the class
	 * priority, or above it if a turnstile lent one, and in the class
	 * the worker started in.  One compare puts the worker back if a
	 * hand-over left it at the waiting priority (30-80 ns when it
	 * fires, S15), and the same compare is the tripwire: under the
	 * pass-level share limit the hand-over re-asserts the priority the
	 * moment its switch returns, so prio_violations reads zero.  The
	 * default build counts, make KWQ_HARD_INVARIANTS=1 asserts.
	 */
	if (__predict_false(curthread->td_priority > kw->kw_pri ||
	    curthread->td_pri_class != kw->kw_pri_class)) {
		kw->kw_prio_violations++;
#ifndef KWQ_SOFT_INVARIANTS
		KASSERT(0, ("kwq: %s/%d pass at priority %d class %d, expected %d/%d",
		    kwq_class_names[kw->kw_class], kw->kw_cpu,
		    curthread->td_priority, curthread->td_pri_class,
		    kw->kw_pri, kw->kw_pri_class));
#endif
	}
	kwq_class_prio_restore(kw);
	if (__predict_false(cut && helper))
		kwq_steal_wake(kc->kc_cpu, kc);	/* the rest of the list is for it */
	else if (__predict_false(donor))
		(void)kwq_shard_wake(kc->kc_cpu, NULL);	/* it just became a donor */
	if (__predict_false(kw->kw_knobs->cap_permille < 1000)) {
		if (cap == KS_CAP_RESTORE)
			kwq_cap_act(kw, cap, wall);
		SDT_PROBE6(kwq, , , cap, kw->kw_class, kw->kw_cpu, KWQ_CAP_PASS,
		    wall - kw->kw_cap_start, kw->kw_cap_used, kw->kw_pass_budget);
	}
	SDT_PROBE4(kwq, , , pass__start, q, kc->kc_cpu, n, age);
	requeued0 = kc->kc_requeued;

	if (depth0 > kc->kc_maxdepth)
		kc->kc_maxdepth = depth0;
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
	kw->kw_pass_switched = (curthread->td_runtime != kw->kw_pass_rt);
	if (__predict_false(kw->kw_pass_switched)) {
		/* The worker left the CPU during this pass (S1a.4). */
		kc->kc_switched++;
		kw->kw_switched++;
	}
	kw->kw_tick_items += n;			/* what this tick's round served */
	SDT_PROBE5(kwq, , , pass__end, q, kc->kc_cpu, n,
	    cpu_now - kw->kw_pass_start, (int)(kc->kc_requeued - requeued0));
	ks_pass_end(kw, kc, cpu_now, has_work);
	donor = false;
	if (__predict_false(kwq_bulk_place == KWQ_PLACE_SHARD) &&
	    (q->kwq_flags & KWQ_F_STEALABLE) != 0) {
		kwq_pass_cost(kc, cpu_now - kw->kw_pass_start, n);
		donor = kwq_donor_point(kw, has_work ? kc : NULL, 0);
	} else if (__predict_false(kwq_bulk_place == KWQ_PLACE_GOSSIP) &&
	    q->kwq_class == KWQ_BULK) {
		kwq_pass_cost(kc, cpu_now - kw->kw_pass_start, n);
		kwq_gossip_point(kw, has_work ? kc : NULL, 0);
	}
	/* S6.3 on the pass's own wall stamp: NET's cpu_now is already wall. */
	wall = net ? cpu_now : ks_ticks2ns(t1, kw->kw_ns_scale);
	cap = ks_cap_check(kw, wall, sched_runnable());
	mtx_unlock_spin(&kw->kw_mtx);
	if (kc->kc_waiters != 0)
		wakeup(kc);
	kwq_kc_unlock(kc);
	if (__predict_false(donor))
		(void)kwq_shard_wake(kc->kc_cpu, NULL);
	if (__predict_false(cap != KS_CAP_NONE))
		kwq_cap_act(kw, cap, wall);
	else if (__predict_false(kw->kw_knobs->cap_permille >= 1000 &&
	    td_ast_pending(curthread, TDA_SCHED))) {
		/*
		 * Limit off (SCHED.md S6.3): ULE ended this thread's slice
		 * and asked for a reschedule, which only userret() would
		 * honour.  Honour it here, between passes, at the priority
		 * ULE would give a process; the switch clears the request
		 * (sched_switch()).  With the limit on the hand-over above
		 * comes first and its switch clears the bit, so this branch
		 * is inert then.
		 */
		kw->kw_slice_yields++;
		kwq_yield(kw, KWQ_YIELD_SLICE, PRI_USER);
	}

	/*
	 * Tick guard (SCHED.md S6.1): unconditional once a tick has passed
	 * since the last voluntary switch.  It costs at most hz switches per
	 * second and does not depend on what sched_runnable() can see, which
	 * is what makes the callout-lateness bound (B7) hold by construction,
	 * as if_pair's pair_ticked() did.  A hand-over just above was a
	 * voluntary switch and satisfies it.
	 */
	if (ks_ticked((u_int)ticks, (u_int)curthread->td_swvoltick)) {
		kw->kw_tick_yields++;
		kwq_yield(kw, KWQ_YIELD_TICK, kwq_yield_prio[kw->kw_class]);
	}
}

/*
 * Cache topology for stealing, scatter and KWQ_CPU_ANY (../SCHED.md S16).
 * For each CPU, every other present CPU in rings of growing distance:
 * the CPUs of the smallest topology group around it that has any, then
 * those its parent adds, and so on to the root, the same tree ULE steals
 * along (smp_topo()); within a ring by distance in ids, one up, one down,
 * two up, two down and so on, wrapping.  The alternation makes the cut at
 * steal_scan symmetric: a worker that a CPU's backlog wakes (it is near
 * the victim in the victim's order) has the victim near in its own order
 * too.  The first version went up from the next id only, so on a ring
 * wider than steal_scan (a07's 127) the helpers a CPU woke, v+1..v+16,
 * searched v+2..v+32 and never reached v (found in review, 2026-11-03).  A ring is a
 * group that adds CPUs; levels that add none (a single-CPU leaf, a
 * package of one core) are skipped, so scope 1 is the nearest sharing
 * on every machine: the SMT sibling on lab, everything on a flat
 * topology such as a07's or the guest's.
 */
#ifdef SMP
static struct cpu_group *
kwq_topo_leaf(struct cpu_group *cg, int cpu)
{
	int i;

	for (;;) {
		for (i = 0; i < cg->cg_children; i++)
			if (CPU_ISSET(cpu, &cg->cg_child[i].cg_mask))
				break;
		if (i == cg->cg_children)
			return (cg);
		cg = &cg->cg_child[i];
	}
}
#endif

SYSCTL_DECL(_kern_kwq);
static struct sysctl_ctx_list kwq_topo_sysctl;

/* kern.kwq.topo: each CPU's search order, its rings separated by "|". */
static int
kwq_sysctl_topo(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	int cpu, error, i, r, start, end;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	CPU_FOREACH(cpu) {
		sbuf_printf(&sb, "\n%d:", cpu);
		for (r = start = 0; r < KWQ_TOPO_RINGS; r++) {
			end = kwq_topo_ring[cpu][r];
			if (end == start)
				continue;
			if (start > 0)
				sbuf_cat(&sb, " |");
			for (i = start; i < end; i++)
				sbuf_printf(&sb, " %d", kwq_topo_order[cpu][i]);
			start = end;
		}
	}
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static void
kwq_topo_add(int cpu, cpuset_t *seen, const cpuset_t *grp, int *n, int *r)
{
	int c, d, ids = (int)mp_maxid + 1, s, added = 0;

	for (d = 1; d < ids; d++) {
		for (s = 0; s < 2; s++) {
			c = s == 0 ? (cpu + d) % ids : (cpu - d + ids) % ids;
			if (CPU_ABSENT(c) || CPU_ISSET(c, seen) ||
			    (grp != NULL && !CPU_ISSET(c, grp)))
				continue;
			CPU_SET(c, seen);
			kwq_topo_order[cpu][(*n)++] = c;
			added++;
		}
	}
	if (added == 0)
		return;
	if (*r < KWQ_TOPO_RINGS)
		(*r)++;
	kwq_topo_ring[cpu][*r - 1] = (u_short)*n;	/* the last ring absorbs the rest */
}

void
kwq_topo_init(void)
{
#ifdef SMP
	struct cpu_group *cg;
#endif
	cpuset_t seen, last;
	int cpu, n, r, i;

	kwq_topo_order = malloc(sizeof(*kwq_topo_order) * (mp_maxid + 1), M_KWQ,
	    M_WAITOK | M_ZERO);
	kwq_topo_ring = malloc(sizeof(*kwq_topo_ring) * (mp_maxid + 1), M_KWQ,
	    M_WAITOK | M_ZERO);
	CPU_FOREACH(cpu) {
		kwq_topo_order[cpu] = malloc(sizeof(int) * (mp_maxid + 1), M_KWQ,
		    M_WAITOK | M_ZERO);
		CPU_ZERO(&seen);
		CPU_SET(cpu, &seen);
		CPU_ZERO(&last);
		n = r = 0;
#ifdef SMP
		cg = kwq_topo_leaf(smp_topo(), cpu);
#ifdef KWQ_TOPO_SIBLING_LAST
		/*
		 * The A/B build of SPEC-p6g.md P6g.0 (make KWQ_SIBLING_LAST=1):
		 * the SMT siblings after every other CPU, a ring of their own.
		 */
		if ((cg->cg_flags & CG_FLAG_THREAD) != 0) {
			CPU_COPY(&cg->cg_mask, &last);
			CPU_CLR(cpu, &last);
			CPU_OR(&seen, &seen, &last);
		}
#endif
		for (; cg != NULL; cg = cg->cg_parent)
			kwq_topo_add(cpu, &seen, &cg->cg_mask, &n, &r);
#endif
		kwq_topo_add(cpu, &seen, NULL, &n, &r);	/* anything the tree missed */
		if (!CPU_EMPTY(&last)) {
			CPU_ANDNOT(&seen, &seen, &last);
			kwq_topo_add(cpu, &seen, &last, &n, &r);
		}
		for (i = r; i < KWQ_TOPO_RINGS; i++)
			kwq_topo_ring[cpu][i] = (u_short)n;
	}
	CPU_ZERO(&kwq_bulk_idle);
	sysctl_ctx_init(&kwq_topo_sysctl);
	SYSCTL_ADD_PROC(&kwq_topo_sysctl, SYSCTL_STATIC_CHILDREN(_kern_kwq),
	    OID_AUTO, "topo", CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
	    NULL, 0, kwq_sysctl_topo, "A",
	    "each CPU's placement and steal order (kwq_topo_order), rings separated by |");
}

void
kwq_topo_fini(void)
{
	int cpu;

	if (kwq_topo_order == NULL)
		return;
	sysctl_ctx_free(&kwq_topo_sysctl);	/* waits for a reader in the handler */
	CPU_FOREACH(cpu)
		free(kwq_topo_order[cpu], M_KWQ);
	free(kwq_topo_order, M_KWQ);
	free(kwq_topo_ring, M_KWQ);
	kwq_topo_order = NULL;
	kwq_topo_ring = NULL;
}

/* How far down kwq_topo_order[cpu] a search may go: scope rings, scan CPUs. */
u_int
kwq_topo_reach(int cpu)
{
	u_int scope = kwq_steal_scope, n;

	if (scope == 0)
		return (0);
	if (scope > KWQ_TOPO_RINGS)
		scope = KWQ_TOPO_RINGS;
	n = kwq_topo_ring[cpu][scope - 1];
	return (n < kwq_steal_scan ? n : kwq_steal_scan);
}

/*
 * Is the victim's worker going to serve this one-item list soon by
 * itself?  A hint read without its lock: awake and not inside a pass
 * means it is between passes or on its way to this list.  A worker in a
 * pass (of this queue or another: BULK items are long, and the pass took
 * what it could already), asleep with the list placed (the tick regime)
 * or waiting in the idle class leaves the item waiting, and those are
 * the cases a lone item is worth stealing in.
 */
static bool
kwq_steal_lone_ok(struct kwq_cpu *vkc)
{
	struct kwq_worker *vkw = kwq_workers[KWQ_BULK][vkc->kc_cpu];

	return (vkw->kw_sleeping || vkw->kw_lowpri || vkw->kw_cur != NULL);
}

/*
 * Steal (../SCHED.md S16): a BULK worker with nothing of its own takes a
 * batch of a stealable queue from another CPU, nearest first, and MOVES
 * it to its own list of the same queue, where it runs as ordinary passes
 * and is charged as all work is, on the CPU that runs it.  The batch is
 * the older half of the victim's live list, at most steal_max items, a
 * lone item only when its worker will not reach it soon; a pass in
 * progress on the victim owns its own batch and is never touched.  The
 * read lock on kwq_steal_rm, held from the victim's lock to the append,
 * is what a drain waits for, so no batch is between two lists while a
 * drain sweeps.  Called with no lock held; true if a batch was moved.
 */
static bool
kwq_steal(struct kwq_worker *kw)
{
	struct rm_priotracker tr;
	struct kwq *q;
	struct kwq_cpu *vkc, *kc;
	struct kwq_item *head, *tail;
	u_int i, reach, n, take, k;
	int vcpu;
	bool moved = false;

	reach = kwq_topo_reach(kw->kw_cpu);
	if (reach == 0)
		return (false);
	kw->kw_steal_tries++;
	rm_rlock(&kwq_steal_rm, &tr);
	for (i = 0; i < reach && !moved; i++) {
		vcpu = kwq_topo_order[kw->kw_cpu][i];
		LIST_FOREACH(q, &kwq_steal_list, kwq_steal) {
			vkc = q->kwq_pcpu[vcpu];
			n = vkc->kc_depth;		/* unlocked: a hint */
			if (n == 0 || (n == 1 && !kwq_steal_lone_ok(vkc)))
				continue;
			kwq_kc_lock(vkc);
			n = vkc->kc_depth;
			if (n == 0 || !q->kwq_active) {
				kwq_kc_unlock(vkc);
				continue;
			}
			take = n == 1 ? 1 : (n + 1) / 2;
			if (take > kwq_steal_max)
				take = kwq_steal_max;
			head = STAILQ_FIRST(&vkc->kc_list);
			tail = head;
			for (k = 1; k < take; k++)
				tail = tail->kwi_link.stqe_next;
			KASSERT(KWQ_TAGBIT(tail->kwi_link.stqe_next) == 0,
			    ("kwq_steal: %s: a notifier on a stealable list",
			    q->kwq_name));
			vkc->kc_list.stqh_first = tail->kwi_link.stqe_next;
			if (vkc->kc_list.stqh_first == NULL)
				vkc->kc_list.stqh_last = &vkc->kc_list.stqh_first;
			tail->kwi_link.stqe_next = NULL;
			vkc->kc_depth -= take;
			vkc->kc_steals_out++;
			vkc->kc_stolen_out += take;
			kwq_list_check(vkc, "steal victim");
			kwq_kc_unlock(vkc);

			kc = q->kwq_pcpu[kw->kw_cpu];
			kwq_kc_lock(kc);
			kwq_list_concat(kc, head, tail);
			kc->kc_depth += take;
			kc->kc_steals_in++;
			kc->kc_stolen_in += take;
			kwq_list_check(kc, "steal stealer");
			(void)kwq_kc_place(kc, take);	/* our own doorbell: places it */
			kwq_kc_unlock(kc);
			SDT_PROBE4(kwq, , , steal, q, vcpu, kw->kw_cpu, (int)take);
			kw->kw_steals++;
			kw->kw_stolen += take;
			moved = true;
			break;
		}
	}
	rm_runlock(&kwq_steal_rm, &tr);
	return (moved);
}

/* Does a BULK worker near `cpu' sleep (the idle mask, a hint)? */
bool
kwq_steal_idle_near(int cpu)
{
	u_int i, reach;

	if (kwq_bulk_place == KWQ_PLACE_SHARD)
		return (kwq_shard_idle_near(cpu));
	if (kwq_bulk_place == KWQ_PLACE_GOSSIP)
		return (kwq_gossip_idle_near(cpu));	/* the pass cap's idle core */
	reach = kwq_topo_reach(cpu);
	for (i = 0; i < reach; i++)
		if (CPU_ISSET(kwq_topo_order[cpu][i], &kwq_bulk_idle))
			return (true);
	return (false);
}

/*
 * The steal wake (../SCHED.md S16): an enqueue left a stealable list on
 * `cpu' deeper than steal_wake (and at each doubling after that), so its
 * worker is behind; wake the nearest sleeping BULK worker to steal.  The
 * idle mask is a hint; kw_sleeping under kw_mtx decides.  No lock held.
 */
void
kwq_steal_wake(int cpu, struct kwq_cpu *kc)
{
	struct kwq_worker *kw;
	u_int i, reach;
	int c;

	if (kwq_bulk_place == KWQ_PLACE_SHARD) {
		(void)kwq_shard_wake(cpu, kc);	/* the group lines, not the mask */
		return;
	}
	if (kwq_bulk_place == KWQ_PLACE_GOSSIP)
		return;		/* the hand-off replaces stealing (D2) */
	reach = kwq_topo_reach(cpu);
	for (i = 0; i < reach; i++) {
		c = kwq_topo_order[cpu][i];
		if (!CPU_ISSET(c, &kwq_bulk_idle))
			continue;
		kw = kwq_workers[KWQ_BULK][c];
		mtx_lock_spin(&kw->kw_mtx);
		if (kw->kw_sleeping && !kw->kw_exit) {
			kw->kw_sleeping = false;
			kw->kw_steal_wakes++;
			wakeup_one(kw->kw_chan);
			mtx_unlock_spin(&kw->kw_mtx);
			return;
		}
		mtx_unlock_spin(&kw->kw_mtx);
	}
}

static void
kwq_worker_main(void *arg)
{
	struct kwq_worker *kw = arg;
	struct kwq_cpu *kc;
	enum ks_action a;
	enum ks_yield y;
	sbintime_t t0, t1, tick;
	uint64_t rate;
	uint64_t wall;
	int rv;
	bool tried = false;	/* a steal was attempted since the last sleep or pass */

#ifdef SMP
	/* Before sched_bind() to a CPU, wait for all CPUs to go on-line. */
	while (!smp_started)
		tsleep(kw, 0, "kwqsmp", hz / 4);
#endif
	thread_lock(curthread);
	sched_prio(curthread, kw->kw_pri);
	sched_bind(curthread, kw->kw_cpu);
	kw->kw_pri_class = curthread->td_pri_class;	/* the pass assertion */
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
			/*
			 * Nothing of its own: a BULK worker tries to steal
			 * once before it sleeps (../SCHED.md S16).  The lock
			 * is dropped for the attempt, so ks_next() runs again
			 * after it, whether it moved a batch or not: a
			 * doorbell rung meanwhile found the worker awake and
			 * placed its list without a wake.
			 */
			if (!tried && kw->kw_class == KWQ_BULK &&
			    kwq_nstealable != 0 && kwq_steal_on &&
			    kwq_bulk_place != KWQ_PLACE_GOSSIP) {
				int mcpu = kw->kw_mb_cpu;	/* the mailbox, emptied */
				uint64_t mword = kw->kw_mb_word;

				kw->kw_mb_cpu = -1;
				kw->kw_mb_word = 0;
				mtx_unlock_spin(&kw->kw_mtx);
				tried = true;
				if (kwq_bulk_place == KWQ_PLACE_SHARD)
					(void)kwq_steal_donor(kw, -1, mcpu, mword);
				else
					(void)kwq_steal(kw);
				continue;
			}
			tried = false;
			/*
			 * Sleep at the class priority: a worker that handed
			 * the CPU over and then ran out of work would
			 * otherwise wake at the waiting priority and reach
			 * its first pass only when the CPU idles or the
			 * deadline raises it.  The priority of a thread that
			 * is about to sleep interests nobody else.
			 */
			kwq_class_prio_restore(kw);
			/*
			 * The tick regime (../KWQ.md S16): leave it after a
			 * tick whose round served one element or none, and
			 * mark the episode coalesced after one that served
			 * more.  kw_tick_round is set only when the last wake
			 * was the timeout, so the round that entered the
			 * regime, which a doorbell woke, is judged neither
			 * way.  A tick_us of 0 (the knob turned off
			 * meanwhile) ends it too.
			 */
			tick = (sbintime_t)kwq_tick_us[kw->kw_class] * SBT_1US;
			if (__predict_false(kw->kw_tick) && kw->kw_tick_round &&
			    kw->kw_tick_items > 1)
				kw->kw_tick_coalesced = true;	/* this tick paid */
			if (__predict_false(kw->kw_tick) && (tick == 0 ||
			    (kw->kw_tick_round && kw->kw_tick_items <= 1)))
				kwq_tick_exit(kw, tick, sbinuptime());
			kw->kw_sleeping = true;
			if (kw->kw_class == KWQ_BULK) {	/* the steal wake's hint */
				(void)kwq_donor_point(kw, NULL, 0);	/* no longer a donor */
				kwq_idle_set(kw);
			}
			SDT_PROBE3(kwq, , , idle, kw->kw_class, kw->kw_cpu,
			    kw->kw_busy_ns - kw->kw_idle_busy);
			t0 = sbinuptime();
			/*
			 * In the regime the sleep carries the bound as its
			 * timeout (precision L/8, so the timer may be
			 * coalesced with others); outside it the sleep is
			 * untimed and only a doorbell ends it.  No callout of
			 * kwq's own: the sleep's timeout is the tick.
			 */
			if (__predict_false(kw->kw_tick)) {
				rv = msleep_spin_sbt(kw->kw_chan, &kw->kw_mtx,
				    "kwqtick", tick, tick >> 3, 0);
			} else {
				msleep_spin(kw->kw_chan, &kw->kw_mtx, "kwqidle", 0);
				rv = 0;
			}
			t1 = sbinuptime();
			if (kw->kw_class == KWQ_BULK) {
				kwq_idle_clr(kw);
				if (__predict_false(kwq_bulk_place == KWQ_PLACE_GOSSIP)) {
					if (kw->kw_pub_armed) {
						callout_stop(&kw->kw_pub_co);
						kw->kw_pub_armed = false;
					}
					kwq_gossip_point(kw, NULL, 0);	/* awake: its load */
				}
			}
			kw->kw_idle_ns += kwq_sbt2ns(t1 - t0);
			kw->kw_idle_busy = kw->kw_busy_ns;
			/*
			 * Got the CPU in the idle class before anyone
			 * promoted us (KWQ.md S16): the producer finished its
			 * burst or its CPU went idle, which is the case the
			 * wait exists for.
			 */
			if (__predict_false(kw->kw_lowpri))
				kwq_wake_promote(kw, KWQ_WAKE_SELF);
			if (__predict_false(kw->kw_turg_armed)) {
				callout_stop(&kw->kw_turg_co);
				kw->kw_turg_armed = false;
			}
			kw->kw_tick_round = (rv == EWOULDBLOCK);
			if (rv == EWOULDBLOCK) {
				/* The tick: nobody woke us, we wake ourselves. */
				kw->kw_sleeping = false;
				kw->kw_ticks++;
				SDT_PROBE3(kwq, , , tick, kw->kw_class, kw->kw_cpu,
				    kw->kw_tick_items);
			} else {
				kw->kw_wakeups++;
				kwq_tick_entry(kw, tick, t1 - t0, t1);
			}
			kw->kw_tick_items = 0;
			mtx_unlock_spin(&kw->kw_mtx);
			continue;
		}
		tried = false;
		mtx_unlock_spin(&kw->kw_mtx);

		switch (a) {
		case KS_SERVE:
			kwq_pass(kw, kc);
			/* P8: the early pull, below pull_us at a pass's end */
			if (__predict_false(kwq_bulk_place == KWQ_PLACE_SHARD) &&
			    kw->kw_class == KWQ_BULK)
				kwq_shard_pull(kw);
			/* gossip: the hand-off and a due merge at a pass's end */
			else if (__predict_false(kwq_bulk_place == KWQ_PLACE_GOSSIP) &&
			    kw->kw_class == KWQ_BULK) {
				kwq_gossip_handoff(kw);
				kwq_gossip_merge(kw->kw_cpu);
			}
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
				kwq_yield(kw, KWQ_YIELD_ROUND,
				    kwq_yield_prio[kw->kw_class]);
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
	KW_U64(kw_cap_yields, "cap_yields", "hand-overs at pass end under the share limit");
	KW_U64(kw_cap_cuts, "cap_cuts", "budget calls answered 0 by the share limit, cutting a pass short at the next item");
	KW_U64(kw_cap_restores, "cap_restores", "period deadlines that raised the worker back to its class priority");
	KW_U64(kw_cap_ns, "cap_ns", "wall time given away under the share limit");
	KW_U64(kw_cap_runon, "cap_runon", "passes ended over the allowance with nothing else runnable (the limit did not bind)");
	KW_U64(kw_slice_yields, "slice_yields", "yields at ULE's own priority because ULE ended the worker's slice (limit off only)");
	KW_U64(kw_handbacks, "handbacks", "rounds ended early because a higher class waited");
	KW_U64(kw_switched, "switched", "passes during which the worker left the CPU (preempted or blocked); NET charges them wall time");
	KW_U64(kw_ticks, "ticks", "wakes by the tick regime's own timeout (KWQ.md S16)");
	KW_U64(kw_ticks_empty, "ticks_empty", "ticks that found nothing (the regime ends)");
	KW_U64(kw_ticks_single, "ticks_single", "ticks whose round served one element (the regime ends)");
	KW_U64(kw_tick_enters, "tick_enters", "entries into the tick regime (three consecutive sleeps shorter than tick_us)");
	KW_U64(kw_tick_exits, "tick_exits", "times the regime ended");
	KW_U64(kw_tick_ns, "tick_ns", "wall time spent in the tick regime");
	KW_U64(kw_tick_barren, "tick_barren", "episodes in which no tick coalesced anything (each doubles the entry hold-off, 2 x tick_us up to 1024 x)");
	KW_U64(kw_wakes_low, "wakes_low", "wakes in the idle class for a producer on the worker's own CPU (KWQ.md S16)");
	KW_U64(kw_wake_self, "wake_self", "of those, the worker got the CPU and promoted itself");
	KW_U64(kw_wake_prom_deadline, "wake_promotions_deadline", "of those, promoted by the wake_delay_us deadline");
	KW_U64(kw_wake_prom_urgent, "wake_promotions_urgent", "of those, promoted by an urgent ring or nudge (a notifier, a drain, a cancel, kwq_enqueue_urgent()), an enqueue from another CPU, the share limit's deadline or the unload");
	KW_U64(kw_wakes_urgent, "wakes_urgent", "wakes of a sleeping worker by an urgent item added to a list already placed with the core (kwq_enqueue_urgent(), a notifier): the wake neither regime would have given it");
	KW_U64(kw_wake_low_ns, "wake_low_ns", "wall time spent waiting in the idle class");
	KW_U64(kw_turg_deferred, "tick_urgent_deferred", "urgent items that found the worker asleep in the tick regime and waited tick_urgent_us instead of waking it");
	KW_U64(kw_turg_wakes, "tick_urgent_wakes", "wakes by the tick_urgent_us bound (one per window of deferred urgent items)");
	KW_U64(kw_prio_violations, "prio_violations", "passes that began below the class priority or outside its class (0; counted under KWQ_SOFT_INVARIANTS, asserted otherwise)");
	KW_U64(kw_busy_ns, "busy_ns", "time charged to passes: worker CPU time, NET wall time");
	KW_U64(kw_idle_ns, "idle_ns", "wall time asleep with nothing queued");
	KW_U64(kw_round, "round", "current round number");
	KW_U64(kw_budget_calls, "budget_calls", "kwq_budget_left() calls by handlers on this worker");
	KW_U64(kw_budget_reads, "budget_reads", "of those, the ones that read the clock (SCHED.md S4.4)");
	if (kw->kw_class == KWQ_BULK) {
		KW_U64(kw_steal_tries, "steal_tries", "steal attempts with nothing of its own to do (SCHED.md S16)");
		KW_U64(kw_steals, "steals", "of those, the ones that moved a batch to this CPU");
		KW_U64(kw_stolen, "stolen", "items in those batches");
		KW_U64(kw_steal_wakes, "steal_wakes", "wakes from sleep sent by producers so that this worker steals (steal_wake)");
		KW_U64(kw_pulls, "pulls", "place=2: batches pulled from a donor at a pass end, the own backlog below pull_us");
		KW_U64(kw_donor_n, "donor_flags", "place=2: times this CPU became a donor (a stealable backlog of donor_us)");
		KW_U64(kw_starved_n, "starved", "place=2: times the starved tick found this worker without a decision point and flagged it");
		KW_U64(kw_handoffs, "handoffs", "place=1: batches this worker handed off at a pass end (SPEC-p6g.md S2.7)");
		KW_U64(kw_directed, "directed", "place=2, shard.directed: batches taken from the victim the waking wake named");
	}
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
			callout_init(&kw->kw_wake_co, 1);
			callout_init(&kw->kw_turg_co, 1);
			callout_init(&kw->kw_watch_co, 1);
			callout_init(&kw->kw_pub_co, 1);
			kw->kw_mb_cpu = -1;
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
			/* An unload waits for no regime (KWQ.md S16). */
			if (kw->kw_lowpri)
				kwq_wake_promote(kw, KWQ_WAKE_URGENT);
			if (kw->kw_sleeping) {
				kw->kw_sleeping = false;
				wakeup_one(kw->kw_chan);
			}
			while (kw->kw_td != NULL)
				msleep_spin(&kw->kw_td, &kw->kw_mtx, "kwqexit",
				    0);
			mtx_unlock_spin(&kw->kw_mtx);
			callout_drain(&kw->kw_cap_co);
			callout_drain(&kw->kw_wake_co);
			callout_drain(&kw->kw_turg_co);
			callout_drain(&kw->kw_watch_co);
			callout_drain(&kw->kw_pub_co);
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
