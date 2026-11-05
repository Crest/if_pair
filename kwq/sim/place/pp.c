/*
 * pp.c - P6g.1 calibration (SPEC-p6g.md S3.8): the cost of moving a
 * written cache line between two pinned threads.  The threads take turns
 * writing a counter in one 128-byte-aligned line; each waits for the
 * other's value, so every step is one transfer of a modified line.  A
 * second test has one thread write a line and the other read it after
 * each write (the status word's case: one writer, one reader).
 *
 *   pp CPU_A CPU_B [ITERS]     prints: a b pingpong_ns_oneway read_after_write_ns
 */
#include <sys/param.h>
#include <sys/cpuset.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static _Alignas(128) _Atomic long line;
static _Alignas(128) _Atomic long ack;
static int cpus[2];
static long iters = 2000000;

static void
pin(int c)
{
	cpuset_t s;

	CPU_ZERO(&s);
	CPU_SET(c, &s);
	if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s) != 0) {
		perror("pthread_setaffinity_np");
		exit(1);
	}
}

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1e9 + ts.tv_nsec);
}

static void *
pong(void *arg)
{
	long i;

	(void)arg;
	pin(cpus[1]);
	for (i = 1; i <= iters; i++) {
		while (atomic_load_explicit(&line, memory_order_acquire) != 2 * i - 1)
			;
		atomic_store_explicit(&line, 2 * i, memory_order_release);
	}
	/* Read-after-write: read `line' once per value the writer publishes. */
	for (i = 1; i <= iters; i++) {
		while (atomic_load_explicit(&line, memory_order_acquire) != -i)
			;
		atomic_store_explicit(&ack, i, memory_order_release);
	}
	return (NULL);
}

int
main(int argc, char **argv)
{
	pthread_t t;
	double t0, t1, t2;
	long i;

	if (argc < 3)
		return (2);
	cpus[0] = atoi(argv[1]);
	cpus[1] = atoi(argv[2]);
	if (argc > 3)
		iters = atol(argv[3]);
	pin(cpus[0]);
	pthread_create(&t, NULL, pong, NULL);
	t0 = now();
	for (i = 1; i <= iters; i++) {
		atomic_store_explicit(&line, 2 * i - 1, memory_order_release);
		while (atomic_load_explicit(&line, memory_order_acquire) != 2 * i)
			;
	}
	t1 = now();
	for (i = 1; i <= iters; i++) {
		atomic_store_explicit(&line, -i, memory_order_release);
		while (atomic_load_explicit(&ack, memory_order_acquire) != i)
			;
	}
	t2 = now();
	pthread_join(t, NULL);
	/* Ping-pong: two transfers per step.  Read-after-write: the writer's
	 * store reaches the reader and the ack comes back: two transfers too,
	 * of two lines. */
	printf("%d %d %.1f %.1f\n", cpus[0], cpus[1], (t1 - t0) / iters / 2,
	    (t2 - t1) / iters / 2);
	return (0);
}
