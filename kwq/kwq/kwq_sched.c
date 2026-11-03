/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq scheduler core: deficit round robin in CPU time with a new list
 * served in alternation, a grace rule, symmetric deficit clamps, parking
 * and the yield decision.  Normative text: ../SCHED.md S4, S6, S13.
 *
 * No kernel calls.  See kwq_sched.h for the contract and the fields used.
 */

#include "kwq_sched.h"

void
ks_worker_init(struct kwq_worker *kw, const struct kwq_sched_knobs *k,
    uint64_t wall_now)
{
	TAILQ_INIT(&kw->kw_new);
	TAILQ_INIT(&kw->kw_active);
	kw->kw_nactive = 0;
	kw->kw_nnew = 0;
	kw->kw_round = 0;
	kw->kw_served_us = 0;
	kw->kw_served_rem = 0;
	kw->kw_phase = KWQ_PH_IDLE;
	kw->kw_ring_left = 0;
	kw->kw_tail_left = -1;
	kw->kw_want_new = true;
	kw->kw_served = 0;
	kw->kw_cur = NULL;
	kw->kw_pass_start = 0;
	kw->kw_pass_budget = 0;
	kw->kw_cap_start = wall_now;
	kw->kw_cap_used = 0;
	kw->kw_cap_over = false;
	kw->kw_cap_handed = false;
	kw->kw_knobs = k;
	kw->kw_bc_calls = 0;
	kw->kw_bc_last = 0;
	kw->kw_budget_calls = kw->kw_budget_reads = 0;
	kw->kw_rounds = kw->kw_passes = kw->kw_yields = 0;
	kw->kw_cap_demotions = kw->kw_cap_yields = kw->kw_cap_runon = 0;
	kw->kw_cap_cuts = 0;
	kw->kw_handbacks = kw->kw_busy_ns = 0;
}

void
ks_queue_init(struct kwq_cpu *kc)
{
	kc->kc_state = KWQ_CPU_IDLE;
	kc->kc_onlist = KWQ_ON_NONE;
	kc->kc_warm = 0;
	kc->kc_idle_served = 0;
	kc->kc_src = KWQ_SRC_NONE;
	kc->kc_deficit = 0;
	kc->kc_passes = kc->kc_overruns = kc->kc_parks = 0;
	kc->kc_boosts = kc->kc_grace = kc->kc_debts = kc->kc_glitches = 0;
	kc->kc_bc_avg = 0;
}

/* Put kc on the ring tail. */
static void
ks_ring_append(struct kwq_worker *kw, struct kwq_cpu *kc)
{
	TAILQ_INSERT_TAIL(&kw->kw_active, kc, kc_active);
	kc->kc_onlist = KWQ_ON_ACTIVE;
	kw->kw_nactive++;
}

/*
 * S4.1.  A warm queue (left the ring less than one ring's worth of
 * service ago) goes back to the ring with its deficit as it is (0 after
 * idle); anything else is boosted through the new list.  The window is
 * service, not rounds: the share limit's budget cut (S6.3) ends passes
 * early, so a cut round delivers a fraction of a quantum per competitor
 * and rounds come faster than service does; counting rounds then lets a
 * burst-and-pause producer's unchanged pause span more than one round
 * and be boosted on a burst the rule meant to deny (SCHED.md S4.6, which
 * specifies the unit on that argument: the guest's gaming case of
 * 2026-10-28 did not reproduce it reliably).  Only the low 32 bits of
 * the service counter are kept per queue (S13): the unsigned difference
 * is what matters.
 */
bool
ks_doorbell(struct kwq_worker *kw, struct kwq_cpu *kc)
{
	bool boost;

	if (kc->kc_warm & KWQ_DEBT) {
		/* Owes time (S4.3): to the ring, where the refill parks it. */
		ks_ring_append(kw, kc);
		kc->kc_debts++;
		boost = false;
	} else if ((kc->kc_warm & KWQ_WARM) &&
	    (uint32_t)kw->kw_served_us - kc->kc_idle_served <=
	    ks_grace_service_us(kw)) {
		ks_ring_append(kw, kc);
		kc->kc_grace++;
		boost = false;
	} else {
		TAILQ_INSERT_TAIL(&kw->kw_new, kc, kc_active);
		kc->kc_onlist = KWQ_ON_NEW;
		kw->kw_nnew++;
		boost = true;
	}
	kc->kc_state = KWQ_CPU_WAKING;
	return (boost);
}

/* Pop the head of the new list and give it a fresh quantum. */
static struct kwq_cpu *
ks_take_new(struct kwq_worker *kw)
{
	struct kwq_cpu *kc;

	kc = TAILQ_FIRST(&kw->kw_new);
	KS_ASSERT(kc != NULL && kw->kw_nnew > 0,
	    ("kwq: new list %p empty but kw_nnew %u", kw, kw->kw_nnew));
	TAILQ_REMOVE(&kw->kw_new, kc, kc_active);
	kw->kw_nnew--;
	kc->kc_onlist = KWQ_ON_NONE;
	kc->kc_warm = 0;
	kc->kc_deficit = kw->kw_knobs->boost_weighted ? ks_quantum(kw, kc) :
	    (int64_t)kw->kw_knobs->quantum_ns;
	kc->kc_boosts++;
	kc->kc_src = KWQ_SRC_NEW;
	kc->kc_state = KWQ_CPU_RUNNING;		/* handed to the worker: I2 */
	kw->kw_cur = kc;
	return (kc);
}

/*
 * S4.2 as a resumable state machine: each call returns the next thing to
 * do.  Slot order within a round: [new] then, for each ring entry present
 * at round start, [ring] [new]; then the new entries present when the
 * ring is done; then the round ends.  A higher class waiting ends the
 * round at the next slot boundary.
 */
enum ks_action
ks_next(struct kwq_worker *kw, bool higher_waiting, struct kwq_cpu **kcp)
{
	struct kwq_cpu *kc;
	int64_t q;
	int m __diagused;	/* the checking count below */

	*kcp = NULL;
	if (kw->kw_phase == KWQ_PH_IDLE) {
		if (TAILQ_EMPTY(&kw->kw_new) && TAILQ_EMPTY(&kw->kw_active))
			return (KS_IDLE);
		kw->kw_round++;
		kw->kw_phase = KWQ_PH_ROUND;
		kw->kw_ring_left = (int)kw->kw_nactive;
		kw->kw_tail_left = -1;
		kw->kw_want_new = true;
		kw->kw_served = 0;
	}
	for (;;) {
		/*
		 * S6.4: hand back to a waiting higher class, but only after this
		 * round has served at least one pass - a yield that gave the lower
		 * class nothing would starve it entirely under a saturated higher
		 * class.
		 */
		if (higher_waiting && kw->kw_served > 0) {
			kw->kw_handbacks++;
			kw->kw_phase = KWQ_PH_IDLE;
			return (KS_HANDBACK);
		}
		if (kw->kw_want_new && !TAILQ_EMPTY(&kw->kw_new)) {
			kw->kw_want_new = false;
			kw->kw_served++;
			*kcp = ks_take_new(kw);
			return (KS_SERVE);
		}
		if (kw->kw_ring_left > 0) {
			kw->kw_ring_left--;
			kw->kw_want_new = true;
			kc = TAILQ_FIRST(&kw->kw_active);
			if (kc == NULL) {		/* ring shrank: entries went idle */
				kw->kw_ring_left = 0;
				continue;
			}
			TAILQ_REMOVE(&kw->kw_active, kc, kc_active);
			kc->kc_onlist = KWQ_ON_NONE;
			kw->kw_nactive--;
			q = ks_quantum(kw, kc);
			kc->kc_deficit += q;
			if (kc->kc_deficit > 2 * q)	/* carry cap */
				kc->kc_deficit = 2 * q;
			if (kc->kc_deficit <= 0) {
				kc->kc_parks++;
				kc->kc_state = KWQ_CPU_PARKED;
				ks_ring_append(kw, kc);
				KS_HOOK_PARK(kw, kc);
				continue;
			}
			kc->kc_src = KWQ_SRC_RING;
			kc->kc_state = KWQ_CPU_RUNNING;	/* handed to the worker: I2 */
			kw->kw_cur = kc;
			kw->kw_served++;
			*kcp = kc;
			return (KS_SERVE);
		}
		/*
		 * Ring done: the new entries present now, then the round ends.
		 * kw_nnew is the list length, kept by ks_doorbell() and
		 * ks_take_new(); the walk that used to count here ran under the
		 * spin lock over one cold line per entry.  The checking build
		 * (INVARIANTS, or the simulator) still counts and compares.
		 */
		if (kw->kw_tail_left < 0) {
#if !defined(_KERNEL) || defined(INVARIANTS)
			m = 0;
			TAILQ_FOREACH(kc, &kw->kw_new, kc_active)
				m++;
			KS_ASSERT(m == (int)kw->kw_nnew,
			    ("kwq: new list has %d entries, kw_nnew %u", m,
			    kw->kw_nnew));
#endif
			kw->kw_tail_left = (int)kw->kw_nnew;
		}
		if (kw->kw_tail_left > 0 && !TAILQ_EMPTY(&kw->kw_new)) {
			kw->kw_tail_left--;
			kw->kw_served++;
			*kcp = ks_take_new(kw);
			return (KS_SERVE);
		}
		kw->kw_phase = KWQ_PH_IDLE;
		return (KS_ROUND_END);
	}
}

/*
 * S6.3: start a new period if the current one is over.  Nothing carries:
 * unused allowance is lost, and the overshoot of a clamped pass is one
 * item, not worth charging.  Returns true if the worker was over the
 * allowance, i.e. the deadline callout must be disarmed.
 */
static bool
ks_cap_period(struct kwq_worker *kw, uint64_t wall_now)
{
	bool was_over;

	if (wall_now - kw->kw_cap_start < kw->kw_knobs->cap_period_ns)
		return (false);
	kw->kw_cap_start = wall_now;
	kw->kw_cap_used = 0;
	was_over = kw->kw_cap_over;
	kw->kw_cap_over = false;
	return (was_over);
}

enum ks_cap
ks_pass_begin(struct kwq_worker *kw, struct kwq_cpu *kc, uint64_t cpu_now,
    uint64_t wall_now)
{
	const struct kwq_sched_knobs *k = kw->kw_knobs;
	enum ks_cap cap = KS_CAP_NONE;
	int64_t left;

	kc->kc_state = KWQ_CPU_RUNNING;		/* already so since ks_next() */
	kw->kw_cur = kc;
	kw->kw_pass_start = cpu_now;
	kw->kw_pass_budget = kc->kc_deficit;
	kw->kw_cap_handed = false;		/* a pass follows the hand-over */
	/*
	 * S6.3: the period may have ended while the worker waited (the
	 * deadline brought it back), so roll it over here, or the first
	 * pass of the period would run on the full deficit.  Under the
	 * allowance a pass may spend no more than what is left of it, so a
	 * cooperative handler stops one item over the allowance instead of
	 * one quantum over.  Over it the budget is the deficit again: with
	 * nothing else runnable the worker runs full passes (work
	 * conserving), and when something else is runnable the budget cut
	 * (ks_cap_spent() in the handler's budget query) ends the pass at
	 * the next item, so one-item passes happen only while contended.
	 */
	if (k->cap_permille < 1000) {
		if (ks_cap_period(kw, wall_now))
			cap = KS_CAP_RESTORE;
		if (kw->kw_cap_used < k->cap_allow_ns) {
			left = (int64_t)(k->cap_allow_ns - kw->kw_cap_used);
			if (kw->kw_pass_budget > left)
				kw->kw_pass_budget = left;
		}
	}
	kw->kw_bc_calls = 0;
	kw->kw_bc_last = 0;
	return (cap);
}

bool
ks_budget_need_clock(struct kwq_worker *kw, uint64_t *leftp)
{
	struct kwq_cpu *kc = kw->kw_cur;
	uint32_t k = kw->kw_knobs->budget_check_every;
	int64_t est, margin;

	kw->kw_budget_calls++;
	kw->kw_bc_calls++;
	if (k <= 1 || kw->kw_bc_calls >= k)
		return (true);
	margin = ks_quantum(kw, kc) / 10;
	est = kw->kw_bc_last + (int64_t)kw->kw_bc_calls * (int64_t)kc->kc_bc_avg;
	if (est + margin >= kw->kw_pass_budget)
		return (true);
	*leftp = (uint64_t)(kw->kw_pass_budget - est);	/* > margin */
	return (false);
}

uint64_t
ks_budget_left(struct kwq_worker *kw, uint64_t cpu_now)
{
	struct kwq_cpu *kc = kw->kw_cur;
	int64_t el = (int64_t)(cpu_now - kw->kw_pass_start), left;

	kw->kw_budget_reads++;
	if (el < 0 || el > (int64_t)KS_GLITCH_NS)	/* clock glitch: stop */
		return (0);
	if (kw->kw_bc_calls > 0) {		/* learn ns per call, S4.4 */
		uint64_t per = (uint64_t)(el - kw->kw_bc_last) / kw->kw_bc_calls;

		kc->kc_bc_avg = kc->kc_bc_avg == 0 ? per : (kc->kc_bc_avg + per) / 2;
	}
	kw->kw_bc_last = el;
	kw->kw_bc_calls = 0;
	left = kw->kw_pass_budget - el;
	return (left > 0 ? (uint64_t)left : 0);
}

/*
 * S4.3.  Charge the worker's own CPU time, clamp the debt at -2Qw (S13),
 * and either put the queue back on the ring or let it go idle with the
 * warm mark the grace rule needs.
 */
void
ks_pass_end(struct kwq_worker *kw, struct kwq_cpu *kc, uint64_t cpu_now,
    bool has_work)
{
	int64_t dt, q;

	dt = (int64_t)(cpu_now - kw->kw_pass_start);
	q = ks_quantum(kw, kc);
	if (dt < 0 || dt > (int64_t)KS_GLITCH_NS) {	/* S13: ticker glitch */
		kc->kc_glitches++;
		dt = q;
	}
	kc->kc_passes++;
	kw->kw_passes++;
	kw->kw_busy_ns += (uint64_t)dt;
	kw->kw_cap_used += (uint64_t)dt;
	/*
	 * Service delivered, S4.6, in whole microseconds with the rest
	 * carried: a NET pass of one small item is shorter than one.
	 */
	kw->kw_served_rem += (uint64_t)dt;
	kw->kw_served_us += kw->kw_served_rem / 1000;
	kw->kw_served_rem %= 1000;
	/*
	 * An overrun is a pass that exceeded its budget by more than one
	 * quantum: a cooperative handler that stops after the item which
	 * exhausts the budget overshoots by one item, which is not one.
	 */
	if (dt > kw->kw_pass_budget + q) {
		kc->kc_overruns++;
		if (kw->kw_pass_switched)	/* the system's excess, S6.3 */
			kc->kc_overruns_switched++;
		KS_HOOK_OVERRUN(kw, kc, (uint64_t)(dt - kw->kw_pass_budget));
	}
	kc->kc_deficit -= dt;
	if (kc->kc_deficit < -(int64_t)kw->kw_knobs->penalty_rounds * q)
		kc->kc_deficit = -(int64_t)kw->kw_knobs->penalty_rounds * q;
	kw->kw_cur = NULL;
	if (has_work) {
		kc->kc_state = kc->kc_deficit > 0 ? KWQ_CPU_WAKING : KWQ_CPU_PARKED;
		ks_ring_append(kw, kc);
	} else {
		kc->kc_state = KWQ_CPU_IDLE;
		kc->kc_warm = (kc->kc_src == KWQ_SRC_RING) ? KWQ_WARM : 0;
		kc->kc_idle_served = (uint32_t)kw->kw_served_us;
		if (kc->kc_deficit < 0)
			kc->kc_warm |= KWQ_DEBT;	/* debt survives idle (S4.3) */
		else
			kc->kc_deficit = 0;		/* DRR: reset on empty */
	}
	kc->kc_src = KWQ_SRC_NONE;
}

/*
 * S6.1.  The yield is skipped when nothing else is runnable, and (S6.3)
 * when the round's last pass handed the CPU over under the share limit:
 * the hand-over was this round's voluntary switch, and a yield at
 * yield_prio straight after it would switch again for nothing.
 */
enum ks_yield
ks_round_end(struct kwq_worker *kw, uint64_t wall_now __unused, bool runnable)
{
	kw->kw_rounds++;
	if (kw->kw_cap_handed) {
		kw->kw_cap_handed = false;
		return (KS_Y_NONE);
	}
	if (!runnable)
		return (KS_Y_NONE);
	kw->kw_yields++;
	return (KS_Y_YIELD);
}

/*
 * S6.3, at every pass end.  The limit is a share of each wall-time period
 * that the worker may spend in passes while anything else is runnable on
 * its CPU, the passes counted as the deficit is charged (CPU time, NET
 * wall time, S1a.4); with nothing else runnable it runs on.
 */
enum ks_cap
ks_cap_check(struct kwq_worker *kw, uint64_t wall_now, bool runnable)
{
	const struct kwq_sched_knobs *k = kw->kw_knobs;

	if (k->cap_permille >= 1000) {		/* off, or turned off just now */
		if (kw->kw_cap_over) {
			kw->kw_cap_over = false;
			return (KS_CAP_RESTORE);
		}
		return (KS_CAP_NONE);
	}
	if (ks_cap_period(kw, wall_now))
		return (KS_CAP_RESTORE);
	if (kw->kw_cap_used < k->cap_allow_ns)
		return (KS_CAP_NONE);
	if (!runnable) {
		kw->kw_cap_runon++;	/* over, but nobody else wants the CPU */
		return (KS_CAP_NONE);
	}
	kw->kw_cap_yields++;
	kw->kw_cap_handed = true;
	if (!kw->kw_cap_over) {
		kw->kw_cap_over = true;
		kw->kw_cap_demotions++;
		return (KS_CAP_DEMOTE);
	}
	return (KS_CAP_YIELD);
}
