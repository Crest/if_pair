/*
 * The scheduler's clock: see kwq_clock.h.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <machine/cpu.h>
#if defined(__amd64__) || defined(__i386__)
#include <machine/clock.h>		/* tsc_freq, tsc_is_invariant */
#elif defined(__aarch64__)
#include <machine/armreg.h>
#endif

#include "kwq_clock.h"

bool kwq_clock_fast = false;
uint64_t kwq_clock_hz;
const char *kwq_clock_name = "cpu_ticks";
static int kwq_clock_allow_fast = 1;	/* kern.kwq.clock_fast, a loader tunable */

SYSCTL_DECL(_kern_kwq);

static int
kwq_sysctl_clock(SYSCTL_HANDLER_ARGS)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%s %ju Hz", kwq_clock_name,
	    (uintmax_t)kwq_tickrate());
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}
SYSCTL_PROC(_kern_kwq, OID_AUTO, clock, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
    NULL, 0, kwq_sysctl_clock, "A",
    "the scheduler's tick source and its rate (tsc, cntvct, timebase, or the kernel's cpu_ticks)");
SYSCTL_INT(_kern_kwq, OID_AUTO, clock_fast, CTLFLAG_RDTUN, &kwq_clock_allow_fast, 0,
    "read the architecture's counter directly where it is fixed-rate (default 1); 0 keeps the kernel's cpu_ticks().  Loader tunable: the choice is made when the module loads");

/*
 * Enable get_cyclecount() where it is a fixed-rate counter whose ticks are
 * the kernel's cpu_ticks() units: the worker mixes the two in its CPU-time
 * accounting, so the rates must agree (within 1 %, for a frequency the
 * kernel measured rather than read).
 */
void
kwq_clock_init(void)
{
	uint64_t hz = 0, kern;
	const char *name = NULL;

	TUNABLE_INT_FETCH("kern.kwq.clock_fast", &kwq_clock_allow_fast);
	if (!kwq_clock_allow_fast)
		return;
#if defined(__amd64__) || defined(__i386__)
	if (tsc_is_invariant && tsc_freq != 0) {
		hz = tsc_freq;
		name = "tsc";
	}
#elif defined(__aarch64__)
	hz = READ_SPECIALREG(cntfrq_el0);
	name = "cntvct";
#elif defined(__powerpc__)
	hz = cpu_tickrate();		/* cpu_ticks() is the timebase already */
	name = "timebase";
#endif
	if (hz == 0 || name == NULL)
		return;
	kern = cpu_tickrate();
	if (kern == 0 || (hz > kern ? hz - kern : kern - hz) * 100 > kern) {
		printf("kwq: %s at %ju Hz does not match cpu_ticks() at %ju Hz, "
		    "using cpu_ticks()\n", name, (uintmax_t)hz, (uintmax_t)kern);
		return;
	}
	kwq_clock_hz = hz;
	kwq_clock_name = name;
	kwq_clock_fast = true;
}
