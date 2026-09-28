/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Userspace environment for kwq_sched.c: the two structs the scheduler
 * core operates on, with exactly the fields kwq_sched.h lists, plus the
 * simulator's own bookkeeping.  The kernel's kwq_internal.h provides the
 * same names inside its cache-line-laid-out structs (P1b).
 */

#ifndef _KWQ_SCHED_ENV_H_
#define	_KWQ_SCHED_ENV_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct kwq_sched_knobs;
struct simq;

struct kwq_cpu {
	TAILQ_ENTRY(kwq_cpu)	kc_active;
	u_int			kc_state;
	u_int			kc_onlist;
	u_int			kc_warm;
	u_int			kc_idle_round;
	u_int			kc_src;
	int64_t			kc_deficit;
	uint64_t		kc_glitches;
	uint64_t		kc_passes, kc_overruns, kc_parks, kc_boosts,
				kc_grace;
	u_int			kc_weight;
	struct simq		*kc_sq;		/* simulator back pointer */
};

struct kwq_worker {
	TAILQ_HEAD(, kwq_cpu)	kw_new;
	TAILQ_HEAD(, kwq_cpu)	kw_active;
	u_int			kw_nactive;
	u_int			kw_nnew;
	uint64_t		kw_round;
	u_int			kw_phase;
	int			kw_ring_left;
	int			kw_tail_left;
	bool			kw_want_new;
	u_int			kw_served;
	struct kwq_cpu		*kw_cur;
	uint64_t		kw_pass_start;
	int64_t			kw_pass_budget;
	uint64_t		kw_win_start;
	uint64_t		kw_win_busy;
	const struct kwq_sched_knobs *kw_knobs;
	uint64_t		kw_rounds, kw_passes, kw_yields, kw_cap_sleeps,
				kw_handbacks, kw_busy_ns;
};

#define	KSQ_WEIGHT(kc)	((kc)->kc_weight)

/* Core self-checks land in the simulator's violation count. */
void	ks_env_fail(const char *fmt, ...);
#define	KS_ASSERT(e, msg)	do { if (!(e)) ks_env_fail msg; } while (0)

#endif /* !_KWQ_SCHED_ENV_H_ */
