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
	kw->kw_round = 0;
	kw->kw_phase = KWQ_PH_IDLE;
	kw->kw_ring_left = 0;
	kw->kw_tail_left = -1;
	kw->kw_want_new = true;
	kw->kw_served = 0;
	kw->kw_cur = NULL;
	kw->kw_pass_start = 0;
	kw->kw_pass_budget = 0;
	kw->kw_win_start = wall_now;
	kw->kw_win_busy = 0;
	kw->kw_knobs = k;
	kw->kw_rounds = kw->kw_passes = kw->kw_yields = 0;
	kw->kw_cap_sleeps = kw->kw_handbacks = kw->kw_busy_ns = 0;
}

void
ks_queue_init(struct kwq_cpu *kc)
{
	kc->kc_state = KWQ_CPU_IDLE;
	kc->kc_onlist = KWQ_ON_NONE;
	kc->kc_warm = 0;
	kc->kc_idle_round = 0;
	kc->kc_src = KWQ_SRC_NONE;
	kc->kc_deficit = 0;
	kc->kc_passes = kc->kc_overruns = kc->kc_parks = 0;
	kc->kc_boosts = kc->kc_grace = 0;
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
 * S4.1.  A warm queue (left the ring within the grace window) goes back
 * to the ring with its deficit as it is (0 after idle); anything else is
 * boosted through the new list.  Only the low 32 bits of the round
 * counter are kept per queue (S13): the unsigned difference is what
 * matters.
 */
bool
ks_doorbell(struct kwq_worker *kw, struct kwq_cpu *kc)
{
	bool boost;

	if (kc->kc_warm &&
	    (uint32_t)kw->kw_round - kc->kc_idle_round <=
	    kw->kw_knobs->grace_rounds) {
		ks_ring_append(kw, kc);
		kc->kc_grace++;
		boost = false;
	} else {
		TAILQ_INSERT_TAIL(&kw->kw_new, kc, kc_active);
		kc->kc_onlist = KWQ_ON_NEW;
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
	TAILQ_REMOVE(&kw->kw_new, kc, kc_active);
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
	int m;

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
				continue;
			}
			kc->kc_src = KWQ_SRC_RING;
			kc->kc_state = KWQ_CPU_RUNNING;	/* handed to the worker: I2 */
			kw->kw_cur = kc;
			kw->kw_served++;
			*kcp = kc;
			return (KS_SERVE);
		}
		/* Ring done: the new entries present now, then the round ends. */
		if (kw->kw_tail_left < 0) {
			m = 0;
			TAILQ_FOREACH(kc, &kw->kw_new, kc_active)
				m++;
			kw->kw_tail_left = m;
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

void
ks_pass_begin(struct kwq_worker *kw, struct kwq_cpu *kc, uint64_t cpu_now)
{
	kc->kc_state = KWQ_CPU_RUNNING;		/* already so since ks_next() */
	kw->kw_cur = kc;
	kw->kw_pass_start = cpu_now;
	kw->kw_pass_budget = kc->kc_deficit;
}

uint64_t
ks_budget_left(const struct kwq_worker *kw, uint64_t cpu_now)
{
	int64_t left;

	left = kw->kw_pass_budget - (int64_t)(cpu_now - kw->kw_pass_start);
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
	kc->kc_passes++;
	kw->kw_passes++;
	kw->kw_busy_ns += (uint64_t)dt;
	kw->kw_win_busy += (uint64_t)dt;
	/*
	 * An overrun is a pass that exceeded its budget by more than one
	 * quantum: a cooperative handler that stops after the item which
	 * exhausts the budget overshoots by one item, which is not one.
	 */
	if (dt > kw->kw_pass_budget + q)
		kc->kc_overruns++;
	kc->kc_deficit -= dt;
	if (kc->kc_deficit < -(int64_t)kw->kw_knobs->penalty_rounds * q)
		kc->kc_deficit = -(int64_t)kw->kw_knobs->penalty_rounds * q;
	kw->kw_cur = NULL;
	if (has_work) {
		kc->kc_state = kc->kc_deficit > 0 ? KWQ_CPU_WAKING : KWQ_CPU_PARKED;
		ks_ring_append(kw, kc);
	} else {
		kc->kc_state = KWQ_CPU_IDLE;
		kc->kc_warm = (kc->kc_src == KWQ_SRC_RING);
		kc->kc_idle_round = (uint32_t)kw->kw_round;
		kc->kc_deficit = 0;
	}
	kc->kc_src = KWQ_SRC_NONE;
}

/*
 * S6.1 and S6.3.  The yield is skipped when nothing else is runnable.  With
 * the cap on, a window that ended over the cap with something runnable
 * turns the yield into a pause.
 */
enum ks_yield
ks_round_end(struct kwq_worker *kw, uint64_t wall_now, bool runnable)
{
	const struct kwq_sched_knobs *k = kw->kw_knobs;
	uint64_t win;

	kw->kw_rounds++;
	if (k->cap_pct < 100) {
		win = wall_now - kw->kw_win_start;
		if (win >= k->cap_window_ns) {
			bool over = kw->kw_win_busy * 100 > win * k->cap_pct;

			kw->kw_win_start = wall_now;
			kw->kw_win_busy = 0;
			if (over && runnable) {
				kw->kw_cap_sleeps++;
				return (KS_Y_PAUSE);
			}
		}
	}
	if (!runnable)
		return (KS_Y_NONE);
	kw->kw_yields++;
	return (KS_Y_YIELD);
}
