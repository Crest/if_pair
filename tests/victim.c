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
 *   victim udpecho PORT
 *       A UDP echo server; runs in the far jail for the mode below.
 *   victim udp HOST PORT SECS [SPORT [GAP_US]]
 *       Request-reply over UDP from source port SPORT (0 = any), one
 *       64-byte datagram at a time, GAP_US apart (default 500): the
 *       round-trip time's median, 99th percentile and maximum, and the
 *       requests unanswered within 200 ms.  A fixed SPORT keeps the flow on
 *       one worker CPU across runs and drivers (the pair hashes the
 *       4-tuple), so the harness can put it on the CPU under test.
 *   victim ports SEED NCPU CPU SRC DST TCPPORT UDPPORT [COUNT]
 *       The pair's flow hash (if_pair.c pair_hash_mbuf(): jenkins_hash32
 *       over source, destination, protocol and the port pair, seeded per
 *       module load) replicated, so that the harness can choose ports
 *       offline: prints cports=N,N,... (COUNT of them, default 1) and
 *       uport=M, the lowest ports from 40000 up whose TCP flow SRC:N ->
 *       DST:TCPPORT and UDP flow SRC:M -> DST:UDPPORT land on CPU (hash %
 *       NCPU) in the direction from SRC: the data of the bulk flow and the
 *       requests of the UDP flow.  The reverse direction hashes on its
 *       own; demanding both on one CPU leaves too few ports on a 128-CPU
 *       machine.  A fresh TCP port per run keeps the previous run's
 *       TIME_WAIT entry out of the way.  SEED is pair_hash_seed read from
 *       the loaded module:
 *   victim seed MODULE
 *       The value of the static pair_hash_seed of the loaded module file
 *       (if_pair.ko or if_pair_tq.ko), through kldsym(2) and /dev/kmem.
 *   victim cpu SEED NCPU SRC DST PROTO SPORT DPORT
 *       The CPU one direction of a flow hashes to (PROTO 6 or 17).
 *   victim udpecho2 PORT SECS GAP_MS
 *       The UDP echo server that also, every GAP_MS, disconnects its own
 *       socket (connect(2) with AF_UNSPEC, which takes the pcb's write
 *       lock while the worker delivering to it holds the read lock) and
 *       reports the latency of those calls: the read-held rwlock test of
 *       PLAN P4d.
 *   victim tcpecho PORT
 *       Accepts TCP connections and closes them at once: the far end of
 *       the connection churn below.
 *   victim tcpconn HOST PORT SECS [GAP_MS]
 *       connect(2) and close(2) in a loop, GAP_MS apart (default 2): the
 *       latency of a connection setup (SYN, SYN-ACK, ACK through the
 *       pair, pcb insert and free: SMR and hash-lock work) p50 / p99 /
 *       max us and the count.
 */
#include <sys/param.h>
#include <sys/linker.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdint.h>
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

/* jenkins_hash32() of sys/libkern/jenkins_hash.c: Bob Jenkins' hashword(). */
#define	JROT(x, k)	(((x) << (k)) | ((x) >> (32 - (k))))
#define	JMIX(a, b, c) do {						\
	a -= c; a ^= JROT(c, 4); c += b;				\
	b -= a; b ^= JROT(a, 6); a += c;				\
	c -= b; c ^= JROT(b, 8); b += a;				\
	a -= c; a ^= JROT(c, 16); c += b;				\
	b -= a; b ^= JROT(a, 19); a += c;				\
	c -= b; c ^= JROT(b, 4); b += a;				\
} while (0)
#define	JFINAL(a, b, c) do {						\
	c ^= b; c -= JROT(b, 14);					\
	a ^= c; a -= JROT(c, 11);					\
	b ^= a; b -= JROT(a, 25);					\
	c ^= b; c -= JROT(b, 16);					\
	a ^= c; a -= JROT(c, 4);					\
	b ^= a; b -= JROT(a, 14);					\
	c ^= b; c -= JROT(b, 24);					\
} while (0)

static uint32_t
jenkins_hash32(const uint32_t *k, size_t length, uint32_t initval)
{
	uint32_t a, b, c;

	a = b = c = 0xdeadbeef + (((uint32_t)length) << 2) + initval;
	while (length > 3) {
		a += k[0]; b += k[1]; c += k[2];
		JMIX(a, b, c);
		length -= 3;
		k += 3;
	}
	switch (length) {
	case 3: c += k[2];	/* FALLTHROUGH */
	case 2: b += k[1];	/* FALLTHROUGH */
	case 1: a += k[0];
		JFINAL(a, b, c);
		/* FALLTHROUGH */
	case 0:
		break;
	}
	return (c);
}

/* The CPU of one direction: addresses and ports as they sit in the packet. */
static unsigned
flow_cpu(uint32_t seed, unsigned ncpu, struct in_addr src, struct in_addr dst,
    int proto, int sport, int dport)
{
	uint32_t key[4];
	uint16_t ports[2] = { htons((uint16_t)sport), htons((uint16_t)dport) };

	key[0] = src.s_addr;
	key[1] = dst.s_addr;
	key[2] = (uint32_t)proto;
	key[3] = (uint32_t)ports[0] << 16 | ports[1];
	return (jenkins_hash32(key, 4, seed) % ncpu);
}

static int
do_ports(uint32_t seed, unsigned ncpu, unsigned cpu, const char *src,
    const char *dst, int tcpport, int udpport, int count)
{
	struct in_addr s, d;
	int p, ncport = 0, uport = 0;

	if (inet_pton(AF_INET, src, &s) != 1 || inet_pton(AF_INET, dst, &d) != 1)
		return (1);
	printf("cports=");
	for (p = 40000; p < 65000 && (ncport < count || uport == 0); p++) {
		if (ncport < count && flow_cpu(seed, ncpu, s, d, 6, p, tcpport) == cpu)
			printf("%s%d", ncport++ ? "," : "", p);
		else if (uport == 0 && flow_cpu(seed, ncpu, s, d, 17, p, udpport) == cpu)
			uport = p;
	}
	if (ncport < count || uport == 0)
		return (1);
	printf(" uport=%d\n", uport);
	return (0);
}

static int
do_seed(const char *module)
{
	struct kld_sym_lookup l;
	char name[] = "pair_hash_seed";
	uint32_t v;
	int fid, fd;

	fid = kldfind(module);
	if (fid < 0)
		return (1);
	memset(&l, 0, sizeof(l));
	l.version = sizeof(l);
	l.symname = name;
	if (kldsym(fid, KLDSYM_LOOKUP, &l) != 0)
		return (1);
	fd = open("/dev/kmem", O_RDONLY);
	if (fd < 0 || pread(fd, &v, sizeof(v), (off_t)l.symvalue) != (ssize_t)sizeof(v))
		return (1);
	printf("%u\n", v);
	return (0);
}

static int
do_cpu(uint32_t seed, unsigned ncpu, const char *src, const char *dst,
    int proto, int sport, int dport)
{
	struct in_addr s, d;

	if (inet_pton(AF_INET, src, &s) != 1 || inet_pton(AF_INET, dst, &d) != 1)
		return (1);
	printf("%u\n", flow_cpu(seed, ncpu, s, d, proto, sport, dport));
	return (0);
}

static int
do_udpecho2(int port, int secs, long gap_ms)
{
	struct sockaddr_in sin, from;
	struct sockaddr sa_unspec;
	struct timespec t0, t1, end, last;
	struct pollfd pfd;
	socklen_t fl;
	char buf[256];
	long *lat, d;
	size_t n = 0, cap;
	ssize_t r;
	int s;

	cap = (size_t)secs * 1000 / (gap_ms > 0 ? gap_ms : 1) + 100;
	lat = calloc(cap, sizeof(*lat));
	s = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)port);
	if (lat == NULL || s < 0 || bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0)
		return (1);
	memset(&sa_unspec, 0, sizeof(sa_unspec));
	sa_unspec.sa_family = AF_UNSPEC;
	sa_unspec.sa_len = sizeof(sa_unspec);
	pfd.fd = s;
	pfd.events = POLLIN;
	clock_gettime(CLOCK_MONOTONIC, &end);
	last = end;
	end.tv_sec += secs;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (t0.tv_sec > end.tv_sec ||
		    (t0.tv_sec == end.tv_sec && t0.tv_nsec >= end.tv_nsec))
			break;
		if (ts_sub_us(&t0, &last) >= gap_ms * 1000L) {
			/* INP_WLOCK against the worker's INP_RLOCK in udp_input */
			(void)connect(s, &sa_unspec, sizeof(sa_unspec));
			clock_gettime(CLOCK_MONOTONIC, &t1);
			d = ts_sub_us(&t1, &t0);
			if (n < cap)
				lat[n++] = d;
			last = t1;
		}
		if (poll(&pfd, 1, 1) == 1) {
			fl = sizeof(from);
			r = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
			if (r > 0)
				(void)sendto(s, buf, (size_t)r, 0, (struct sockaddr *)&from, fl);
		}
	}
	qsort(lat, n, sizeof(*lat), cmp);
	printf("udpecho2 n=%zu wl_p50=%ld wl_p99=%ld wl_max=%ld\n", n,
	    n ? lat[n / 2] : 0, n ? lat[n * 99 / 100] : 0, n ? lat[n - 1] : 0);
	return (0);
}

static int
do_tcpecho(int port)
{
	struct sockaddr_in sin;
	int s, c, one = 1;

	s = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)port);
	if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
	    bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0 || listen(s, 128) != 0)
		return (1);
	for (;;) {
		c = accept(s, NULL, NULL);
		if (c >= 0)
			close(c);
	}
}

static int
do_tcpconn(const char *host, int port, int secs, long gap_ms)
{
	struct sockaddr_in sin;
	struct timespec t0, t1, end, gap = { 0, gap_ms * 1000000L };
	long *lat, failed = 0;
	size_t n = 0, cap;
	int s;

	cap = (size_t)secs * 1000 / (gap_ms > 0 ? gap_ms : 1) + 1000;
	lat = calloc(cap, sizeof(*lat));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)port);
	if (lat == NULL || inet_pton(AF_INET, host, &sin.sin_addr) != 1)
		return (1);
	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += secs;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (t0.tv_sec > end.tv_sec ||
		    (t0.tv_sec == end.tv_sec && t0.tv_nsec >= end.tv_nsec))
			break;
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0 || connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0)
			failed++;
		if (s >= 0)
			close(s);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if (n < cap)
			lat[n++] = ts_sub_us(&t1, &t0);
		if (gap_ms > 0)
			nanosleep(&gap, NULL);
	}
	qsort(lat, n, sizeof(*lat), cmp);
	printf("tcpconn n=%zu conn_p50=%ld conn_p99=%ld conn_max=%ld failed=%ld\n",
	    n, n ? lat[n / 2] : 0, n ? lat[n * 99 / 100] : 0, n ? lat[n - 1] : 0,
	    failed);
	return (0);
}

static int
do_udpecho(int port)
{
	struct sockaddr_in sin, from;
	socklen_t fl;
	char buf[256];
	ssize_t n;
	int s;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)port);
	if (s < 0 || bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0)
		return (1);
	for (;;) {
		fl = sizeof(from);
		n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
		if (n > 0)
			(void)sendto(s, buf, (size_t)n, 0, (struct sockaddr *)&from, fl);
	}
}

static int
do_udp(const char *host, int port, int secs, int sport, long gap_us)
{
	struct sockaddr_in sin;
	struct timespec t0, t1, end, gap = { 0, gap_us * 1000L };
	struct pollfd pfd;
	char buf[64];
	long *rtt, lost = 0;
	size_t n = 0, cap;
	int s;

	cap = (size_t)secs * 1000000L / (gap_us > 0 ? gap_us : 1) + 1000;
	rtt = calloc(cap, sizeof(*rtt));
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (rtt == NULL || s < 0)
		return (1);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	if (sport > 0) {
		sin.sin_port = htons((uint16_t)sport);
		if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0)
			return (1);
	}
	sin.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &sin.sin_addr) != 1 ||
	    connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0)
		return (1);
	memset(buf, 'k', sizeof(buf));
	pfd.fd = s;
	pfd.events = POLLIN;
	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += secs;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (t0.tv_sec > end.tv_sec ||
		    (t0.tv_sec == end.tv_sec && t0.tv_nsec >= end.tv_nsec))
			break;
		memcpy(buf, &n, sizeof(n));
		if (send(s, buf, sizeof(buf), 0) != (ssize_t)sizeof(buf))
			return (1);
		if (poll(&pfd, 1, 200) == 1 && recv(s, buf, sizeof(buf), 0) > 0) {
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (n < cap)
				rtt[n] = ts_sub_us(&t1, &t0);
		} else {
			lost++;
			if (n < cap)
				rtt[n] = 200000;
		}
		n++;
		if (gap_us > 0)
			nanosleep(&gap, NULL);
	}
	if (n > cap)
		n = cap;
	qsort(rtt, n, sizeof(*rtt), cmp);
	printf("udp n=%zu rtt_p50=%ld rtt_p99=%ld rtt_max=%ld lost=%ld\n", n,
	    n ? rtt[n / 2] : 0, n ? rtt[n * 99 / 100] : 0, n ? rtt[n - 1] : 0,
	    lost);
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
	if (argc >= 3 && strcmp(argv[1], "seed") == 0)
		return (do_seed(argv[2]));
	if (argc >= 9 && strcmp(argv[1], "ports") == 0)
		return (do_ports((uint32_t)strtoul(argv[2], NULL, 0),
		    (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]), argv[5],
		    argv[6], atoi(argv[7]), atoi(argv[8]),
		    argc > 9 ? atoi(argv[9]) : 1));
	if (argc >= 3 && strcmp(argv[1], "udpecho") == 0)
		return (do_udpecho(atoi(argv[2])));
	if (argc >= 5 && strcmp(argv[1], "udpecho2") == 0)
		return (do_udpecho2(atoi(argv[2]), atoi(argv[3]), atol(argv[4])));
	if (argc >= 3 && strcmp(argv[1], "tcpecho") == 0)
		return (do_tcpecho(atoi(argv[2])));
	if (argc >= 5 && strcmp(argv[1], "tcpconn") == 0)
		return (do_tcpconn(argv[2], atoi(argv[3]), atoi(argv[4]),
		    argc > 5 ? atol(argv[5]) : 2));
	if (argc >= 9 && strcmp(argv[1], "cpu") == 0)
		return (do_cpu((uint32_t)strtoul(argv[2], NULL, 0),
		    (unsigned)atoi(argv[3]), argv[4], argv[5], atoi(argv[6]),
		    atoi(argv[7]), atoi(argv[8])));
	if (argc >= 5 && strcmp(argv[1], "udp") == 0)
		return (do_udp(argv[2], atoi(argv[3]), atoi(argv[4]),
		    argc > 5 ? atoi(argv[5]) : 0, argc > 6 ? atol(argv[6]) : 500));
	fprintf(stderr, "usage: victim timer SECS [WORK_US] | spin SECS | "
	    "exec COUNT [GAP_MS] | udpecho PORT | udp HOST PORT SECS [SPORT [GAP_US]] | "
	    "ports SEED NCPU CPU SRC DST TCPPORT UDPPORT [COUNT] | seed MODULE | "
	    "cpu SEED NCPU SRC DST PROTO SPORT DPORT | udpecho2 PORT SECS GAP_MS | "
	    "tcpecho PORT | tcpconn HOST PORT SECS [GAP_MS]\n");
	return (2);
}
