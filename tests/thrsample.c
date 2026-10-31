/*
 * thrsample - sample the threads of some processes without hooking the
 * scheduler: every INTERVAL_US read each thread's state, wait message or
 * the lock it blocks on, and its CPU through sysctl(KERN_PROC), and print
 * one line per thread with the time since start.  For WAKE.md S12.2, where
 * probes in sched_switch() (DTrace, lockstat, process-mode hwpmc) changed
 * the very behaviour they measured.
 *
 *	thrsample INTERVAL_US SECONDS PID...
 *
 * Lines: "seconds pid tid state where cpu", state R (on a CPU), Q (runnable,
 * waiting for one), S (sleeping), L (blocked on a lock), - (other); where is
 * the wait message, the lock name, or "-".
 */
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double
since(const struct timespec *t0)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return ((t.tv_sec - t0->tv_sec) + (t.tv_nsec - t0->tv_nsec) / 1e9);
}

int
main(int argc, char **argv)
{
	struct timespec t0;
	struct kinfo_proc *kp = NULL;
	size_t cap = 0, len;
	double el, secs;
	int a, i, n, us;

	if (argc < 4) {
		fprintf(stderr, "usage: thrsample INTERVAL_US SECONDS PID...\n");
		return (2);
	}
	us = atoi(argv[1]);
	secs = atof(argv[2]);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while ((el = since(&t0)) < secs) {
		for (a = 3; a < argc; a++) {
			int mib[4] = { CTL_KERN, KERN_PROC,
			    KERN_PROC_PID | KERN_PROC_INC_THREAD, atoi(argv[a]) };

			len = 0;
			if (sysctl(mib, 4, NULL, &len, NULL, 0) == -1)
				continue;
			len += 4 * sizeof(*kp);		/* threads may appear */
			if (len > cap) {
				free(kp);
				if ((kp = malloc(len)) == NULL)
					return (1);
				cap = len;
			}
			if (sysctl(mib, 4, kp, &len, NULL, 0) == -1)
				continue;
			n = len / sizeof(*kp);
			for (i = 0; i < n; i++) {
				const char *w = "-";
				char st = '-';

				switch (kp[i].ki_stat) {
				case SRUN:
					st = kp[i].ki_oncpu != NOCPU ? 'R' : 'Q';
					break;
				case SSLEEP:
					st = 'S';
					break;
				case SLOCK:
					st = 'L';
					break;
				}
				if (kp[i].ki_kiflag & KI_LOCKBLOCK)
					w = kp[i].ki_lockname;
				else if (kp[i].ki_wmesg[0] != '\0')
					w = kp[i].ki_wmesg;
				printf("%.4f %s %d %c %s %d\n", el, argv[a],
				    kp[i].ki_tid, st, w, st == 'R' ?
				    kp[i].ki_oncpu : kp[i].ki_lastcpu);
			}
		}
		usleep(us);
	}
	return (0);
}
