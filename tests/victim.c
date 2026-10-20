/*
 * victim: an ordinary timeshare process standing in for interactive use
 * on a CPU that a kwq worker saturates.  Pin it there with cpuset(1); it
 * takes no scheduling privilege of its own, so what it measures is what
 * a user's shell, editor or build gets.  Used by tests/t_34_interactive.sh;
 * built on the fly with cc -O2.  One line of key=value output per run,
 * times in microseconds.
 *
 *   victim timer SECS [WORK_US]
 *       A 1 kHz timer on an absolute schedule.  Per wake it reports how
 *       late the wake-up was (late_*) and, with WORK_US > 0, how long it
 *       took from the scheduled time until WORK_US of its own CPU time
 *       had been consumed (turn_*: the turnaround of a keystroke's worth
 *       of work).  A wake-up more than ten periods late resets the
 *       schedule and counts a catchup, so a stall shows as one maximum
 *       rather than as a thousand late wake-ups.
 *   victim spin SECS
 *       A CPU-bound loop for SECS of wall time; share is its CPU time
 *       over the wall time, from getrusage(2): what a compile job would
 *       get on that CPU.
 *   victim exec COUNT [GAP_MS]
 *       fork(2) + execve(2) of /usr/bin/true, COUNT times, GAP_MS apart
 *       (default 20): the wall time of each, as a shell spawning commands.
 */
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	PERIOD_NS	1000000L

static int
cmp(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return (x < y ? -1 : x > y);
}

static long
ts_sub_us(const struct timespec *a, const struct timespec *b)
{
	return ((a->tv_sec - b->tv_sec) * 1000000L +
	    (a->tv_nsec - b->tv_nsec) / 1000);
}

static void
ts_add_ns(struct timespec *t, long ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000L) {
		t->tv_nsec -= 1000000000L;
		t->tv_sec++;
	}
}

/* Burn `us' of this thread's own CPU time, however long that takes. */
static void
work(long us)
{
	struct timespec t0, t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
	do {
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	} while (ts_sub_us(&t, &t0) < us);
}

static void
stats(const char *pfx, long *v, size_t n)
{
	long sum = 0;
	size_t i;

	for (i = 0; i < n; i++)
		sum += v[i];
	qsort(v, n, sizeof(*v), cmp);
	printf(" %s_avg=%ld %s_p99=%ld %s_max=%ld", pfx, n ? sum / (long)n : 0,
	    pfx, n ? v[n * 99 / 100] : 0, pfx, n ? v[n - 1] : 0);
}

static int
do_timer(int secs, long work_us)
{
	struct timespec next, now;
	long *late, *turn, d, catchups = 0;
	size_t n = 0, cap;

	cap = (size_t)secs * 1000 + 1000;
	late = calloc(cap, sizeof(*late));
	turn = calloc(cap, sizeof(*turn));
	if (late == NULL || turn == NULL)
		return (1);
	clock_gettime(CLOCK_MONOTONIC, &next);
	while (n < (size_t)secs * 1000) {
		ts_add_ns(&next, PERIOD_NS);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
		d = ts_sub_us(&now, &next);
		if (d < 0)
			d = 0;
		late[n] = d;
		if (work_us > 0) {
			work(work_us);
			clock_gettime(CLOCK_MONOTONIC, &now);
			turn[n] = ts_sub_us(&now, &next);
		}
		n++;
		if (d > 10 * (PERIOD_NS / 1000)) {
			catchups++;
			next = now;
		}
	}
	printf("timer n=%zu", n);
	stats("late", late, n);
	if (work_us > 0)
		stats("turn", turn, n);
	printf(" catchups=%ld\n", catchups);
	return (0);
}

static int
do_spin(int secs)
{
	struct timespec t0, t;
	struct rusage ru;
	double wall, cpu;
	volatile unsigned long x = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	do {
		for (int i = 0; i < 100000; i++)
			x += i;
		clock_gettime(CLOCK_MONOTONIC, &t);
	} while (ts_sub_us(&t, &t0) < secs * 1000000L);
	wall = (double)ts_sub_us(&t, &t0) / 1e6;
	getrusage(RUSAGE_SELF, &ru);
	cpu = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec +
	    (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
	printf("spin wall=%.3f cpu=%.3f share=%.4f\n", wall, cpu,
	    wall > 0 ? cpu / wall : 0);
	return (0);
}

static int
do_exec(int count, long gap_ms)
{
	struct timespec t0, t, gap = { 0, gap_ms * 1000000L };
	long *dur;
	int i, status;
	pid_t pid;
	char *const argv[] = { "true", NULL };

	dur = calloc((size_t)count, sizeof(*dur));
	if (dur == NULL)
		return (1);
	for (i = 0; i < count; i++) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		pid = fork();
		if (pid == 0) {
			execv("/usr/bin/true", argv);
			_exit(127);
		}
		if (pid < 0 || waitpid(pid, &status, 0) < 0)
			return (1);
		clock_gettime(CLOCK_MONOTONIC, &t);
		dur[i] = ts_sub_us(&t, &t0);
		nanosleep(&gap, NULL);
	}
	printf("exec n=%d", count);
	stats("exec", dur, (size_t)count);
	printf("\n");
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "timer") == 0)
		return (do_timer(atoi(argv[2]), argc > 3 ? atol(argv[3]) : 0));
	if (argc >= 3 && strcmp(argv[1], "spin") == 0)
		return (do_spin(atoi(argv[2])));
	if (argc >= 3 && strcmp(argv[1], "exec") == 0)
		return (do_exec(atoi(argv[2]), argc > 3 ? atol(argv[3]) : 20));
	fprintf(stderr, "usage: victim timer SECS [WORK_US] | spin SECS | "
	    "exec COUNT [GAP_MS]\n");
	return (2);
}
