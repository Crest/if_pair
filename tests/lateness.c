/*
 * lateness: a 1 kHz timer victim.  Sleeps 1 ms at a time for the given
 * number of seconds and reports how late each wake-up was against the
 * schedule, as the average, 99th percentile and maximum in microseconds.
 * Pinned to a CPU with cpuset(1) it shows what a busy kernel thread on
 * that CPU does to a user timer.  Used by tests/t_30_wakeups.sh; built
 * on the fly with cc -O2.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define	PERIOD_NS	1000000L

static int
cmp(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return (x < y ? -1 : x > y);
}

int
main(int argc, char **argv)
{
	struct timespec next, now;
	long *late, max = 0, sum = 0;
	size_t n = 0, cap;
	int secs = argc > 1 ? atoi(argv[1]) : 5;

	cap = (size_t)secs * 1000 + 1000;
	late = calloc(cap, sizeof(*late));
	if (late == NULL)
		return (1);
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;) {
		next.tv_nsec += PERIOD_NS;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
		long d = (now.tv_sec - next.tv_sec) * 1000000L +
		    (now.tv_nsec - next.tv_nsec) / 1000;
		if (d < 0)
			d = 0;
		if (n < cap)
			late[n++] = d;
		sum += d;
		if (d > max)
			max = d;
		if (n >= (size_t)secs * 1000)
			break;
	}
	qsort(late, n, sizeof(*late), cmp);
	printf("%zu %ld %ld %ld\n", n, n ? sum / (long)n : 0,
	    late[n * 99 / 100], max);	/* count avg p99 max, microseconds */
	return (0);
}
