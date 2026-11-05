/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Userspace environment for kwq_place.c: the block accesses go through
 * the simulator's cache-line model (placesim.c, or kp_test.c's plain
 * memory), which counts the transfers they cause for the CPU the
 * simulator is running at that moment.  The kernel's version is
 * ../../kwq/kwq_place_env.h.
 */

#ifndef _KWQ_PLACE_ENV_H_
#define	_KWQ_PLACE_ENV_H_

#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t	kp_env_load(const uint64_t *p);
void		kp_env_store(uint64_t *p, uint64_t v);
bool		kp_env_tryflag(u_int *f);
void		kp_env_clrflag(u_int *f);
void		kp_env_fail(const char *fmt, ...);

#define	KP_LOAD64(p)		kp_env_load(p)
#define	KP_STORE64(p, v)	kp_env_store((p), (v))
#define	KP_TRYFLAG(c)		kp_env_tryflag(&(c)->kx_flag)
#define	KP_CLRFLAG(c)		kp_env_clrflag(&(c)->kx_flag)
#define	KP_ASSERT(e, msg)	do { if (!(e)) kp_env_fail msg; } while (0)

#endif /* !_KWQ_PLACE_ENV_H_ */
