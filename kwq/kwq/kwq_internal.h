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
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/time.h>

#include "kwq.h"

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
	uint64_t		kc_steals_in;	/* P6 */
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

	/* Runtime-written: own block so a drain does not dirty the header. */
	volatile u_int		kwq_active __aligned(KWQ_LINE);
	u_int			kwq_drained;
	u_int			kwq_state;	/* KWQ_ST_*, for sysctl and DDB */
	LIST_ENTRY(kwq)		kwq_all;
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
void	kwq_doorbell(struct kwq_cpu *kc, int ring);	/* kc locked, state IDLE;
					   `ring' one of KWQ_RING_* */
bool	kwq_urgent_nudge(struct kwq_cpu *kc, int ring);	/* kc locked, state
					   WAKING: wake a sleeping worker for an
					   urgent item added to an already placed
					   list */
bool	kwq_higher_waiting(enum kwq_class cls);	/* hand-back flags, this CPU */
uint64_t kwq_cputime_ns(struct kwq_worker *kw);
uint64_t kwq_cputime_ns_tick(struct kwq_worker *kw, uint64_t *tick);


/*
 * The per-CPU list holds items and notifiers in arrival order.  A
 * notifier's own outgoing link carries KWQ_LINK_TAG (kwq.h); the head
 * pointer and every item's link are plain.  Only these helpers touch
 * the links, since the STAILQ macros would lose or misread the tag.
 */
#define	KWQ_TAGBIT(p)	((uintptr_t)(p) & KWQ_LINK_TAG)

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
