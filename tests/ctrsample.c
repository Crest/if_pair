/*
 * ctrsample - sample sysctl counters with timestamps, hooking nothing: every
 * INTERVAL_US read each named sysctl (an integer of 4 or 8 bytes, or an
 * array of longs such as kern.cp_times) and print one line with the time
 * since start.  For WAKE.md S12.2: the worker's kwq counters and every CPU's
 * cp_times on the same 100 ms grid as iperf3's intervals.
 *
 *	ctrsample INTERVAL_US SECONDS NAME...
 *
 * The first line is "# t NAME..." (an array prints as NAME[0..n-1]); then
 * "seconds value..." per sample, arrays comma-separated.  With CTR_CMD and
 * CTR_EVERY (N) in the environment, every Nth sample also runs CTR_CMD with
 * sh -c and prints each line of its output as "@ seconds line", on the same
 * clock (e.g. netstat -x of the flow's sockets).
 */
#include <sys/param.h>
#include <sys/sysctl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double
since(const struct timespec *t0)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return ((t.tv_sec - t0->tv_sec) + (t.tv_nsec - t0->tv_nsec) / 1e9);
}

static void
print_value(const char *name)
{
	union { int32_t i; int64_t l; } one;
	long arr[4096];
	size_t len, i;

	len = sizeof(arr);
	if (sysctlbyname(name, arr, &len, NULL, 0) == -1) {
		printf(" -");
		return;
	}
	if (len == sizeof(int32_t)) {
		memcpy(&one.i, arr, sizeof(one.i));
		printf(" %d", one.i);
	} else if (len == sizeof(int64_t)) {
		memcpy(&one.l, arr, sizeof(one.l));
		printf(" %jd", (intmax_t)one.l);
	} else {
		for (i = 0; i < len / sizeof(long); i++)
			printf("%c%ld", i == 0 ? ' ' : ',', arr[i]);
	}
}

int
main(int argc, char **argv)
{
	struct timespec t0;
	double el, secs;
	const char *cmd;
	char line[1024];
	FILE *fp;
	long n = 0;
	int a, every, us;

	if (argc < 4) {
		fprintf(stderr, "usage: ctrsample INTERVAL_US SECONDS NAME...\n");
		return (2);
	}
	us = atoi(argv[1]);
	secs = atof(argv[2]);
	cmd = getenv("CTR_CMD");
	every = getenv("CTR_EVERY") != NULL ? atoi(getenv("CTR_EVERY")) : 0;
	printf("# t");
	for (a = 3; a < argc; a++)
		printf(" %s", argv[a]);
	printf("\n");
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while ((el = since(&t0)) < secs) {
		printf("%.4f", el);
		for (a = 3; a < argc; a++)
			print_value(argv[a]);
		printf("\n");
		if (cmd != NULL && every > 0 && n++ % every == 0 &&
		    (fp = popen(cmd, "r")) != NULL) {
			while (fgets(line, sizeof(line), fp) != NULL)
				printf("@ %.4f %s", el, line);
			pclose(fp);
		}
		usleep(us);
	}
	return (0);
}
