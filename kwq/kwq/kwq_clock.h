/*
 * The scheduler's clock (SCHED.md S15.2, PLAN P4b "clocks").
 *
 * kwq charges passes in CPU time and bounds them with a budget read in
 * kwq_budget_left(); both need a cheap monotonic per-CPU tick.  cpu_ticks()
 * is that on amd64 (the bare rdtsc) but on arm64 it is the generic
 * tc_cpu_ticks(): a critical section, an isb-fenced 32-bit timecounter
 * read and the per-CPU wrap extension that misfires on the Altra (S13), 22
 * ns against 9 for the counter itself (2026-10-20).  kwq_ticks() reads
 * get_cyclecount() where that is a fixed-rate 64-bit counter - the
 * invariant TSC, CNTVCT_EL0, the PowerPC timebase - and cpu_ticks()
 * elsewhere; kwq_tickrate() is its frequency.  The read is unfenced: an
 * instruction early by a pipeline's length is inside the budget estimate's
 * margin (S4.4), the worker is bound so no cross-CPU skew enters, and a
 * per-CPU read cannot go backwards, so the S13 glitch guard still holds.
 * Where the fast clock is used, its ticks are the same units as
 * cpu_ticks() (the kernel derives the latter from the same counter), which
 * kwq_clock_init() checks before enabling it; the worker's CPU-time
 * accounting mixes the two (kwq_worker.c, kwq_cputime_ns_tick()).
 */
#ifndef _KWQ_CLOCK_H_
#define	_KWQ_CLOCK_H_

#include <machine/cpu.h>

extern bool		kwq_clock_fast;		/* get_cyclecount() in use */
extern uint64_t		kwq_clock_hz;		/* its rate; cpu_tickrate() otherwise */
extern const char	*kwq_clock_name;	/* "tsc", "cntvct", "timebase", "cpu_ticks" */

static inline uint64_t
kwq_ticks(void)
{
	if (__predict_true(kwq_clock_fast))
		return (get_cyclecount());
	return (cpu_ticks());
}

static inline uint64_t
kwq_tickrate(void)
{
	if (__predict_true(kwq_clock_fast))
		return (kwq_clock_hz);
	return (cpu_tickrate());
}

void	kwq_clock_init(void);	/* module load, before the workers start */

#endif /* !_KWQ_CLOCK_H_ */
