/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq DTrace provider: probe declarations for the module's other files and
 * the hooks the scheduler core fires through (kwq_sched_env.h).  The probe
 * set, argument lists and cost classes are ../KWQ.md S10.1; the translator
 * is kwq.d (KWQ.md S10.2).  args[0] of the queue probes is a kwqinfo_t.
 */

#ifndef _KWQ_SDT_H_
#define	_KWQ_SDT_H_

#include <sys/sdt.h>

/*
 * KWQ_NO_SDT (make KWQ_NO_SDT=1): compile every probe site to nothing,
 * for measuring what the disabled probes cost (KWQ.md S10.3).  The
 * provider and probe definitions in kwq_sdt.c stay, so dtrace -l still
 * lists them; they simply never fire.  The arguments stay behind in an
 * unevaluated sizeof, so a variable only a probe reads (the doorbell's
 * state) is still used and costs nothing.
 */
#ifdef KWQ_NO_SDT
#undef SDT_PROBE1
#undef SDT_PROBE2
#undef SDT_PROBE3
#undef SDT_PROBE4
#undef SDT_PROBE5
#undef SDT_PROBE6
#define	KWQ_SDT_NOARG(a)	(void)sizeof(a)
#define	SDT_PROBE1(prov, mod, func, name, a0)				\
	do { KWQ_SDT_NOARG(a0); } while (0)
#define	SDT_PROBE2(prov, mod, func, name, a0, a1)			\
	do { KWQ_SDT_NOARG(a0); KWQ_SDT_NOARG(a1); } while (0)
#define	SDT_PROBE3(prov, mod, func, name, a0, a1, a2)			\
	do { KWQ_SDT_NOARG(a0); KWQ_SDT_NOARG(a1); KWQ_SDT_NOARG(a2); } while (0)
#define	SDT_PROBE4(prov, mod, func, name, a0, a1, a2, a3)		\
	do { KWQ_SDT_NOARG(a0); KWQ_SDT_NOARG(a1); KWQ_SDT_NOARG(a2);	\
	    KWQ_SDT_NOARG(a3); } while (0)
#define	SDT_PROBE5(prov, mod, func, name, a0, a1, a2, a3, a4)		\
	do { KWQ_SDT_NOARG(a0); KWQ_SDT_NOARG(a1); KWQ_SDT_NOARG(a2);	\
	    KWQ_SDT_NOARG(a3); KWQ_SDT_NOARG(a4); } while (0)
#define	SDT_PROBE6(prov, mod, func, name, a0, a1, a2, a3, a4, a5)	\
	do { KWQ_SDT_NOARG(a0); KWQ_SDT_NOARG(a1); KWQ_SDT_NOARG(a2);	\
	    KWQ_SDT_NOARG(a3); KWQ_SDT_NOARG(a4); KWQ_SDT_NOARG(a5); } while (0)
#endif

SDT_PROVIDER_DECLARE(kwq);
SDT_PROBE_DECLARE(kwq, , , create);
SDT_PROBE_DECLARE(kwq, , , activate);
SDT_PROBE_DECLARE(kwq, , , drain__start);
SDT_PROBE_DECLARE(kwq, , , drain__end);
SDT_PROBE_DECLARE(kwq, , , destroy);
SDT_PROBE_DECLARE(kwq, , , enqueue);
SDT_PROBE_DECLARE(kwq, , , reject);
SDT_PROBE_DECLARE(kwq, , , pass__start);
SDT_PROBE_DECLARE(kwq, , , pass__end);
SDT_PROBE_DECLARE(kwq, , , overrun);
SDT_PROBE_DECLARE(kwq, , , park);
SDT_PROBE_DECLARE(kwq, , , round__end);
SDT_PROBE_DECLARE(kwq, , , yield);
SDT_PROBE_DECLARE(kwq, , , cap);
SDT_PROBE_DECLARE(kwq, , , idle);
SDT_PROBE_DECLARE(kwq, , , doorbell);
SDT_PROBE_DECLARE(kwq, , , tick);
SDT_PROBE_DECLARE(kwq, , , wake);
SDT_PROBE_DECLARE(kwq, , , steal);
SDT_PROBE_DECLARE(kwq, , , worker__block);
SDT_PROBE_DECLARE(kwq, , , worker__spawn);
SDT_PROBE_DECLARE(kwq, , , worker__exit);
SDT_PROBE_DECLARE(kwq, , , budget__hit);

/* kwq:::doorbell worker states */
#define	KWQ_DB_AWAKE	0
#define	KWQ_DB_POLLING	1	/* reserved: no worker polls in this tree */
#define	KWQ_DB_ASLEEP	2
#define	KWQ_DB_LOWPRI	4	/* asleep, made runnable in the idle class (KWQ.md S16) */
#define	KWQ_DB_TICKED	6	/* asleep with the tick armed: placed, not woken (KWQ.md S16) */

/*
 * kwq:::wake reasons: why a worker waiting in the idle class was promoted
 * to the class priority (KWQ.md S16).  The numbers are those of the
 * kwq-wake-idle branch; 0, 1 and 3 were its count-threshold and kwq_kick()
 * reasons, which are not implemented (a count threshold is not a knob).
 */
#define	KWQ_WAKE_DEADLINE	2	/* wake_delay_us passed */
#define	KWQ_WAKE_URGENT		4	/* an urgent ring or nudge (a notifier, a
					   drain, a cancel, kwq_enqueue_urgent()),
					   an enqueue from another CPU, the share
					   limit's deadline, or the unload */
#define	KWQ_WAKE_SELF		5	/* the worker got the CPU by itself */

/* kwq:::yield reasons */
#define	KWQ_YIELD_ROUND	0
#define	KWQ_YIELD_TICK	1
#define	KWQ_YIELD_CAP	2	/* the share limit's hand-over, S6.3 */
#define	KWQ_YIELD_SLICE	3	/* ULE ended the slice (limit off), S6.3 */

/* kwq:::cap events */
#define	KWQ_CAP_PASS	0	/* a pass begins under the limit (budget in arg5) */
#define	KWQ_CAP_DEMOTE	1	/* allowance spent, first hand-over: deadline armed */
#define	KWQ_CAP_YIELD	2	/* hand-over, same period */
#define	KWQ_CAP_RESTORE	3	/* the period ended while over */
#define	KWQ_CAP_DEADLINE 4	/* the deadline callout ran */

#endif /* !_KWQ_SDT_H_ */
