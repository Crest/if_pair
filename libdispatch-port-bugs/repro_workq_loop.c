/*
 * Standalone harness for the inner loop of the FreeBSD
 * _dispatch_workq_count_runnable_workers() in the port's
 * files/patch-src_event_workqueue.c, versus the corrected loop.
 */
#include <stdio.h>

#define SRUN 2
#define SSLEEP 3
struct kp { unsigned ki_tid; int ki_stat; };

static int
count_port(unsigned *reg, int nreg, struct kp *kp, int count)
{
	int runners = 0;
	for (int i = 0; i < nreg; ++i) {
		unsigned tid = reg[i];
		for (int j = 0; i < count; ++i) {		/* as shipped */
			if (kp[j].ki_tid != tid) { continue; }
			if (kp[j].ki_stat == SRUN) { ++runners; break; }
		}
	}
	return (runners);
}

static int
count_fixed(unsigned *reg, int nreg, struct kp *kp, int count)
{
	int runners = 0;
	for (int i = 0; i < nreg; ++i) {
		unsigned tid = reg[i];
		for (int j = 0; j < count; ++j) {		/* corrected */
			if (kp[j].ki_tid != tid) { continue; }
			if (kp[j].ki_stat == SRUN) { ++runners; break; }
		}
	}
	return (runners);
}

int
main(void)
{
	/* kinfo_proc as the sysctl returns it: main thread first, then workers */
	struct kp kp[] = {
		{ 100001, SSLEEP },	/* main thread, blocked in join */
		{ 100002, SRUN },	/* worker */
		{ 100003, SRUN },	/* worker */
		{ 100004, SRUN },	/* worker */
	};
	unsigned reg[] = { 100002, 100003, 100004 };	/* registered workers */
	int count = sizeof(kp) / sizeof(kp[0]);
	int nreg = sizeof(reg) / sizeof(reg[0]);

	printf("registered workers: %d, all runnable\n", nreg);
	printf("port loop  counts %d runnable\n", count_port(reg, nreg, kp, count));
	printf("fixed loop counts %d runnable\n", count_fixed(reg, nreg, kp, count));
	return (0);
}
