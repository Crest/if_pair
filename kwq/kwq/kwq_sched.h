/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq scheduler core: ../SCHED.md S4 (algorithm) and S6 (yielding) as
 * pure functions over struct kwq_worker and struct kwq_cpu.
 *
 * This file and kwq_sched.c make no kernel calls: no locks, no clocks,
 * no threads.  The caller (kwq_worker.c in the kernel, sim/sim.c in
 * userspace) holds the locks the fields need, reads the clocks, and acts
 * on the decisions returned.  Both worlds provide <kwq_sched_env.h>,
 * which declares struct kwq_worker and struct kwq_cpu with the fields
 * listed below and the KSQ_WEIGHT() accessor.
 *
 * Fields the core uses (names are those of kwq_internal.h):
 *
 *   struct kwq_cpu:    kc_active (TAILQ_ENTRY), kc_state, kc_onlist,
 *                      kc_warm, kc_idle_round (u_int), kc_src, kc_deficit
 *                      (int64_t ns), kc_passes, kc_overruns, kc_parks, kc_glitches,
 *                      kc_boosts, kc_grace
 *   struct kwq_worker: kw_new, kw_active (TAILQ_HEAD of kwq_cpu),
 *                      kw_nactive, kw_nnew (u_int list lengths),
 *                      kw_round (uint64_t), kw_phase,
 *                      kw_ring_left, kw_tail_left, kw_want_new, kw_cur,
 *                      kw_served, kw_pass_start, kw_pass_budget,
 *                      kw_win_start, kw_win_busy, kw_knobs, kw_rounds, kw_passes,
 *                      kw_yields, kw_cap_sleeps, kw_handbacks, kw_busy_ns
 *
 * Units: all times are nanoseconds.  "cpu" times are the worker's own CPU
 * time (S1a.4); "wall" times are a monotonic clock (sbinuptime in the
 * kernel).  Only differences of either are ever taken.
 */

#ifndef _KWQ_SCHED_H_
#define	_KWQ_SCHED_H_

#include <kwq_sched_env.h>

/*
 * Consistency checks inside the core.  The environment defines KS_ASSERT
 * (kernel: KASSERT, so INVARIANTS builds check; simulator: a violation).
 */
#ifndef KS_ASSERT
#define	KS_ASSERT(e, msg)	do { } while (0)
#endif

/* Per-class knobs the core reads; the owner keeps them current. */
struct kwq_sched_knobs {
	uint64_t	quantum_ns;	/* Q, validated 10 us .. 1 s */
	uint32_t	grace_rounds;	/* GRACE_ROUNDS, 1 */
	uint32_t	cap_pct;	/* 100 or more = cap off */
	uint64_t	cap_window_ns;
	uint64_t	cap_sleep_ns;
	bool		boost_weighted;	/* boost = Q x w (default) or Q; S12 */
	uint32_t	penalty_rounds;	/* debt clamp: D >= -penalty_rounds x Qw; 32 */
};

/* kc_state */
enum kwq_cpu_state {
	KWQ_CPU_IDLE = 0,
	KWQ_CPU_WAKING,
	KWQ_CPU_RUNNING,
	KWQ_CPU_PARKED,
};

/* kc_onlist */
enum kwq_onlist {
	KWQ_ON_NONE = 0,
	KWQ_ON_NEW,
	KWQ_ON_ACTIVE,
};

/* kc_src: which list the pass in progress came from */
enum kwq_src {
	KWQ_SRC_NONE = 0,
	KWQ_SRC_NEW,
	KWQ_SRC_RING,
};

/* kw_phase */
enum kwq_phase {
	KWQ_PH_IDLE = 0,	/* between rounds */
	KWQ_PH_ROUND,		/* inside a round */
};

/* What ks_next() tells the worker to do. */
enum ks_action {
	KS_IDLE,		/* nothing queued: sleep */
	KS_SERVE,		/* run a pass on *kcp; budget set */
	KS_ROUND_END,		/* the round is complete: ks_round_end() */
	KS_HANDBACK,		/* a higher class waits: ks_round_end() now */
};

/* What ks_round_end() tells the worker to do. */
enum ks_yield {
	KS_Y_NONE,		/* nothing else runnable: continue */
	KS_Y_YIELD,		/* kern_yield(yield_prio), re-assert priority */
	KS_Y_PAUSE,		/* pause_sbt(cap_sleep) instead of the yield */
};

void	ks_worker_init(struct kwq_worker *kw, const struct kwq_sched_knobs *k,
	    uint64_t wall_now);
void	ks_queue_init(struct kwq_cpu *kc);

/*
 * Producer side (S4.1), caller holds the worker lock: place a (queue, CPU)
 * that just went from empty to non-empty.  Returns true if it went to the
 * new list (a boost), false if the grace rule sent it to the ring.
 */
bool	ks_doorbell(struct kwq_worker *kw, struct kwq_cpu *kc);

/*
 * Worker side (S4.2), caller holds the worker lock: what next.  On
 * KS_SERVE, *kcp is the queue to pass over, removed from its list, and
 * its deficit is its budget.  `higher_waiting' is the hand-back input
 * (S6.4).
 */
enum ks_action ks_next(struct kwq_worker *kw, bool higher_waiting,
	    struct kwq_cpu **kcp);

/* Pass bracketing (S4.3, S4.4). */
void	ks_pass_begin(struct kwq_worker *kw, struct kwq_cpu *kc,
	    uint64_t cpu_now);
uint64_t ks_budget_left(const struct kwq_worker *kw, uint64_t cpu_now);
/*
 * Charge the pass and re-place the queue; caller holds the queue lock and
 * the worker lock.  `has_work' is the emptiness re-check under the queue
 * lock.
 */
void	ks_pass_end(struct kwq_worker *kw, struct kwq_cpu *kc,
	    uint64_t cpu_now, bool has_work);

/* End of round (S6.1, S6.3). `runnable' is sched_runnable(). */
enum ks_yield ks_round_end(struct kwq_worker *kw, uint64_t wall_now,
	    bool runnable);

/* Per-queue refill amount, Q x w. */
static inline int64_t
ks_quantum(const struct kwq_worker *kw, const struct kwq_cpu *kc)
{
	return ((int64_t)kw->kw_knobs->quantum_ns * (int64_t)KSQ_WEIGHT(kc));
}

/*
 * Tick guard (S6.1): has a hardclock tick passed since the last voluntary
 * switch?  Unsigned difference, correct across the wrap of `ticks'.
 */
static inline bool
ks_ticked(unsigned int ticks_now, unsigned int swvoltick)
{
	return (ticks_now - swvoltick >= 1);
}

/*
 * Fixed-point tick-to-nanosecond conversion (S13): scale = ns per tick in
 * 32.32.  Exact for any t: the product is split so nothing overflows
 * 64 bits (hi x scale < 2^32 x 2^32).
 */
/*
 * Clock glitch guard (SCHED.md S13): a pass whose CPU-time delta is
 * negative or longer than this is a ticker fault, not work.  cpu_ticks()
 * on Ampere Altra reads a few ticks backwards now and then and
 * tc_cpu_ticks() takes that for a 32-bit wrap (+2^32 ticks = 171.8 s at
 * 25 MHz).  Such a pass is charged one quantum and counted.
 */
#define	KS_GLITCH_NS	1000000000ULL

static inline uint64_t
ks_ns_scale(uint64_t rate_hz)
{
	if (rate_hz == 0)
		rate_hz = 1000000000ULL;	/* before calibration: 1 GHz */
	return ((1000000000ULL << 32) / rate_hz);
}

static inline uint64_t
ks_ticks2ns(uint64_t t, uint64_t scale)
{
	uint64_t hi = t >> 32, lo = t & 0xffffffffULL;

	return (hi * scale + ((lo * scale) >> 32));
}

#ifdef _KERNEL
extern struct kwq_sched_knobs kwq_knobs[KWQ_NCLASS];	/* kwq.c, from sysctl */
#endif

#endif /* !_KWQ_SCHED_H_ */
