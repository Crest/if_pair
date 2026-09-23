/*
 * Minimal reproducer for devel/libdispatch on FreeBSD: two threads whose
 * kernel thread IDs share an aligned block of four are treated as the SAME
 * lock owner (the port does not shift the ID clear of the two lock flag
 * bits), so a legal dispatch_sync() by the second thread trips libdispatch's
 * "already owned by current thread" deadlock check and the process dies
 * with SIGILL.  Expected behaviour: the waiter blocks ~400 ms, then runs.
 *
 *   cc -O1 -fuse-ld=lld repro_min.c -I/usr/local/include -L/usr/local/lib \
 *      -ldispatch -lpthread -o repro_min && ./repro_min
 */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread_np.h>
#include <semaphore.h>
#include <stdio.h>
#include <unistd.h>

#define MAXT 16
enum { EXIT, HOLDER, WAITER };
struct th { pthread_t t; int tid; int role; sem_t go; } T[MAXT];
static sem_t ready;
static dispatch_queue_t q;

static void hold(void *ctx) { (void)ctx; usleep(500000); }
static void noop(void *ctx) { (void)ctx; }

static void *
worker(void *arg)
{
	struct th *me = arg;

	me->tid = pthread_getthreadid_np();
	sem_post(&ready);
	sem_wait(&me->go);
	if (me->role == HOLDER) {
		dispatch_sync_f(q, NULL, hold);
	} else if (me->role == WAITER) {
		usleep(100000);			/* let the holder acquire first */
		dispatch_sync_f(q, NULL, noop);
		printf("waiter tid %d ran after the holder released the queue: OK\n",
		    me->tid);
	}
	return (NULL);
}

int
main(void)
{
	int n, i, a = -1, b = -1;

	q = dispatch_queue_create("repro.serial", DISPATCH_QUEUE_SERIAL);
	sem_init(&ready, 0, 0);
	/* FreeBSD hands out thread IDs sequentially: a few threads suffice. */
	for (n = 0; n < MAXT && a < 0; n++) {
		sem_init(&T[n].go, 0, 0);
		pthread_create(&T[n].t, NULL, worker, &T[n]);
		sem_wait(&ready);
		for (i = 0; i < n; i++)
			if ((T[i].tid & ~3) == (T[n].tid & ~3)) { a = i; b = n; }
	}
	if (a < 0) { puts("no two threads share an ID block; rerun"); return (2); }
	printf("holder tid %d and waiter tid %d both map to lock owner %d\n",
	    T[a].tid, T[b].tid, T[a].tid & ~3);
	T[a].role = HOLDER; T[b].role = WAITER;
	sem_post(&T[a].go);
	usleep(20000);
	sem_post(&T[b].go);
	for (i = 0; i < n; i++)
		if (i != a && i != b) sem_post(&T[i].go);
	for (i = 0; i < n; i++)
		pthread_join(T[i].t, NULL);
	puts("completed");
	return (0);
}
