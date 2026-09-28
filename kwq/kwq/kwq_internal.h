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
	KWQ_NF_CANCELLED,	/* kwq_notify_cancel() ran; not run again until re-init */
};

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
	 * kc_onlist, kc_warm and kc_idle_round are under kw_mtx.
	 */
	struct mtx		kc_mtx;
	STAILQ_HEAD(, kwq_item)	kc_list;	/* items */
	u_int			kc_depth;	/* items on kc_list */
	u_int			kc_state;	/* enum kwq_cpu_state (kwq_sched.h) */
	u_int			kc_nnotify;	/* notifiers on kc_notify */
	u_int			kc_flags;	/* ---- 64 bytes up to here ---- */
	STAILQ_HEAD(, kwq_item)	kc_notify;	/* pending notifiers */
	u_short			kc_waiters;	/* drain/cancel sleepers on kc */
	u_char			kc_onlist;	/* enum kwq_onlist */
	u_char			kc_warm;	/* grace rule (SCHED.md S4.6) */
	u_int			kc_idle_round;	/* low 32 bits of kw_round at idle */
	sbintime_t		kc_empty_since;	/* sbinuptime at empty -> non-empty */
	TAILQ_ENTRY(kwq_cpu)	kc_active;	/* worker's new list or ring */
	uint64_t		kc_rejected;	/* ENOBUFS/ENXIO, producer under lock */
	uint64_t		kc_coalesced;	/* kwq_notify() no-ops */

	/* Block 1..: consumer-private, written only by the owning worker. */
	struct kwq		*kc_q __aligned(KWQ_LINE);
	int			kc_cpu;
	u_int			kc_src;		/* enum kwq_src: list of the pass in progress */
	int64_t			kc_deficit;	/* DRR deficit, ns */
	uint64_t		kc_items;	/* items + notifiers handed to the handler */
	uint64_t		kc_passes;
	uint64_t		kc_cycles;	/* wall cpu_ticks spent in passes */
	uint64_t		kc_requeued;
	uint64_t		kc_maxdepth;	/* sampled at pass start */
	uint64_t		kc_maxlat_ns;	/* oldest-item age at pass start */
	uint64_t		kc_overruns;	/* passes > budget + Qw */
	uint64_t		kc_parks;	/* rounds skipped for a deficit <= 0 */
	uint64_t		kc_boosts;	/* passes served from the new list */
	uint64_t		kc_grace;	/* doorbells sent to the ring by the grace rule */
	uint64_t		kc_glitches;	/* passes with a negative or > KS_GLITCH_NS CPU-time delta */
	uint64_t		kc_steals_in;	/* P6 */
} __aligned(KWQ_LINE);

CTASSERT(__offsetof(struct kwq_cpu, kc_notify) <= 64);	/* item path in line 0 */
CTASSERT(__offsetof(struct kwq_cpu, kc_q) == KWQ_LINE);

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
	LIST_ENTRY(kwq)		kwq_all;
	struct sysctl_ctx_list	kwq_sysctl;
} __aligned(KWQ_LINE);

CTASSERT(__offsetof(struct kwq, kwq_active) == KWQ_LINE);

struct kwq_worker {
	/*
	 * Block 0: shared with producers (doorbell) under kw_mtx, a spin mutex
	 * so that KWQ_F_SPIN producers may ring it.  kw_round is a plain
	 * store by the worker, read by producers under kw_mtx for the grace
	 * rule (SCHED.md S3).
	 */
	struct mtx		kw_mtx;
	TAILQ_HEAD(, kwq_cpu)	kw_new;		/* newly non-empty: boosted */
	TAILQ_HEAD(, kwq_cpu)	kw_active;	/* the DRR ring */
	u_int			kw_nactive;
	u_int			kw_nnew;	/* length of kw_new (SCHED.md S3) */
	bool			kw_sleeping;
	bool			kw_exit;
	struct thread		*kw_td;
	uint64_t		kw_round;
	void			*kw_chan;	/* wait channel (kwq_worker.c) */

	/* Block 1..: private to the worker (ks_* run under kw_mtx anyway). */
	enum kwq_class		kw_class __aligned(KWQ_LINE);
	int			kw_cpu;
	int			kw_pri;
	u_int			kw_phase;	/* enum kwq_phase */
	int			kw_ring_left;
	int			kw_tail_left;
	bool			kw_want_new;
	u_int			kw_served;
	struct kwq_cpu		*kw_cur;	/* pass in progress */
	uint64_t		kw_pass_start;	/* worker CPU time, ns */
	int64_t			kw_pass_budget;	/* deficit at pass start, ns */
	uint64_t		kw_win_start;	/* cap window, wall ns */
	uint64_t		kw_win_busy;
	const struct kwq_sched_knobs *kw_knobs;
	uint64_t		kw_ns_scale;	/* ks_ns_scale(cpu_tickrate()) */
	uint64_t		kw_rate;	/* the rate the scale was derived from */
	uint64_t		kw_rounds, kw_passes, kw_wakeups, kw_yields,
				kw_tick_yields, kw_cap_sleeps, kw_handbacks,
				kw_busy_ns, kw_idle_ns;
} __aligned(KWQ_LINE);

extern struct kwq_worker **kwq_workers[KWQ_NCLASS];	/* [class][cpu] */
extern const char *kwq_class_names[KWQ_NCLASS];
extern int kwq_yield_prio[KWQ_NCLASS];

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

/* sbintime_t (32.32 s) -> ns, for differences only. */
static inline uint64_t
kwq_sbt2ns(sbintime_t sbt)
{
	return (((uint64_t)sbt * 1000000000ULL) >> 32);
}

/* kwq_worker.c */
int	kwq_workers_start(struct sysctl_oid_list **class_oids);
void	kwq_workers_stop(void);
void	kwq_doorbell(struct kwq_cpu *kc);	/* kc locked, state IDLE */
bool	kwq_higher_waiting(enum kwq_class cls);	/* hand-back flags, this CPU */
uint64_t kwq_cputime_ns(const struct kwq_worker *kw);

#endif /* !_KWQ_INTERNAL_H_ */
