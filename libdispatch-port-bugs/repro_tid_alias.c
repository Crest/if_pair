/*
 * Reproducer: FreeBSD libdispatch port encodes the raw thread ID as the
 * lock owner, but the owner field masks the two low bits, so threads with
 * IDs in the same aligned block of four alias each other.  Any two such
 * threads contending on dispatch_sync() for one serial queue trip the
 * "already owned by current thread" deadlock check.
 */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define NTHREADS 8
#define ITERS 20000

static dispatch_queue_t q;

static void
work(void *ctx)
{
	(void)ctx;
	for (volatile int i = 0; i < 2000; i++)
		;
}

static void *
thr(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int tid = pthread_getthreadid_np();

	printf("thread %d: tid %d -> lock owner value %d\n", idx, tid,
	    tid & ~3);
	fflush(stdout);
	for (int i = 0; i < ITERS; i++)
		dispatch_sync_f(q, NULL, work);
	return (NULL);
}

int
main(void)
{
	pthread_t t[NTHREADS];

	q = dispatch_queue_create("repro.serial", DISPATCH_QUEUE_SERIAL);
	for (int i = 0; i < NTHREADS; i++)
		pthread_create(&t[i], NULL, thr, (void *)(intptr_t)i);
	for (int i = 0; i < NTHREADS; i++)
		pthread_join(t[i], NULL);
	printf("completed without crash\n");
	return (0);
}
