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

#define	KSQ_WEIGHT(kc)	((kc)->kc_q->kwq_weight)

#endif /* !_KWQ_SCHED_ENV_H_ */
