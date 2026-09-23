/*
 * Control for repro_tid_alias: the same workload, but only threads whose IDs
 * fall in DISTINCT aligned blocks of four take part.  FreeBSD hands out
 * thread IDs sequentially, so candidates are created one at a time and those
 * colliding with an accepted block exit at once.  With no aliasing the
 * misfiring deadlock check has nothing to trip on, so this must complete.
 *
 * Each candidate reports its decision through a semaphore before main()
 * looks at it, so the accept/reject bookkeeping is race-free.
 */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread_np.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdint.h>

#define NWORKERS 8
#define ITERS 20000
#define MAXTRIES 200

static dispatch_queue_t q;
static sem_t decided;			/* posted by each candidate once */
static int used_blocks[NWORKERS], nused;
static int last_accepted;		/* decision of the newest candidate */

static void work(void *ctx) { (void)ctx; for (volatile int i = 0; i < 2000; i++) ; }

/* Start routine of every candidate thread (passed to pthread_create). */
static void *
candidate(void *arg)
{
	int tid = pthread_getthreadid_np(), blk = tid & ~3, accepted = 1;

	(void)arg;
	for (int i = 0; i < nused; i++)		/* main() is blocked in sem_wait: no race */
		if (used_blocks[i] == blk) accepted = 0;
	if (accepted) used_blocks[nused++] = blk;
	last_accepted = accepted;
	sem_post(&decided);
	if (!accepted) return (NULL);

	printf("worker tid %d -> lock owner value %d\n", tid, blk);
	fflush(stdout);
	for (int i = 0; i < ITERS; i++)
		dispatch_sync_f(q, NULL, work);
	return (NULL);
}

int
main(void)
{
	pthread_t workers[NWORKERS], t;
	int nw = 0, created = 0;

	q = dispatch_queue_create("ctl.serial", DISPATCH_QUEUE_SERIAL);
	sem_init(&decided, 0, 0);
	while (nw < NWORKERS && created < MAXTRIES) {
		pthread_create(&t, NULL, candidate, NULL);
		created++;
		sem_wait(&decided);		/* wait for this candidate's verdict */
		if (last_accepted)
			workers[nw++] = t;	/* runs the workload; joined below */
		else
			pthread_join(t, NULL);	/* rejected: already exiting */
	}
	for (int i = 0; i < nw; i++)
		pthread_join(workers[i], NULL);
	printf("completed without crash (%d workers, %d threads created)\n", nw, created);
	return (nw == NWORKERS ? 0 : 2);
}
