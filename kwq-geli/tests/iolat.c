/*
 * iolat: the load of geli_bench.sh's scale, ssd, deep, deepcpu, mixed
 * and dev tests.  DEPTH
 * threads per device issue BYTES-sized pread(2)s or pwrite(2)s on the
 * raw device for SECS, each waiting for its own: a queue depth of
 * DEPTH per device.  Sequential offsets are shared by a device's
 * threads, so a depth above 1 is one stream with that many requests in
 * flight; -r picks every offset at random, aligned to BYTES.  Each
 * request's latency goes into a log-linear histogram (32 steps per
 * power of two, 3 %).  One line of key=value output: requests, MB/s
 * (1e6 bytes) and requests/s in total, the slowest and fastest device's
 * MB/s and Jain's index over the devices, and the latency's median,
 * 90th, 99th and 99.9th percentile and maximum, in microseconds.
 *
 *   iolat [-w] [-r] [-b BYTES] [-q DEPTH] [-t SECS] DEVICE ...
 *
 * Built on the fly with cc -O2 -lpthread -lm.
 */
#include <sys/types.h>
#include <sys/disk.h>
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	SUB	5			/* 2^SUB steps per power of two */
#define	NBUCKET	((64 - SUB + 1) << SUB)

struct dev {
	const char	*name;
	int		 fd;
	off_t		 size;
	_Atomic uint64_t next;		/* the sequential stream's next offset */
	uint64_t	 bytes;
};

struct worker {
	pthread_t	 td;
	struct dev	*dev;
	uint64_t	 rnd;
	uint64_t	 ops, bytes;
	uint64_t	 hist[NBUCKET];
	char		*buf;
	int		 err;
};

static int wflag, rflag;
static size_t bs = 131072;
static int depth = 1;
static double secs = 10;
static pthread_barrier_t start;
static uint64_t t_start, t_end;

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
}

static int
bucket(uint64_t v)
{
	int e;

	if (v < (2u << SUB))
		return ((int)v);
	e = 63 - __builtin_clzll(v) - SUB;
	return ((e << SUB) + (int)(v >> e));
}

static double
bucket_value(int i)	/* the middle of bucket i, ns */
{
	int e;

	if (i < (2 << SUB))
		return (i);
	e = (i >> SUB) - 1;
	return ((((uint64_t)(i - (e << SUB))) << e) + ((1ull << e) - 1) / 2.0);
}

static uint64_t
xorshift(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return (*s);
}

static void *
run(void *arg)
{
	struct worker *w = arg;
	struct dev *d = w->dev;
	uint64_t nreq = d->size / bs, t0, t1;
	off_t off;
	ssize_t n;

	pthread_barrier_wait(&start);
	for (;;) {
		if (rflag)
			off = (off_t)(xorshift(&w->rnd) % nreq) * bs;
		else
			off = (off_t)(atomic_fetch_add(&d->next, 1) % nreq) * bs;
		t0 = now_ns();
		if (t0 >= t_end)
			break;
		n = wflag ? pwrite(d->fd, w->buf, bs, off) :
		    pread(d->fd, w->buf, bs, off);
		t1 = now_ns();
		if (n != (ssize_t)bs) {
			w->err = n < 0 ? errno : EIO;
			break;
		}
		w->ops++;
		w->bytes += bs;
		w->hist[bucket(t1 - t0)]++;
	}
	return (NULL);
}

static double
pct(const uint64_t *h, uint64_t total, double p)
{
	uint64_t want = (uint64_t)ceil(total * p), sum = 0;
	int i;

	for (i = 0; i < NBUCKET; i++) {
		sum += h[i];
		if (sum >= want && h[i] > 0)
			return (bucket_value(i) / 1000);
	}
	return (0);
}

int
main(int argc, char **argv)
{
	struct dev *devs;
	struct worker *ws;
	uint64_t hist[NBUCKET], ops = 0, bytes = 0, maxb = 0;
	double el, r, s = 0, ss = 0, mn = -1, mx = 0;
	int c, i, j, nd, nw, bad = 0;

	while ((c = getopt(argc, argv, "wrb:q:t:")) != -1) {
		switch (c) {
		case 'w': wflag = 1; break;
		case 'r': rflag = 1; break;
		case 'b': bs = strtoul(optarg, NULL, 0); break;
		case 'q': depth = atoi(optarg); break;
		case 't': secs = atof(optarg); break;
		default:
			fprintf(stderr, "usage: iolat [-w] [-r] [-b BYTES] [-q DEPTH] [-t SECS] DEVICE ...\n");
			return (2);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 || bs < 512 || bs % 512 != 0 || depth < 1 || secs <= 0)
		errx(2, "usage: iolat [-w] [-r] [-b BYTES] [-q DEPTH] [-t SECS] DEVICE ...");
	nd = argc;
	nw = nd * depth;
	if ((devs = calloc(nd, sizeof(*devs))) == NULL ||
	    (ws = calloc(nw, sizeof(*ws))) == NULL)
		err(1, "calloc");
	for (i = 0; i < nd; i++) {
		devs[i].name = argv[i];
		if ((devs[i].fd = open(argv[i], wflag ? O_RDWR : O_RDONLY)) < 0)
			err(1, "%s", argv[i]);
		if (ioctl(devs[i].fd, DIOCGMEDIASIZE, &devs[i].size) < 0)
			err(1, "%s: DIOCGMEDIASIZE", argv[i]);
		if (devs[i].size < (off_t)bs)
			errx(1, "%s: smaller than one request", argv[i]);
	}
	pthread_barrier_init(&start, NULL, nw + 1);
	for (i = 0; i < nw; i++) {
		ws[i].dev = &devs[i / depth];
		ws[i].rnd = 0x9e3779b97f4a7c15ull * (i + 1);
		if ((ws[i].buf = aligned_alloc(4096, (bs + 4095) & ~(size_t)4095)) == NULL)
			err(1, "aligned_alloc");
		memset(ws[i].buf, 0xa5, bs);
		if ((errno = pthread_create(&ws[i].td, NULL, run, &ws[i])) != 0)
			err(1, "pthread_create");
	}
	t_start = now_ns();
	t_end = t_start + (uint64_t)(secs * 1e9);
	pthread_barrier_wait(&start);
	memset(hist, 0, sizeof(hist));
	for (i = 0; i < nw; i++) {
		pthread_join(ws[i].td, NULL);
		if (ws[i].err != 0) {
			warnc(ws[i].err, "%s", ws[i].dev->name);
			bad = 1;
		}
		ops += ws[i].ops;
		bytes += ws[i].bytes;
		ws[i].dev->bytes += ws[i].bytes;
		for (j = 0; j < NBUCKET; j++) {
			hist[j] += ws[i].hist[j];
			if (ws[i].hist[j] > 0 && (uint64_t)j > maxb)
				maxb = j;
		}
	}
	el = (now_ns() - t_start) / 1e9;
	for (i = 0; i < nd; i++) {
		r = devs[i].bytes / el / 1e6;
		s += r;
		ss += r * r;
		if (mn < 0 || r < mn)
			mn = r;
		if (r > mx)
			mx = r;
	}
	printf("ops=%ju mbps=%.1f iops=%.0f dev_min_mbps=%.1f dev_max_mbps=%.1f jain=%.4f "
	    "lat_p50_us=%.1f lat_p90_us=%.1f lat_p99_us=%.1f lat_p999_us=%.1f lat_max_us=%.1f\n",
	    (uintmax_t)ops, bytes / el / 1e6, ops / el, mn, mx,
	    ss > 0 ? s * s / (nd * ss) : 0,
	    pct(hist, ops, 0.5), pct(hist, ops, 0.9), pct(hist, ops, 0.99),
	    pct(hist, ops, 0.999), ops > 0 ? bucket_value((int)maxb) / 1000 : 0);
	return (bad);
}
