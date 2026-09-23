/* Contended dispatch_once(): the loser waits via _dispatch_wait_on_address(). */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static dispatch_once_t pred;
static void init(void *ctx) { (void)ctx; sleep(1); }
static void *thr(void *arg) { (void)arg; dispatch_once_f(&pred, NULL, init); return (NULL); }

int
main(void)
{
	pthread_t t[4];
	for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, thr, NULL);
	for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
	printf("completed without crash\n");
	return (0);
}
