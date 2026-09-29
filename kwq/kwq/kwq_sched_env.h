/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Kernel environment for kwq_sched.c: the scheduler core operates on the
 * structs of kwq_internal.h.  The simulator provides its own
 * kwq_sched_env.h with the same field names (../sim/kwq_sched_env.h).
 */

#ifndef _KWQ_SCHED_ENV_H_
#define	_KWQ_SCHED_ENV_H_

#include "kwq_internal.h"
#include "kwq_sdt.h"

#define	KSQ_WEIGHT(kc)	((kc)->kc_q->kwq_weight)
#define	KS_ASSERT(e, msg)	KASSERT(e, msg)
#define	KS_HOOK_PARK(kw, kc)						\
	SDT_PROBE3(kwq, , , park, (kc)->kc_q, (kc)->kc_cpu, (kc)->kc_deficit)
#define	KS_HOOK_OVERRUN(kw, kc, over)					\
	SDT_PROBE3(kwq, , , overrun, (kc)->kc_q, (kc)->kc_cpu, (over))

#endif /* !_KWQ_SCHED_ENV_H_ */
