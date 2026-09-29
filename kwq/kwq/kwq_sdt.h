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
 * lists them; they simply never fire.
 */
#ifdef KWQ_NO_SDT
#undef SDT_PROBE1
#undef SDT_PROBE2
#undef SDT_PROBE3
#undef SDT_PROBE4
#undef SDT_PROBE5
#define	SDT_PROBE1(prov, mod, func, name, a0)			do { } while (0)
#define	SDT_PROBE2(prov, mod, func, name, a0, a1)		do { } while (0)
#define	SDT_PROBE3(prov, mod, func, name, a0, a1, a2)		do { } while (0)
#define	SDT_PROBE4(prov, mod, func, name, a0, a1, a2, a3)	do { } while (0)
#define	SDT_PROBE5(prov, mod, func, name, a0, a1, a2, a3, a4)	do { } while (0)
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
SDT_PROBE_DECLARE(kwq, , , idle);
SDT_PROBE_DECLARE(kwq, , , steal);
SDT_PROBE_DECLARE(kwq, , , worker__block);
SDT_PROBE_DECLARE(kwq, , , worker__spawn);
SDT_PROBE_DECLARE(kwq, , , worker__exit);
SDT_PROBE_DECLARE(kwq, , , budget__hit);

/* kwq:::yield reasons */
#define	KWQ_YIELD_ROUND	0
#define	KWQ_YIELD_TICK	1

#endif /* !_KWQ_SDT_H_ */
