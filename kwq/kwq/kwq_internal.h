/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq internals: struct kwq, struct kwq_cpu, struct kwq_worker.
 * Layout rules: ../KWQ.md S17.  Scheduler fields: ../SCHED.md S3, used by
 * kwq_sched.c through kwq_sched_env.h.
 */

#ifndef _KWQ_INTERNAL_H_
#define	_KWQ_INTERNAL_H_

#include <sys/param.h>
#include <sys/callout.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rmlock.h>
#include <sys/sysctl.h>
#include <sys/_cpuset.h>
#include <sys/malloc.h>
#include <sys/time.h>

#include "kwq.h"
#include "kwq_place.h"

/*
 * Granule for producer/consumer shared state.  128 rather than
 * CACHE_LINE_SIZE because amd64's spatial prefetcher pairs 64-byte lines
 * into 128-byte blocks, so two CPUs' private state in adjacent 64-byte
 * lines still bounces; arm64 is 128 anyway.
 */
#define	KWQ_LINE	(CACHE_LINE_SIZE > 128 ? CACHE_LINE_SIZE : 128)

MALLOC_DECLARE(M_KWQ);

struct kwq_sched_knobs;

/* Notifier states, protected by the notifier's (queue, CPU) mutex. */
enum kwq_nf_state {
	KWQ_NF_IDLE = 0,
	KWQ_NF_PENDING,		/* on the live list or in a batch not yet released */
	KWQ_NF_CANCELLED,	/* kwq_notify_cancel() ran; waiting to be consumed */
	KWQ_NF_DEAD,		/* cancelled and consumed: refuses signals until re-init */
	/*
	 * kn_state holds the state in its low two bits and a generation,
	 * bumped on every consume, above them.  The generation lets the
	 * worker tell a notifier the handler never consumed (same generation
	 * as at swap-out) from one it consumed and re-armed (new generation),
	 * in O(1), for the head and tail of a pass: see kwq_pass().  It wraps
	 * modulo 2^30 (unsigned arithmetic, so defined), which is harmless:
	 * the two reads it is compared between are the swap-out and the end
	 * of the same pass, and a notifier is consumed at most once per pass
	 * (at most one instance is ever pending; a re-signal joins the live
	 * list, not the batch), so the generation moves by 0 or 1 between
	 * them and a false match would need exactly 2^30 bumps.  One bit
	 * would do; the width is what the word has spare.
	 */
};

#define	KWQ_NF_ST(w)		((w) & 3)
#define	KWQ_NF_SET(w, st)	(((w) & ~3u) | (st))
#define	KWQ_NF_BUMP(w, st)	(((((w) >> 2) + 1) << 2) | (st))	/* next generation */
#define	KWQ_NF_GEN(w)		((w) >> 2)

/* kc_flags */
#define	KWQ_KC_SPIN	0x01	/* kc_mtx is MTX_SPIN */

struct kwq_cpu {
	/*
	 * Block 0 (one KWQ_LINE): shared between producers and the consumer.
	 * The per-enqueue set (mutex, list, depth, state) sits in the first
	 * 64 bytes; burst-start and rare fields follow.  kc_state's
	 * IDLE <-> non-IDLE transitions happen under kc_mtx (and kw_mtx);
	 * the worker moves it between non-IDLE values under kw_mtx alone,
	 * which producers, who only test for IDLE under kc_mtx, tolerate.
	 * kc_onlist, kc_warm and kc_idle_served are under kw_mtx.
	 */
	struct mtx		kc_mtx;
	STAILQ_HEAD(, kwq_item)	kc_list;	/* items and notifiers, arrival order */
	u_int			kc_depth;	/* elements on kc_list */
	u_int			kc_state;	/* enum kwq_cpu_state (kwq_sched.h) */
	u_int			kc_nnotify;	/* notifiers pending: on kc_list or handed over */
	u_int			kc_flags;	/* ---- 64 bytes up to here ---- */
	u_short			kc_waiters;	/* drain/cancel sleepers on kc */
	u_char			kc_onlist;	/* enum kwq_onlist */
	u_char			kc_warm;	/* KWQ_WARM | KWQ_DEBT (SCHED.md S4.3, S4.6) */
	u_int			kc_idle_served;	/* low 32 bits of kw_served_us when the
						   queue went idle from the ring (S4.6) */
	sbintime_t		kc_empty_since;	/* sbinuptime at empty -> non-empty */
	TAILQ_ENTRY(kwq_cpu)	kc_active;	/* worker's new list or ring */
	uint64_t		kc_rejected;	/* ENOBUFS/ENXIO, producer under lock */
	uint64_t		kc_coalesced;	/* kwq_notify() no-ops */
	uint64_t		kc_doorbells;	/* wakeups performed for this list */
	uint64_t		kc_doorbells_skipped;	/* lists placed while the worker's tick
						   was armed: no wakeup now, the tick or
						   tick_urgent_us serves them (KWQ.md S16) */

	/* Block 1..: consumer-private, written only by the owning worker. */
	struct kwq		*kc_q __aligned(KWQ_LINE);
	int			kc_cpu;
	u_int			kc_src;		/* enum kwq_src: list of the pass in progress */
	int64_t			kc_deficit;	/* DRR deficit, ns */
	uint64_t		kc_items;	/* elements handed to the handler, notifiers included */
	uint64_t		kc_signals;	/* of those, notifiers (counted as the handler consumes them) */
	uint64_t		kc_passes;
	uint64_t		kc_cycles;	/* wall cpu_ticks spent in passes */
	uint64_t		kc_requeued;
	uint64_t		kc_maxdepth;	/* sampled at pass start */
	uint64_t		kc_maxlat_ns;	/* oldest-item age at pass start */
	uint64_t		kc_overruns;	/* passes > budget + Qw */
	uint64_t		kc_overruns_switched;	/* of those, passes during which the
						   worker left the CPU: the system's
						   excess, not the handler's (S6.3) */
	uint64_t		kc_parks;	/* rounds skipped for a deficit <= 0 */
	uint64_t		kc_boosts;	/* passes served from the new list */
	uint64_t		kc_grace;	/* doorbells sent to the ring by the grace rule */
	uint64_t		kc_glitches;	/* passes with a negative or > KS_GLITCH_NS CPU-time delta */
	uint64_t		kc_switched;	/* NET: passes during which the worker left the CPU
					   (preempted or blocked); charged as wall time (S1a.4) */
	uint64_t		kc_debts;	/* doorbells sent to the ring because the queue owed time */
	uint64_t		kc_bc_avg;	/* ns per kwq_budget_left() call, learned (SCHED.md S4.4) */
	uint64_t		kc_steals_in;	/* batches this CPU's worker took from
						   other CPUs' lists of the queue (S16) */
	uint64_t		kc_stolen_in;	/* items in those batches */
	/*
	 * Written by other CPUs under kc_mtx: by a BULK worker that takes a
	 * batch from this list (SCHED.md S16), by kwq_scatter()'s caller.
	 * Block 0 is full, and both are rare next to the passes that write
	 * this block, so the line they cost the owner now and then is not
	 * worth a block of their own.
	 */
	uint64_t		kc_steals_out;	/* batches taken from this list */
	uint64_t		kc_stolen_out;	/* items in those batches */
	uint64_t		kc_scattered;	/* items kwq_scatter() put on this list */
	/*
	 * P8 (place=2, ../SPEC-p6g.md K9): the cost of an item on this CPU,
	 * ns, an average over passes that the owner keeps at pass end; it
	 * sizes the backlog that makes the CPU a donor and the steal bound.
	 */
	uint64_t		kc_cost_ns;
} __aligned(KWQ_LINE);

CTASSERT(__offsetof(struct kwq_cpu, kc_flags) < 64);	/* enqueue path in line 0 */
CTASSERT(__offsetof(struct kwq_cpu, kc_q) == KWQ_LINE);

/* kwq_state */
enum kwq_qstate { KWQ_ST_INACTIVE = 0, KWQ_ST_ACTIVE, KWQ_ST_DRAINING, KWQ_ST_DRAINED };

struct kwq {
	/* Read-mostly: written at create/activate only. */
	char			kwq_name[KWQ_NAMELEN];
	char			kwq_lockname[KWQ_NAMELEN + 8];
	enum kwq_class		kwq_class;
	uint32_t		kwq_flags;
	u_int			kwq_limit;
	u_int			kwq_weight;
	kwq_handler_t		*kwq_fn;
	void			*kwq_ctx;
	struct kwq_cpu		**kwq_pcpu;	/* [mp_maxid + 1], NULL for absent */
	struct vnet		*kwq_vnet;
	int			kwq_steal_slot;	/* in kwq_steal_tab, -1 if none (P8) */
	uint32_t		kwq_steal_id;	/* names it in a donor hint (P8) */

	/* Runtime-written: own block so a drain does not dirty the header. */
	volatile u_int		kwq_active __aligned(KWQ_LINE);
	u_int			kwq_drained;
	u_int			kwq_state;	/* KWQ_ST_*, for sysctl and DDB */
	LIST_ENTRY(kwq)		kwq_all;
	LIST_ENTRY(kwq)		kwq_steal;	/* on kwq_steal_list, under kwq_steal_rm */
	bool			kwq_stealing;	/* on that list */
	struct sysctl_ctx_list	kwq_sysctl;
} __aligned(KWQ_LINE);

CTASSERT(__offsetof(struct kwq, kwq_active) == KWQ_LINE);

struct kwq_worker {
	/*
	 * Block 0: shared with producers (doorbell) under kw_mtx, a spin mutex
	 * so that KWQ_F_SPIN producers may ring it.  kw_served_us and
	 * kw_nactive, which the grace rule reads at the doorbell, are written
	 * under kw_mtx too (the worker at pass end and in its ring walk, a
	 * producer's doorbell when it appends to the ring), so the producer
	 * reads them consistently (SCHED.md S3, S4.6).
	 */
	struct mtx		kw_mtx;
	TAILQ_HEAD(, kwq_cpu)	kw_new;		/* newly non-empty: boosted */
	TAILQ_HEAD(, kwq_cpu)	kw_active;	/* the DRR ring */
	u_int			kw_nactive;
	u_int			kw_nnew;	/* length of kw_new (SCHED.md S3) */
	bool			kw_sleeping;
	bool			kw_exit;
	bool			kw_lowpri;	/* made runnable in the idle class at
						   cap_prio, not yet promoted (KWQ.md S16) */
	struct thread		*kw_td;
	uint64_t		kw_round;
	uint64_t		kw_served_us;	/* microseconds of pass service delivered
						   since the worker started; advances only
						   inside passes (SCHED.md S4.6) */
	uint64_t		kw_served_rem;	/* ns of it short of the next microsecond */
	void			*kw_chan;	/* wait channel (kwq_worker.c) */
	/*
	 * The directed steal's mailbox (place=2, kern.kwq.bulk.shard.directed):
	 * whoever wakes this worker to steal names the CPU and the list (a
	 * donor hint's word), under kw_mtx, in the line the wake writes anyway.
	 */
	int			kw_mb_cpu;	/* -1: empty */
	uint64_t		kw_mb_word;

	/*
	 * Block 1..: private to the worker (ks_* run under kw_mtx anyway),
	 * except the regimes' state (kw_tick, kw_wake_*, kw_turg_* and their
	 * counters), which producers read and write under kw_mtx.
	 */
	enum kwq_class		kw_class __aligned(KWQ_LINE);
	int			kw_cpu;
	int			kw_pri;
	u_int			kw_phase;	/* enum kwq_phase */
	int			kw_ring_left;
	int			kw_tail_left;
	bool			kw_want_new;
	u_int			kw_served;
	struct kwq_cpu		*kw_cur;	/* pass in progress */
	struct kwq_item		*kw_pass_tail;	/* last item handed to the handler
						   this pass: kwq_requeue(tail NULL) */
	uint64_t		kw_pass_start;	/* worker CPU time, ns */
	int64_t			kw_pass_budget;	/* deficit at pass start, ns */
	u_int			kw_bc_calls;	/* budget calls since the last clock read (S4.4) */
	int64_t			kw_bc_last;	/* elapsed at that read, ns */
	uint64_t		kw_cap_start;	/* S6.3 period start, worker wall ns */
	uint64_t		kw_cap_used;	/* pass time charged in this period */
	bool			kw_cap_over;	/* a hand-over happened this period */
	bool			kw_cap_handed;	/* the last pass ended in a hand-over */
	volatile bool		kw_cap_fired;	/* the period's deadline callout ran */
	struct callout		kw_cap_co;	/* raises the priority back at the period's end */
	bool			kw_pass_switched;	/* the pass just ended left the CPU */
	int			kw_pri_class;	/* td_pri_class at start; the priority
						   assertion and the idle-class wait */
	/*
	 * The tick regime (KWQ.md S16, off unless kern.kwq.<class>.tick_us is
	 * set): the worker sleeps with the class's bound L as its timeout and
	 * producers place their lists without waking it.  Entered from a
	 * doorbell wake after KWQ_TICK_ENTER consecutive sleeps shorter than
	 * L, subject to the hold-off; left after a tick whose round served
	 * one element or none.
	 */
	/*
	 * The idle-class wait for same-CPU producers (KWQ.md S16, off unless
	 * kern.kwq.<class>.wake_delay_us is set): an enqueue from the
	 * worker's own CPU makes the worker runnable in the idle class at
	 * cap_prio instead of preempting the producer; the deadline below,
	 * or the worker getting the CPU by itself, promotes it back.
	 */
	struct callout		kw_wake_co;	/* the wake_delay_us deadline */
	bool			kw_wake_armed;	/* kw_wake_co is pending */
	/*
	 * The urgent items' bound inside the tick regime (tick_urgent_us):
	 * armed by the first urgent item that finds the worker asleep in the
	 * regime, stopped when the worker wakes for any reason.
	 */
	struct callout		kw_turg_co;
	bool			kw_turg_armed;	/* kw_turg_co is pending */
	sbintime_t		kw_wake_since;	/* when the idle-class wait began */

	bool			kw_tick;	/* in the regime (under kw_mtx) */
	bool			kw_tick_round;	/* the round in progress began with a tick */
	bool			kw_tick_coalesced;	/* a tick's round of this episode served > 1 */
	u_int			kw_tick_short;	/* consecutive sleeps shorter than L */
	sbintime_t		kw_tick_holdoff;	/* no entry before this time */
	sbintime_t		kw_tick_backoff;	/* the current hold-off, 2L .. 1024L */
	sbintime_t		kw_tick_start;	/* sbinuptime of the entry */
	uint64_t		kw_tick_items;	/* elements served since the last wake */
	uint64_t		kw_round_wall;	/* sbinuptime ns at the last round end (kwq:::round-end) */
	uint64_t		kw_idle_busy;	/* kw_busy_ns at the last wakeup (kwq:::idle) */
	const struct kwq_sched_knobs *kw_knobs;
	uint64_t		kw_ns_scale;	/* ks_ns_scale(kwq_tickrate()) */
	uint64_t		kw_rate;	/* the rate the scale was derived from */
	/*
	 * CPU-time base (kwq_cputime_ns_tick): the thread's CPU time in ticks
	 * at kw_seg_k, exact from td_runtime and switchtime, valid while
	 * td_runtime still equals kw_seg_rt, i.e. the worker has not been
	 * switched out since; then CPU time = kw_seg_cpu + (kwq_ticks() -
	 * kw_seg_k) without the kernel's ticker (kwq_clock.h).
	 */
	uint64_t		kw_seg_rt, kw_seg_cpu, kw_seg_k;
	uint64_t		kw_pass_rt;	/* NET: td_runtime at pass begin, to detect a switch-out */
	uint64_t		kw_rounds, kw_passes, kw_wakeups, kw_yields,
				kw_tick_yields, kw_handbacks, kw_cap_demotions,
				kw_cap_yields, kw_cap_restores, kw_cap_ns, kw_cap_runon,
				kw_cap_cuts, kw_slice_yields,
				kw_busy_ns, kw_idle_ns, kw_budget_calls, kw_budget_reads, kw_switched,
				kw_ticks, kw_ticks_empty, kw_ticks_single, kw_tick_enters,
				kw_tick_exits, kw_tick_ns, kw_tick_barren, kw_prio_violations,
				kw_wakes_low, kw_wake_self, kw_wake_prom_deadline,
				kw_wake_prom_urgent, kw_wake_low_ns, kw_wakes_urgent,
				kw_turg_deferred, kw_turg_wakes;
	/*
	 * BULK stealing (SCHED.md S16): attempts made with nothing of its own
	 * to do, of those the ones that moved a batch, the items moved, and
	 * the wakes producers sent it to steal (kw_steal_wakes under kw_mtx).
	 */
	uint64_t		kw_steal_tries, kw_steals, kw_stolen, kw_steal_wakes;
	/*
	 * P8 (place=2, ../SPEC-p6g.md K8-K12, kwq_shard.c): where the idle
	 * bit was set (KWQ_IDLE_*), the donor state (kw_donor, kw_starved,
	 * the backlog at the last decision point), the decision points
	 * counted for the starved tick, and the tick itself.  All under
	 * kw_mtx; kw_donor and kw_starved are read unlocked by thieves.
	 */
	u_char			kw_idle_at;
	bool			kw_donor;
	bool			kw_helper_donor;	/* a donor for the helper it woke */
	volatile bool		kw_starved;
	bool			kw_watch_armed;
	uint64_t		kw_backlog_ns;
	uint64_t		kw_dec_n, kw_watch_n;
	struct callout		kw_watch_co;
	uint64_t		kw_pulls, kw_donor_n, kw_starved_n, kw_directed;
	/*
	 * The gossip scheme (place=1, kwq_gossip.c): the load figure's state,
	 * the backlog at the last decision point (step 0's own load), the
	 * callout that publishes a sleeper's fall after the hold, hand-offs.
	 */
	struct kp_load		kw_kl;
	uint64_t		kw_kp_start_ns;
	struct callout		kw_pub_co;
	bool			kw_pub_armed;
	uint64_t		kw_handoffs;
} __aligned(KWQ_LINE);

extern struct kwq_worker **kwq_workers[KWQ_NCLASS];	/* [class][cpu] */
LIST_HEAD(kwq_list, kwq);
extern struct kwq_list kwq_all;		/* every queue, under kwq_sx (kwq.c) */
extern const char *kwq_class_names[KWQ_NCLASS];
extern int kwq_yield_prio[KWQ_NCLASS];
extern int kwq_cap_prio[KWQ_NCLASS];
extern u_int kwq_tick_us[KWQ_NCLASS];	/* the tick regime's bound L; 0 = off (KWQ.md S16) */
extern u_int kwq_wake_delay_us[KWQ_NCLASS];	/* the idle-class wait's deadline; 0 = off */
extern u_int kwq_tick_urgent_us[KWQ_NCLASS];	/* urgent items' bound in the tick regime; 0 = at once */

/*
 * BULK stealing and scatter (SCHED.md S16, KWQ.md S3).  kwq_steal_list
 * holds the KWQ_F_STEALABLE queues; kwq_steal_rm protects it and fences a
 * BULK queue's drain against batches in flight (a steal or a scatter
 * holds the read lock from its check of kwq_active to its last append).
 * kwq_bulk_idle has a bit per CPU whose BULK worker sleeps, a hint read
 * without locks (kw_sleeping under kw_mtx is the truth).  kwq_topo_order
 * lists, per CPU, every other CPU nearest first in cache topology, and
 * kwq_topo_ring the end of each ring of it (KWQ_TOPO_RINGS at most).
 */
#define	KWQ_TOPO_RINGS	8
LIST_HEAD(kwq_steal_head, kwq);
extern struct kwq_steal_head kwq_steal_list;
extern struct rmlock kwq_steal_rm;
extern volatile u_int kwq_nstealable;
extern struct _cpuset kwq_bulk_idle;
extern int **kwq_topo_order;
extern u_short (*kwq_topo_ring)[KWQ_TOPO_RINGS];
extern u_int kwq_steal_on, kwq_steal_scope, kwq_steal_scan, kwq_steal_max,
	    kwq_steal_wake_at, kwq_steal_batch;

/*
 * BULK placement (../SPEC-p6g.md S2.8): kern.kwq.bulk.place 0 is
 * kwq_pick_any(), the one idle mask and kwq_steal()'s scan (SCHED.md
 * S16); 2 is P8 (kwq_shard.c): the idle mask in lines of
 * kwq_shard_group CPUs, the donor line and its hints, the starved tick.
 * The stealable queues have a slot each in kwq_steal_tab, under
 * kwq_steal_rm, so that a hint names a queue without a pointer to it.
 */
#define	KWQ_PLACE_TODAY		0
#define	KWQ_PLACE_GOSSIP	1
#define	KWQ_PLACE_SHARD		2
#define	KWQ_STEAL_SLOTS		4096
#define	KWQ_IDLE_NONE		0
#define	KWQ_IDLE_MASK		1	/* kwq_bulk_idle */
#define	KWQ_IDLE_GROUP		2	/* its group's line */
extern volatile u_int kwq_bulk_place;
extern struct kwq *kwq_steal_tab[KWQ_STEAL_SLOTS];

/*
 * How many consecutive sleeps shorter than L enter the tick regime
 * (KWQ.md S16).  Three: the first build entered on two doorbell wakes
 * within L, which every burst of two items satisfies, and so charged a
 * short burst with a long pause one L on its tail for one saved wake
 * (WAKE.md S10.3).
 */
#define	KWQ_TICK_ENTER	3

int	kwq_class_priority(enum kwq_class cls);

static inline void
kwq_kc_lock(struct kwq_cpu *kc)
{
	if (kc->kc_flags & KWQ_KC_SPIN)
		mtx_lock_spin(&kc->kc_mtx);
	else
		mtx_lock(&kc->kc_mtx);
}

static inline void
kwq_kc_unlock(struct kwq_cpu *kc)
{
	if (kc->kc_flags & KWQ_KC_SPIN)
		mtx_unlock_spin(&kc->kc_mtx);
	else
		mtx_unlock(&kc->kc_mtx);
}

/* Sleep on kc with its mutex held; the mutex is released and reacquired. */
static inline void
kwq_kc_wait(struct kwq_cpu *kc, const char *wmesg)
{
	kc->kc_waiters++;
	if (kc->kc_flags & KWQ_KC_SPIN)
		msleep_spin(kc, &kc->kc_mtx, wmesg, 0);
	else
		msleep(kc, &kc->kc_mtx, 0, wmesg, 0);
	kc->kc_waiters--;
}

/*
 * sbintime_t (32.32 s) -> ns.  sbttons() splits the multiply so absolute
 * uptimes convert correctly; the first version multiplied the whole value
 * by 1e9 and wrapped after 2.1 s of uptime, which the kwq:::round-end
 * probe exposed as negative round lengths (2026-09-29; the cap window,
 * off by default, was the other user).
 */
static inline uint64_t
kwq_sbt2ns(sbintime_t sbt)
{
	return ((uint64_t)sbttons(sbt));
}

/*
 * How an enqueue rings the doorbell (KWQ.md S16).  A plain ring takes
 * the opt-in regimes; a signal (a notifier, a drain, a cancel) and a
 * client's urgent item (kwq_enqueue_urgent()) take neither, except that
 * inside the tick regime an urgent item waits tick_urgent_us when that
 * is set, so that urgent items close together share one wake.
 */
#define	KWQ_RING_PLAIN	0
#define	KWQ_RING_SIGNAL	1
#define	KWQ_RING_ITEM	2

/* kwq_worker.c */
int	kwq_workers_start(struct sysctl_oid_list **cpu_oids);
void	kwq_workers_stop(void);
bool	kwq_kc_place(struct kwq_cpu *kc, u_int added);	/* kc locked, `added'
					   items just appended: stamp, doorbell */
void	kwq_doorbell(struct kwq_cpu *kc, int ring);	/* kc locked, state IDLE;
					   `ring' one of KWQ_RING_* */
bool	kwq_urgent_nudge(struct kwq_cpu *kc, int ring);	/* kc locked, state
					   WAKING: wake a sleeping worker for an
					   urgent item added to an already placed
					   list */
bool	kwq_higher_waiting(enum kwq_class cls);	/* hand-back flags, this CPU */
void	kwq_steal_wake(int cpu, struct kwq_cpu *kc);	/* no locks held: wake a
					   sleeping BULK worker near `cpu' to steal
					   from it (S16), kc the list if known */
bool	kwq_steal_idle_near(int cpu);	/* a BULK worker near `cpu' sleeps (hint) */
void	kwq_topo_init(void);
void	kwq_topo_fini(void);
u_int	kwq_topo_reach(int cpu);	/* entries of kwq_topo_order[cpu] within
					   kwq_steal_scope rings, at most kwq_steal_scan */
uint64_t kwq_cputime_ns(struct kwq_worker *kw);

/* kwq_shard.c (P8, place=2) */
void	kwq_shard_init(void);		/* after kwq_topo_init()'s order */
void	kwq_shard_fini(void);
int	kwq_shard_slot_add(struct kwq *q);	/* kwq_steal_rm write-locked */
void	kwq_shard_slot_del(struct kwq *q);	/* kwq_steal_rm write-locked */
void	kwq_idle_set(struct kwq_worker *kw);	/* kw_mtx held, going to sleep */
void	kwq_idle_clr(struct kwq_worker *kw);	/* kw_mtx held, awake */
bool	kwq_shard_idle_near(int cpu);	/* a BULK worker in reach sleeps (hint) */
bool	kwq_bulk_idle_hint(int cpu);	/* cpu's BULK worker sleeps, in the mode's view */
bool	kwq_shard_wake(int cpu, struct kwq_cpu *kc);	/* wake a sleeper in reach to
					   steal from cpu (kc, or its hint); no lock held */
bool	kwq_donor_point(struct kwq_worker *kw, struct kwq_cpu *cur, u_int ahead);
					/* kw_mtx held: true if it just became a donor */
bool	kwq_donor_helper(struct kwq_worker *kw, struct kwq_cpu *kc);	/* kw_mtx held */
bool	kwq_steal_donor(struct kwq_worker *kw, int64_t room_ns, int mcpu,
	    uint64_t mword);	/* no lock held; mcpu/mword: the mailbox, or -1/0 */
void	kwq_shard_pull(struct kwq_worker *kw);	/* after a pass, no lock held */
int	kwq_pick_shard(struct kwq *q);	/* KWQ_CPU_ANY under place=2 */
int	kwq_shard_spill(struct kwq *q, int cpu);	/* a fixed CPU, or where it spills */
void	kwq_watch_arm(struct kwq_worker *kw);	/* kw_mtx held */
void	kwq_pass_cost(struct kwq_cpu *kc, uint64_t ns, u_int n);	/* pass end, owner */

/* kwq_gossip.c (the gossip scheme, place=1) */
void	kwq_gossip_init(void);		/* after kwq_shard_init() */
void	kwq_gossip_fini(void);
void	kwq_gossip_point(struct kwq_worker *kw, struct kwq_cpu *cur, u_int ahead);
					/* kw_mtx held: the load figure, published */
void	kwq_gossip_sleep(struct kwq_worker *kw);	/* kw_mtx held, going to sleep */
void	kwq_gossip_merge(int cpu);	/* a due merge of cpu's core */
int	kwq_pick_gossip(struct kwq *q);	/* KWQ_CPU_ANY under place=1 */
int	kwq_gossip_spill(struct kwq *q, int cpu);
bool	kwq_gossip_idle_near(int cpu);	/* an idle core in the table near cpu */
bool	kwq_gossip_cpu_idle(int cpu);	/* cpu's status word says idle */
void	kwq_gossip_handoff(struct kwq_worker *kw);	/* after a pass, no lock held */
uint64_t kwq_cputime_ns_tick(struct kwq_worker *kw, uint64_t *tick);


/*
 * The per-CPU list holds items and notifiers in arrival order.  A
 * notifier's own outgoing link carries KWQ_LINK_TAG (kwq.h); the head
 * pointer and every item's link are plain.  Only these helpers touch
 * the links, since the STAILQ macros would lose or misread the tag.
 */
#define	KWQ_TAGBIT(p)	((uintptr_t)(p) & KWQ_LINK_TAG)

/*
 * make KWQ_LIST_CHECK=1: walk a (queue, CPU) list under its lock after
 * every change that moves more than one element (a pass's cut, a steal, a
 * requeue, a scatter's chunk) and assert that its length is kc_depth and
 * that stqh_last is its last link.  O(depth); for tests on the guest.
 */
#ifdef KWQ_LIST_CHECK
static inline void
kwq_list_check(struct kwq_cpu *kc, const char *where)
{
	struct kwq_item *it, *last = NULL;
	u_int n = 0;

	for (it = kc->kc_list.stqh_first; it != NULL;
	    it = (struct kwq_item *)((uintptr_t)it->kwi_link.stqe_next & ~KWQ_LINK_TAG)) {
		last = it;
		if (++n > kc->kc_depth + 1)
			break;
	}
	KASSERT(n == kc->kc_depth, ("kwq %s: %s cpu%d: %u elements, depth %u",
	    where, kc->kc_q->kwq_name, kc->kc_cpu, n, kc->kc_depth));
	KASSERT(last == NULL ? kc->kc_list.stqh_last == &kc->kc_list.stqh_first :
	    kc->kc_list.stqh_last == &last->kwi_link.stqe_next,
	    ("kwq %s: %s cpu%d: stqh_last %p, last element %p", where,
	    kc->kc_q->kwq_name, kc->kc_cpu, kc->kc_list.stqh_last, last));
}
#else
#define	kwq_list_check(kc, where)	do { } while (0)
#endif

/* Append `it' (a notifier if `tag') as the last element. */
static inline void
kwq_list_append(struct kwq_cpu *kc, struct kwq_item *it, bool tag)
{
	it->kwi_link.stqe_next = (struct kwq_item *)(tag ? KWQ_LINK_TAG : 0);
	*kc->kc_list.stqh_last = (struct kwq_item *)((uintptr_t)it |
	    KWQ_TAGBIT(*kc->kc_list.stqh_last));
	kc->kc_list.stqh_last = &it->kwi_link.stqe_next;
}

/* Append a client's list of items (plain links, tail->next == NULL). */
static inline void
kwq_list_concat(struct kwq_cpu *kc, struct kwq_item *head, struct kwq_item *tail)
{
	*kc->kc_list.stqh_last = (struct kwq_item *)((uintptr_t)head |
	    KWQ_TAGBIT(*kc->kc_list.stqh_last));
	kc->kc_list.stqh_last = &tail->kwi_link.stqe_next;
}

/* Prepend head..tail, keeping tail's own tag; for kwq_requeue(). */
static inline void
kwq_list_prepend(struct kwq_cpu *kc, struct kwq_item *head, struct kwq_item *tail)
{
	tail->kwi_link.stqe_next = (struct kwq_item *)
	    ((uintptr_t)kc->kc_list.stqh_first | KWQ_TAGBIT(tail->kwi_link.stqe_next));
	if (kc->kc_list.stqh_first == NULL)
		kc->kc_list.stqh_last = &tail->kwi_link.stqe_next;
	kc->kc_list.stqh_first = head;
}

#endif /* !_KWQ_INTERNAL_H_ */
