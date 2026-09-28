/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq(9) - kernel work queues: public KPI.
 *
 * Design: ../KWQ.md (S2 objects and lifecycle, S3 contracts, S6 what a
 * handler may do).  Terminology: ../GLOSSARY.md.
 *
 * A queue belongs to one work class (NET, BULK, BLOCKING) and has one
 * intrusive FIFO list per CPU, served by that CPU's class worker.  The
 * client embeds struct kwq_item in its own objects, chooses the CPU per
 * item (its ordering domain), and supplies one handler that receives
 * whole batches.  Nothing is allocated on the data path.
 */

#ifndef _KWQ_H_
#define	_KWQ_H_

#include <sys/param.h>
#include <sys/queue.h>

struct kwq;

/*
 * Embedded in the client's object.  An item is either not queued, on
 * exactly one (queue, CPU) list, or owned by the handler that received
 * it; the client provides the proof (double enqueue is a bug).
 */
struct kwq_item {
	STAILQ_ENTRY(kwq_item)	kwi_link;
};
#define	KWQ_ITEM_NEXT(it)	STAILQ_NEXT((it), kwi_link)
#define	KWQ_ITEM_INIT(it)	((it)->kwi_link.stqe_next = NULL)

/*
 * The handler.  `head' is a private list of `n' items in FIFO order
 * which the handler owns and must consume (process, free, requeue or
 * hand on).  n < 0 means "release these -n items without running them"
 * (drain of a KWQ_F_DISCARD queue).  Notifiers are delivered one per
 * call, n == 1 (or -1), so that a re-notify during the call is safe.
 */
typedef void kwq_handler_t(struct kwq *q, struct kwq_item *head, int n,
    void *ctx);

enum kwq_class {
	KWQ_NET = 0,		/* PI_NET, never sleeps, one worker per CPU */
	KWQ_BULK = 1,		/* PI_SOFT, never sleeps, stealable */
	KWQ_BLOCKING = 2,	/* PUSER, may sleep (P7, not yet available) */
};
#define	KWQ_NCLASS	3

struct kwq_params {
	u_int	limit;		/* items per CPU list; 0 = class default */
	u_int	weight;		/* 1..8; 0 = 1 */
	u_int	nreserve;	/* BLOCKING + KWQ_F_RESERVE only */
	int	domain;		/* NUMA domain for internal memory; -1 = CPU's own */
};
#define	KWQ_LIMIT_NONE	UINT_MAX	/* unbounded: closed/reclamation only */

#define	KWQ_F_INACTIVE	0x01	/* create inactive; kwq_activate() later */
#define	KWQ_F_STEALABLE	0x02	/* BULK: idle workers may take batches; no
				   kwq_notify() on such a queue */
#define	KWQ_F_DISCARD	0x04	/* drain hands pending items to the handler
				   with n < 0 instead of running them */
#define	KWQ_F_RESERVE	0x08	/* BLOCKING: nreserve reserved workers */
#define	KWQ_F_VNET	0x10	/* handler runs with the creator's vnet set */
#define	KWQ_F_SPIN	0x20	/* per-CPU lists use spin mutexes: producers may
				   be interrupt filters or hold spin locks */
#define	KWQ_F_ALL	0x3f

#define	KWQ_CPU_ANY	(-1)	/* BULK only: least loaded CPU near the caller */
#define	KWQ_NAMELEN	32

/*
 * Idempotent signal.  Preallocated and owned by the client, bound to one
 * CPU for its lifetime; kwq keeps kn_state.  kwq_notify() cannot fail.
 */
struct kwq_notifier {
	struct kwq_item	kn_item;
	int		kn_cpu;
	u_int		kn_state;
};

/* Configuration (sleepable context). */
struct kwq	*kwq_create(const char *name, enum kwq_class cls, uint32_t flags,
		    const struct kwq_params *p, kwq_handler_t *fn, void *ctx);
void		 kwq_activate(struct kwq *q);
void		 kwq_drain(struct kwq *q);
void		 kwq_destroy(struct kwq *q);

/* Data path (never sleeps, never allocates). */
int		 kwq_enqueue(struct kwq *q, int cpu, struct kwq_item *it);
int		 kwq_enqueue_list(struct kwq *q, int cpu, struct kwq_item *head,
		    struct kwq_item *tail, int n);
int		 kwq_cpu_for_hash(uint32_t hash);
int		 kwq_scatter(struct kwq *q, struct kwq_item *list, int n,
		    void (*done)(void *), void *arg);

/* Signals. */
void		 kwq_notifier_init(struct kwq_notifier *nf, int cpu);
bool		 kwq_notify(struct kwq *q, struct kwq_notifier *nf);
void		 kwq_notify_cancel(struct kwq *q, struct kwq_notifier *nf);

/* Handler only. */
void		 kwq_requeue(struct kwq *q, struct kwq_item *head,
		    struct kwq_item *tail, int n);
uint64_t	 kwq_budget_left(struct kwq *q);

/* Introspection. */
const char	*kwq_name(const struct kwq *q);
enum kwq_class	 kwq_class(const struct kwq *q);

/*
 * mbuf mapping: an mbuf's m_stailqpkt link (the m_nextpkt union member,
 * what mbufq(9) uses) is layout-identical to struct kwq_item, so packet
 * clients pay no extra field.
 */
#ifdef _SYS_MBUF_H_
#define	KWQ_MBUF_ITEM(m)						\
	((struct kwq_item *)(void *)&(m)->m_stailqpkt)
#define	KWQ_ITEM_MBUF(it)						\
	((struct mbuf *)(void *)((char *)(it) -				\
	    __offsetof(struct mbuf, m_stailqpkt)))
CTASSERT(sizeof(STAILQ_ENTRY(mbuf)) == sizeof(struct kwq_item));
CTASSERT(__offsetof(struct mbuf, m_stailqpkt) ==
    __offsetof(struct mbuf, m_nextpkt));
#endif

#endif /* !_KWQ_H_ */
