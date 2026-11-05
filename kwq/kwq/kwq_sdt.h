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
SDT_PROBE_DECLARE(kwq, , , steal__wake);
SDT_PROBE_DECLARE(kwq, , , steal__miss);
SDT_PROBE_DECLARE(kwq, , , donor);
SDT_PROBE_DECLARE(kwq, , , place);
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

/* kwq:::steal arg4, kwq:::steal-miss arg1: how the batch moves (SCHED.md S16, S17) */
#define	KWQ_STEAL_SCAN		0	/* place=0: kwq_steal()'s scan of the reach */
#define	KWQ_STEAL_DONOR		1	/* place=2: an idle thief, from the nearest donor */
#define	KWQ_STEAL_MAILBOX	2	/* place=2: from the victim its wake named */
#define	KWQ_STEAL_CHAIN		3	/* place=2: from the victim of its last batch */
#define	KWQ_STEAL_PULL		4	/* place=2: a pull at a pass end */
#define	KWQ_STEAL_HANDOFF	5	/* place=1: the busy worker hands a batch off */

/* kwq:::steal-wake arg2: why a sleeping BULK worker is woken to steal */
#define	KWQ_SWAKE_ENQUEUE	0	/* an enqueue crossed steal_wake or a doubling */
#define	KWQ_SWAKE_HELPER	1	/* a pass took one item and left the rest */
#define	KWQ_SWAKE_DONOR		2	/* place=2: the CPU just became a donor */
#define	KWQ_SWAKE_WATCHDOG	3	/* place=2: the starvation watchdog flagged it */

/* kwq:::steal-miss arg3: why an attempt moved nothing */
#define	KWQ_SMISS_NONE		0	/* no victim: the scan found none, or no donor in reach */
#define	KWQ_SMISS_STALE		1	/* the word names no live queue (slot, id) */
#define	KWQ_SMISS_EMPTY		2	/* the victim's list is empty */
#define	KWQ_SMISS_LONE		3	/* one item, which the rule leaves to its worker */
#define	KWQ_SMISS_INACTIVE	4	/* the queue is draining */

/* kwq:::place arg3: how the CPU was found */
#define	KWQ_PICK_TODAY		0	/* place=0: kwq_pick_any() */
#define	KWQ_PICK_NEAR		1	/* place=2: an idle CPU of the near groups */
#define	KWQ_PICK_FAR		2	/* place=2: an idle CPU of a far group (the far probe) */
#define	KWQ_PICK_SELF		3	/* place=2: the producer's CPU, not a donor */
#define	KWQ_PICK_NONDONOR	4	/* place=2: the nearest CPU that is not a donor */
#define	KWQ_PICK_PROBE		5	/* place=2: the shallowest of the own list and four */
#define	KWQ_PICK_GOSSIP		16	/* place=1: 16 + kp_place()'s step (KP_STEP_*) */

#endif /* !_KWQ_SDT_H_ */
