/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Kernel environment for kwq_place.c (kwq_place.h): single-copy atomic
 * 64-bit accesses and the try-flag.  The simulator provides its own
 * kwq_place_env.h (../sim/place/kwq_place_env.h), which counts the cache
 * lines the same accesses move.
 */

#ifndef _KWQ_PLACE_ENV_H_
#define	_KWQ_PLACE_ENV_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <machine/atomic.h>

#define	KP_LOAD64(p)		atomic_load_64(p)
#define	KP_STORE64(p, v)	atomic_store_64((p), (v))
#define	KP_TRYFLAG(c)		(atomic_cmpset_acq_int(&(c)->kx_flag, 0, 1) != 0)
#define	KP_CLRFLAG(c)		atomic_store_rel_int(&(c)->kx_flag, 0)
#define	KP_ASSERT(e, msg)	KASSERT(e, msg)

#endif /* !_KWQ_PLACE_ENV_H_ */
