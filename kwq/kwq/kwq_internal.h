/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq internals: struct kwq, struct kwq_cpu, struct kwq_worker.
 * Layout rules: ../KWQ.md S17.
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

#include "kwq.h"

/*
 * Granule for producer/consumer shared state.  128 rather than
 * CACHE_LINE_SIZE because amd64's spatial prefetcher pairs 64-byte lines
 * into 128-byte blocks, so two CPUs' private state in adjacent 64-byte
 * lines still bounces; arm64 is 128 anyway.
 */
#define	KWQ_LINE	(CACHE_LINE_SIZE > 128 ? CACHE_LINE_SIZE : 128)

MALLOC_DECLARE(M_KWQ);

/* (queue, CPU) states; the doorbell rings only on IDLE -> WAKING. */
enum kwq_cpu_state {
	KWQ_CPU_IDLE = 0,	/* list empty, worker not scheduled for it */
	KWQ_CPU_WAKING,		/* on the worker's active list */
	KWQ_CPU_RUNNING,	/* worker holds a batch from it */
	KWQ_CPU_PARKED,		/* negative deficit (P1) */
};

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
	 * Block 0 (one KWQ_LINE): shared between producers and the consumer,
	 * everything under kc_mtx.  The per-enqueue set (mutex, list, depth,
	 * state) sits in the first 64 bytes; burst-start and rare fields
	 * follow.
	 */
	struct mtx		kc_mtx;
	STAILQ_HEAD(, kwq_item)	kc_list;	/* items */
	u_int			kc_depth;	/* items on kc_list */
	u_int			kc_state;	/* enum kwq_cpu_state */
	u_int			kc_nnotify;	/* notifiers on kc_notify */
	u_int			kc_flags;	/* ---- 64 bytes up to here ---- */
	STAILQ_HEAD(, kwq_item)	kc_notify;	/* pending notifiers */
	u_int			kc_waiters;	/* drain/cancel sleepers on kc */
	u_int			kc_pad0;
	uint64_t		kc_empty_since;	/* cpu_ticks at empty -> non-empty */
	TAILQ_ENTRY(kwq_cpu)	kc_active;	/* worker's active list */
	uint64_t		kc_rejected;	/* ENOBUFS/ENXIO, producer under lock */
	uint64_t		kc_coalesced;	/* kwq_notify() no-ops */

	/* Block 1..: consumer-private, written only by the owning worker. */
	struct kwq		*kc_q __aligned(KWQ_LINE);
	int			kc_cpu;
	int64_t			kc_deficit;	/* P1 */
	uint64_t		kc_items;	/* items + notifiers handed to the handler */
	uint64_t		kc_passes;
	uint64_t		kc_cycles;	/* cpu_ticks spent in the handler */
	uint64_t		kc_requeued;
	uint64_t		kc_maxdepth;	/* sampled at pass start */
	uint64_t		kc_maxlat_ns;	/* P2 */
	uint64_t		kc_overruns;	/* P1 */
	uint64_t		kc_parks;	/* P1 */
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
	/* Shared with producers (doorbell) under kw_mtx, a spin mutex so that
	   KWQ_F_SPIN producers may ring it. */
	struct mtx		kw_mtx;
	TAILQ_HEAD(, kwq_cpu)	kw_active;
	bool			kw_sleeping;
	bool			kw_exit;
	struct thread		*kw_td;

	/* Private. */
	enum kwq_class		kw_class __aligned(KWQ_LINE);
	int			kw_cpu;
	int			kw_pri;
	struct kwq_cpu		*kw_cur;	/* pass in progress */
	uint64_t		kw_passes;
	uint64_t		kw_wakeups;
} __aligned(KWQ_LINE);

extern struct kwq_worker **kwq_workers[KWQ_NCLASS];	/* [class][cpu] */
extern const char *kwq_class_names[KWQ_NCLASS];

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

/* kwq.c */
int	kwq_class_priority(enum kwq_class cls);

/* kwq_worker.c */
int	kwq_workers_start(void);
void	kwq_workers_stop(void);
void	kwq_doorbell(struct kwq_cpu *kc);	/* kc locked, state IDLE */

#endif /* !_KWQ_INTERNAL_H_ */
