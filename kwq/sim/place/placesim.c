/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * placesim - a discrete-event simulator of BULK placement (SPEC-p6g.md S3):
 * kwq's BULK workers (one per CPU thread, DRR over per-queue FIFO lists),
 * the producers that feed them, the CPUs they share with other threads,
 * and the cache lines the placement policies read and write.  Policy P5
 * runs the kernel's own placement code (../../kwq/kwq_place.c).
 *
 *   placesim [key=value ...]          one run, TSV metrics on stdout
 *   placesim help                     the keys and their defaults
 *
 * Time is in ns.  The model (S3.2):
 *
 *   CPU      each hardware thread runs at most one BULK job (an item or
 *            the worker's own overhead) and one job of other threads; when
 *            both have work the BULK worker gets `cap' of the time (the
 *            share limit), else all of it.  A thread whose sibling is busy
 *            runs at smt_bulk (BULK) or smt_other (other work) of its
 *            speed.  Rates change at events and completion times are
 *            recomputed (a fluid model).
 *   lines    every shared word the policies use sits in a line with a
 *            version; a read by a CPU that holds the current version
 *            costs c_hit, else a transfer priced by the topology distance
 *            to the last writer; a write invalidates the other holders.
 *            The costs are charged as CPU time to the thread doing it.
 *   workers  wake latency after a sleep (short or long), DRR with a
 *            quantum of 1 ms, passes that take a list (cut by P1's pass
 *            cap), items never preempted; a sleeping worker is woken by
 *            the doorbell of an enqueue.
 *   items    cost in ns of work at full speed; an item that runs on
 *            another core than the one it was produced on pays move_ns.
 *
 * Policies: 0 oracle, 10 oracle within the L3, 1 today (kwq_pick_any,
 * idle-path steal, steal wake, pass cap), 2 no stealing, 3 two choices,
 * 4 per-L3 load arrays, 5 gossip (kwq_place.c) with ablations, 6 random,
 * 7 donor (today's placement; idle workers steal only from CPUs that
 * flagged a backlog of H or more in one donor line, crest's first idea),
 * 8 shard (7 with the idle mask cut into lines of `group' CPUs, each
 * written by its own CPUs only; a producer reads its own group's line
 * and up to probe_groups more, then far_probe groups' lines further
 * out; a per-CPU tick flags a starved worker a donor).
 */

#include <sys/types.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kwq_place.h"

#define	MAXCPU		256
#define	MAXQ		1024
#define	MAXPROD		4096
#define	NONE		(-1)
#define	US		1000.0
#define	MS		1000000.0
#define	SEC		1000000000.0

/* ------------------------------------------------------------------ */
/* Configuration */

enum { P_ORACLE = 0, P_TODAY = 1, P_NOSTEAL = 2, P_TWO = 3, P_GROUP = 4,
    P_GOSSIP = 5, P_RANDOM = 6, P_DONOR = 7, P_SHARD = 8, P_ORACLE_L3 = 10 };

struct cfg {
	/* topology */
	char	topo[16];
	u_int	ncores, nthr, per_l3, l3_per_pkg, absent;
	/* policy */
	int	policy;
	u_int	shape;
	struct kp_params kp;
	int	handoff, spill, selfstep, handoff_n, busyterm;
	/* costs, ns */
	double	c_hit, c_sib, c_l3, c_pkg, c_far, c_atomic, scan_entry;
	double	place_ns, merge_ns, timer_ns;
	double	wl_short, wl_long, wl_thresh, wl_deep, wl_deep_thresh;
	double	smt_bulk, smt_other, cap, cap_period, quantum, move_ns;
	u_int	steal_scan, steal_batch, steal_max, steal_wake_at, limit;
	int	topo_sib_last;
	/* run */
	double	dur, warm;
	uint64_t seed;
	char	scen[24];
	/* scenario parameters */
	double	rho, c_small, c_large, p_large, pareto, other_ns, think;
	double	burst, period, gap, factor, share_drop, drop_ms;
	int	nprov, inflight, k, other_place, fixed, layout;
	char	mix[16];
	int	series;		/* print the 1 ms series */
	int	expcost;	/* exponential item costs (V1) */
	int	contend;	/* line serialization (default on) */
	int	group, probe_groups;	/* P8 */
	int	far_probe;	/* P8: far groups read per placement, at most */
	double	far_us;		/* P8: from a producer with this backlog, us (<0: a donor) */
	int	donor_cascade, donor_pull, donor_wakes, donor_avoid;	/* P7/P8 */
	double	donor_us;	/* a donor's backlog, us (spill at it too) */
	int	donor_bound;	/* a steal takes no more than stays below donor_us */
	double	watch_us;	/* P8: the starved-worker tick, us (0: none) */
	int	donor_lone;	/* P8: a lone item behind running work is a backlog */
	int	oneq;		/* all open producers share one queue */
	int	extraq;		/* stealable queues without producers */
	int	verbose;
};

static struct cfg C;

static void
cfg_default(void)
{
	memset(&C, 0, sizeof(C));
	strcpy(C.topo, "lab");
	C.policy = P_GOSSIP;
	C.shape = KP_TORUS4;
	kp_params_default(&C.kp);
	C.handoff = C.spill = C.selfstep = 1;
	C.handoff_n = 4;
	C.busyterm = 0;		/* the other-threads term hurt: S2 at 128 providers */
	C.contend = 1;
	C.group = 16;
	C.donor_cascade = C.donor_pull = C.donor_wakes = C.donor_avoid = 1;
	C.donor_us = 500;
	C.donor_bound = 1;	/* L: the matrix sweep of 200, 500 and 2000 us */
	C.watch_us = 1000;
	C.donor_lone = 1;
	C.probe_groups = 2;
	C.far_probe = 1;
	C.far_us = 0;	/* any backlog: the screen of 0 and 20 us, 2026-11-05 */
	C.c_hit = 2; C.c_sib = 20; C.c_l3 = 40; C.c_pkg = 100; C.c_far = 200;
	C.c_atomic = 10; C.scan_entry = 4;	/* V3: 17.9 us per scan of 2048 lists */
	C.place_ns = 30; C.merge_ns = 200; C.timer_ns = 150;
	/* lab: 1 us and 9 us calibrated by V3 and V4; the deep tier assumed */
	C.wl_short = 1 * US; C.wl_long = 9 * US; C.wl_thresh = 100 * US;
	C.wl_deep = 30 * US; C.wl_deep_thresh = 5 * MS;
	C.smt_bulk = 0.61; C.smt_other = 0.6; C.cap = 0.5; C.cap_period = 1 * MS; C.quantum = 1 * MS;
	C.move_ns = 0;
	C.steal_scan = 16; C.steal_batch = 16; C.steal_max = 64; C.steal_wake_at = 2;
	C.limit = 4096;
	C.dur = 200 * MS; C.warm = 20 * MS;
	C.seed = 1;
	strcpy(C.scen, "uniform");
	C.rho = 0.5; C.c_small = 3 * US; C.c_large = 300 * US; C.p_large = 0;
	C.pareto = 0; C.other_ns = 0; C.think = 0;
	C.burst = 2000; C.period = 10 * MS; C.gap = 100 * MS; C.factor = 4;
	C.share_drop = 0.1; C.drop_ms = 10;
	C.nprov = 16; C.inflight = 1; C.k = 4; C.other_place = 1; C.fixed = 0;
	strcpy(C.mix, "small");
}

/* ------------------------------------------------------------------ */
/* Random numbers: splitmix64 per stream */

static uint64_t
rnd64(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return (z ^ (z >> 31));
}

static double
rndu(uint64_t *s)
{
	return ((rnd64(s) >> 11) * (1.0 / 9007199254740992.0));
}

static double
rndexp(uint64_t *s, double mean)
{
	double u = rndu(s);

	return (-mean * log(u > 0 ? u : 1e-300));
}

static uint64_t gs;		/* the run's own stream */

/* ------------------------------------------------------------------ */
/* Events: a binary heap */

enum { EV_DONE_B, EV_DONE_O, EV_WAKE, EV_PUBLISH, EV_ARRIVE, EV_ISSUE,
    EV_RETRY, EV_PHASE, EV_SAMPLE, EV_CAP, EV_WATCH, EV_END };

struct ev {
	double		t;
	uint64_t	seq;
	int		type, a;
	uint64_t	b;
};

static struct ev *heap;
static size_t	nheap, capheap;
static uint64_t	evseq;
static double	now;

static bool
ev_less(const struct ev *x, const struct ev *y)
{
	return (x->t < y->t || (x->t == y->t && x->seq < y->seq));
}

static void
ev_push(double t, int type, int a, uint64_t b)
{
	size_t i;
	struct ev e = { t, evseq++, type, a, b };

	if (nheap == capheap) {
		capheap = capheap ? capheap * 2 : 1024;
		heap = realloc(heap, capheap * sizeof(*heap));
	}
	i = nheap++;
	while (i > 0 && ev_less(&e, &heap[(i - 1) / 2])) {
		heap[i] = heap[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	heap[i] = e;
}

static struct ev
ev_pop(void)
{
	struct ev top = heap[0], last = heap[--nheap];
	size_t i = 0, c;

	for (;;) {
		c = 2 * i + 1;
		if (c >= nheap)
			break;
		if (c + 1 < nheap && ev_less(&heap[c + 1], &heap[c]))
			c++;
		if (!ev_less(&heap[c], &last))
			break;
		heap[i] = heap[c];
		i = c;
	}
	if (nheap > 0)
		heap[i] = last;
	return (top);
}

/* ------------------------------------------------------------------ */
/* Topology */

struct cpu;
static int	ncpu;			/* threads present */
static int	maxid;			/* ids 0..maxid-1, some may be absent */
static int	cpu_core[MAXCPU], cpu_slot[MAXCPU], cpu_l3[MAXCPU], cpu_pkg[MAXCPU];
static bool	cpu_present[MAXCPU];
static int	ncores_t;
static int	core_cpu[MAXCPU][KP_MAXSMT], core_nthr[MAXCPU];
static int	order[MAXCPU][MAXCPU], order_n[MAXCPU], ring1[MAXCPU];
static struct kp_topo T;
static struct kp_core *blocks;		/* ncores x 3 lines, one array */
static int	hops[MAXCPU][MAXCPU];	/* torus hops between cores */

static void
topo_build(void)
{
	int i, j, c, id;
	u_int nc, nt;

	if (strcmp(C.topo, "guest") == 0) {
		C.ncores = 4; C.nthr = 1; C.per_l3 = 4; C.l3_per_pkg = 1;
	} else if (strcmp(C.topo, "lab") == 0) {
		C.ncores = 8; C.nthr = 2; C.per_l3 = 8; C.l3_per_pkg = 1;
	} else if (strcmp(C.topo, "a07") == 0) {
		C.ncores = 128; C.nthr = 1; C.per_l3 = 128; C.l3_per_pkg = 1;
	} else if (strcmp(C.topo, "t4") == 0) {
		C.ncores = 64; C.nthr = 2; C.per_l3 = 8; C.l3_per_pkg = 4;
	} else if (strcmp(C.topo, "odd6") == 0) {
		C.ncores = 3; C.nthr = 2; C.per_l3 = 3; C.l3_per_pkg = 1;
	} else if (strcmp(C.topo, "odd12") == 0) {
		C.ncores = 6; C.nthr = 2; C.per_l3 = 3; C.l3_per_pkg = 2;
	} else if (strcmp(C.topo, "odd127") == 0) {
		C.ncores = 127; C.nthr = 1; C.per_l3 = 127; C.l3_per_pkg = 1;
	} else if (strcmp(C.topo, "custom") != 0) {
		fprintf(stderr, "unknown topo %s\n", C.topo);
		exit(2);
	}
	nc = C.ncores;
	nt = C.nthr;
	assert(nc * nt <= MAXCPU && nt <= KP_MAXSMT);
	/* CPU ids core-major, as FreeBSD numbers SMT pairs (2k, 2k+1). */
	maxid = (int)(nc * nt);
	ncpu = 0;
	ncores_t = 0;
	for (i = 0; i < (int)nc; i++) {
		core_nthr[ncores_t] = 0;
		for (j = 0; j < (int)nt; j++) {
			id = i * (int)nt + j;
			/* T5's absent CPUs: every `absent'-th id missing */
			cpu_present[id] = !(C.absent > 0 && id % (int)C.absent == (int)C.absent - 1);
			if (!cpu_present[id])
				continue;
			cpu_core[id] = ncores_t;
			cpu_slot[id] = core_nthr[ncores_t];
			core_cpu[ncores_t][core_nthr[ncores_t]++] = id;
			cpu_l3[id] = i / (int)C.per_l3;
			cpu_pkg[id] = i / (int)C.per_l3 / (int)C.l3_per_pkg;
			ncpu++;
		}
		if (core_nthr[ncores_t] > 0)
			ncores_t++;
	}
	/* kwq_topo_order: rings by sharing level, alternating +d, -d by id. */
	for (c = 0; c < maxid; c++) {
		int lvl, d, s, n = 0, sib_n = 0;
		bool seen[MAXCPU] = { false };

		if (!cpu_present[c])
			continue;
		seen[c] = true;
		ring1[c] = 0;
		for (lvl = 0; lvl < 4; lvl++) {
			int added = 0;

			if (lvl == 0 && C.topo_sib_last)
				continue;
			for (d = 1; d < maxid; d++)
				for (s = 0; s < 2; s++) {
					int x = s == 0 ? (c + d) % maxid : (c - d + maxid) % maxid;
					bool in;

					if (!cpu_present[x] || seen[x])
						continue;
					in = lvl == 0 ? cpu_core[x] == cpu_core[c] :
					    lvl == 1 ? cpu_l3[x] == cpu_l3[c] :
					    lvl == 2 ? cpu_pkg[x] == cpu_pkg[c] : true;
					if (C.topo_sib_last && cpu_core[x] == cpu_core[c])
						in = false;
					if (!in)
						continue;
					seen[x] = true;
					order[c][n++] = x;
					added++;
				}
			if (lvl == 0)
				sib_n = added;
		}
		if (C.topo_sib_last)
			for (d = 1; d < maxid; d++)
				for (s = 0; s < 2; s++) {
					int x = s == 0 ? (c + d) % maxid : (c - d + maxid) % maxid;

					if (cpu_present[x] && !seen[x]) {
						seen[x] = true;
						order[c][n++] = x;
					}
				}
		order_n[c] = n;
		ring1[c] = sib_n;
	}
	/* The torus and the blocks, for P5 (built for every policy: cheap). */
	memset(&T, 0, sizeof(T));
	T.kt_ncores = (u_int)ncores_t;
	T.kt_maxcpu = (u_int)maxid;
	T.kt_node = calloc((size_t)ncores_t, sizeof(*T.kt_node));
	T.kt_core = calloc((size_t)ncores_t, sizeof(*T.kt_core));
	T.kt_cpu_core = calloc((size_t)maxid, sizeof(int));
	T.kt_cpu_slot = calloc((size_t)maxid, 1);
	blocks = aligned_alloc(KP_LINE, (size_t)ncores_t * sizeof(struct kp_core));
	memset(blocks, 0, (size_t)ncores_t * sizeof(struct kp_core));
	for (i = 0; i < ncores_t; i++) {
		T.kt_core[i] = &blocks[i];
		T.kt_node[i].kn_nthr = (uint8_t)core_nthr[i];
		for (j = 0; j < core_nthr[i]; j++)
			T.kt_node[i].kn_cpu[j] = core_cpu[i][j];
		T.kt_node[i].kn_l3 = (uint16_t)cpu_l3[core_cpu[i][0]];
		T.kt_node[i].kn_pkg = (uint16_t)cpu_pkg[core_cpu[i][0]];
	}
	for (c = 0; c < maxid; c++) {
		T.kt_cpu_core[c] = cpu_present[c] ? cpu_core[c] : -1;
		T.kt_cpu_slot[c] = (uint8_t)(cpu_present[c] ? cpu_slot[c] : 0);
	}
	kp_torus_build(&T, C.shape, (uint32_t)C.seed);
	/* hops by BFS */
	for (i = 0; i < ncores_t; i++) {
		int qq[MAXCPU], h = 0, tl = 0, x, k;

		for (j = 0; j < ncores_t; j++)
			hops[i][j] = -1;
		hops[i][i] = 0;
		qq[tl++] = i;
		while (h < tl) {
			x = qq[h++];
			for (k = 0; k < T.kt_node[x].kn_nnb; k++) {
				j = T.kt_node[x].kn_nb[k];
				if (hops[i][j] < 0) {
					hops[i][j] = hops[i][x] + 1;
					qq[tl++] = j;
				}
			}
		}
	}
}

/* topology distance between CPUs, 0 same thread .. 4 other package */
static int
cdist(int a, int b)
{
	if (a == b)
		return (0);
	if (cpu_core[a] == cpu_core[b])
		return (1);
	if (cpu_l3[a] == cpu_l3[b])
		return (2);
	if (cpu_pkg[a] == cpu_pkg[b])
		return (3);
	return (4);
}

static double
xfer_cost(int a, int b)
{
	switch (b < 0 ? 2 : cdist(a, b)) {
	case 0: return (C.c_hit);
	case 1: return (C.c_sib);
	case 2: return (C.c_l3);
	case 3: return (C.c_pkg);
	default: return (C.c_far);
	}
}

/* ------------------------------------------------------------------ */
/* Cache lines: a version, the last writer, the CPUs holding it */

#define	HOLD_W	((MAXCPU + 63) / 64)

struct line {
	uint64_t	hold[HOLD_W];
	int		writer;
	uint32_t	version;
	double		busy;		/* serialization: the line moves one at a time */
};

static uint64_t	st_waits;
static double	st_wait_ns;

/*
 * Contention: a transfer or an invalidation holds the line for its
 * transfer time; one that finds the line held waits for it (an RMW or a
 * miss queues behind the others in the coherence fabric).  Hits do not.
 */
static double
line_occupy(struct line *ln, double xfer)
{
	double start = now > ln->busy ? now : ln->busy, w = start - now;

	if (!C.contend)
		return (xfer);
	ln->busy = start + xfer;
	if (w > 0) {
		st_waits++;
		st_wait_ns += w;
	}
	return (w + xfer);
}

static struct line *lines;
static int	nlines;
static int	L_IDLE;			/* P1's idle mask */
static int	L_GROUP;		/* P4's group words: L_GROUP + l3 */
static int	L_BLOCK;		/* P5's blocks: L_BLOCK + 3 * core + k */
static int	L_LIST;			/* (queue, cpu) list heads: L_LIST + q * maxid + cpu */
static int	L_WMTX;			/* P1: a worker's kw_mtx: L_WMTX + cpu */
static int	L_DONOR;		/* P7's donor mask */
static int	L_SHARD;		/* P8: the idle mask of group g, L_SHARD + g */
static int	L_DHINT;		/* P7: a donor's longest stealable list, L_DHINT + cpu */

/* What the running code costs: charged by its caller to a thread. */
static int	cur_cpu = 0;
static double	cur_cost;
static uint64_t	st_reads, st_xfers, st_writes, st_invals;

static double
line_read(int cpu, int l)
{
	struct line *ln = &lines[l];
	double c;

	st_reads++;
	if (ln->hold[cpu / 64] & (1ULL << (cpu % 64)))
		return (C.c_hit);
	st_xfers++;
	c = line_occupy(ln, xfer_cost(cpu, ln->writer));
	ln->hold[cpu / 64] |= 1ULL << (cpu % 64);
	return (c);
}

static double
line_write(int cpu, int l)
{
	struct line *ln = &lines[l];
	double c = C.c_hit;
	int i, far = -1;
	bool others = false;

	st_writes++;
	for (i = 0; i < HOLD_W; i++) {
		uint64_t m = ln->hold[i];

		if (i == cpu / 64)
			m &= ~(1ULL << (cpu % 64));
		if (m != 0) {
			others = true;
			far = i * 64 + __builtin_ctzll(m);
		}
	}
	if (others || !(ln->hold[cpu / 64] & (1ULL << (cpu % 64)))) {
		st_invals += others;
		c = line_occupy(ln, xfer_cost(cpu, others ? far : ln->writer));
	}
	memset(ln->hold, 0, sizeof(ln->hold));
	ln->hold[cpu / 64] = 1ULL << (cpu % 64);
	ln->writer = cpu;
	ln->version++;
	return (c);
}

static void
lines_init(int nq)
{
	nlines = 0;
	L_IDLE = nlines++;
	L_GROUP = nlines; nlines += MAXCPU * 16;
	L_BLOCK = nlines; nlines += 3 * ncores_t;
	L_LIST = nlines; nlines += nq * maxid;
	L_WMTX = nlines; nlines += maxid;
	L_DONOR = nlines++;
	L_SHARD = nlines; nlines += maxid;
	L_DHINT = nlines; nlines += maxid;
	lines = calloc((size_t)nlines, sizeof(*lines));
	for (int i = 0; i < nlines; i++)
		lines[i].writer = -1;
}

/* kwq_place_env.h for the shared code */
static int
block_line(const void *p)
{
	size_t off = (size_t)((const char *)p - (const char *)blocks);

	assert(off < (size_t)ncores_t * sizeof(struct kp_core));
	return (L_BLOCK + (int)(off / KP_LINE));
}

uint64_t
kp_env_load(const uint64_t *p)
{
	cur_cost += line_read(cur_cpu, block_line(p));
	return (*p);
}

void
kp_env_store(uint64_t *p, uint64_t v)
{
	cur_cost += line_write(cur_cpu, block_line(p));
	*p = v;
}

bool
kp_env_tryflag(u_int *f)
{
	if (*f)
		return (false);
	*f = 1;
	return (true);
}

void
kp_env_clrflag(u_int *f)
{
	*f = 0;
}

static uint64_t st_violations;

void
kp_env_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "kp assert: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	st_violations++;
}

static kp_time_t
kpnow(void)
{
	/* offset so that the 32-bit clock wraps 1 s into every run */
	return ((kp_time_t)(0xfff00000u + (uint64_t)(now * 1.048576e-3)));
}

/* ------------------------------------------------------------------ */
/* Items, queues, lists */

struct item {
	int32_t		next;
	int16_t		q, src, core0;	/* producer's CPU, the core its data is on */
	uint8_t		moves, cls;
	int32_t		prod;
	double		cost, t_enq, t_first;
};

static struct item *items;
static int32_t	nitems, capitems, freeitem = NONE;

static int32_t
item_new(void)
{
	int32_t i;

	if (freeitem != NONE) {
		i = freeitem;
		freeitem = items[i].next;
	} else {
		if (nitems == capitems) {
			capitems = capitems ? capitems * 2 : 4096;
			items = realloc(items, (size_t)capitems * sizeof(*items));
		}
		i = nitems++;
	}
	memset(&items[i], 0, sizeof(items[i]));
	items[i].next = NONE;
	return (i);
}

static void
item_free(int32_t i)
{
	items[i].next = freeitem;
	freeitem = i;
}

struct queue {
	bool		stealable;
	double		nominal;	/* mean cost, the cost hint */
	double		done_work, offered;
	uint64_t	done;
};

static struct queue Q[MAXQ];
static int	nq;

struct qlist {
	int32_t		head, tail;
	u_int		depth;
	double		work;		/* ns of work queued, at full speed */
	double		deficit;
	double		cost_e;		/* ns of CPU per item on this CPU */
	bool		active;		/* in the CPU's DRR ring */
	bool		fresh;		/* its next pass starts a round: add a quantum */
};

static struct qlist *ql;
#define	QL(q, c)	(&ql[(size_t)(q) * (size_t)maxid + (size_t)(c)])
#define	LISTLINE(q, c)	(L_LIST + (q) * maxid + (c))

static void
list_append(struct qlist *l, int32_t i)
{
	items[i].next = NONE;
	if (l->tail == NONE)
		l->head = i;
	else
		items[l->tail].next = i;
	l->tail = i;
	l->depth++;
	l->work += items[i].cost;
}

static int32_t
list_pop(struct qlist *l)
{
	int32_t i = l->head;

	if (i == NONE)
		return (NONE);
	l->head = items[i].next;
	if (l->head == NONE)
		l->tail = NONE;
	l->depth--;
	l->work -= items[i].cost;
	items[i].next = NONE;
	return (i);
}

/* Cut the first n items off l as a chain; returns head, *tail and *w set. */
static int32_t
list_cut(struct qlist *l, u_int n, int32_t *tail, double *w)
{
	int32_t h = l->head, t = h;
	u_int k;

	*w = 0;
	if (n == 0 || h == NONE)
		return (NONE);
	if (n > l->depth)
		n = l->depth;
	*w = items[t].cost;
	for (k = 1; k < n; k++) {
		t = items[t].next;
		*w += items[t].cost;
	}
	l->work -= *w;
	l->head = items[t].next;
	if (l->head == NONE)
		l->tail = NONE;
	items[t].next = NONE;
	l->depth -= n;
	*tail = t;
	return (h);
}

static void
list_prepend_chain(struct qlist *l, int32_t h, int32_t t, u_int n, double w)
{
	if (h == NONE)
		return;
	items[t].next = l->head;
	l->head = h;
	if (l->tail == NONE)
		l->tail = t;
	l->depth += n;
	l->work += w;
}

static void
list_append_chain(struct qlist *l, int32_t h, int32_t t, u_int n, double w)
{
	if (h == NONE)
		return;
	if (l->tail == NONE)
		l->head = h;
	else
		items[l->tail].next = h;
	l->tail = t;
	l->depth += n;
	l->work += w;
}

/* ------------------------------------------------------------------ */
/* CPUs: workers and the fluid model */

enum { W_SLEEP, W_WAKING, W_RUN };
enum { J_NONE, J_ITEM, J_OVH };	/* J_OVH: the worker's own time outside passes */

struct chunk {
	int32_t		next;
	int32_t		prod;		/* closed loop: issue the next request */
	double		work;
};

static struct chunk *chunks;
static int32_t	nchunks, capchunks, freechunk = NONE;

struct cpu {
	int		ws;
	double		sleep_t;
	bool		tried;
	/* DRR: a ring of active queues */
	int		*ring;
	int		rhead, rn;
	/* the pass */
	int		pq;
	int32_t		bhead, btail;
	u_int		bn;
	double		bwork;		/* the batch's work */
	bool		inmask;		/* P1's idle mask bit */
	/* BULK job */
	int		bj;
	int32_t		bitem;
	double		brem, brate, bvb, bcpu;
	uint64_t	bseq;
	double		debt_b;
	/* other threads */
	int32_t		ohead, otail;
	int		on;
	double		orem, orate, ovo;
	uint64_t	oseq;
	bool		hog;
	double		capx;		/* BULK's share while other threads want the CPU */
	/* the share limit: BULK's contended time in the current period */
	int64_t		cap_k;
	double		cap_used;
	bool		cap_over;
	uint64_t	capseq;
	double		debt_o;
	double		last_t;		/* fluid state last advanced */
	/* the load figure (P5) */
	struct kp_load	kl;
	double		share_e, busy_e;
	uint64_t	pubseq;
	uint32_t	rng;
	uint64_t	rs;
	/* work queued on this CPU (lists and batch), ns at full speed */
	double		qwork;
	/* accounting, ns */
	double		t_bulk, t_other, t_scheme, t_idle, t_sleepstart;
	/* M4 */
	double		idle_seen;	/* first sample idle while a backlog waited */
	int		idle_hops;
	/* P4 */
	u_int		grp_load;
	/* P7 */
	bool		donor;
	int		dq;		/* the donor's longest stealable queue */
	int		far_g;		/* P8: the far probe's group, -1 before the first */
	uint64_t	dec_n, watch_n;	/* P8: decision points, and at the tick's arming */
	bool		watch_armed;
	bool		starved;	/* P8: flagged by the tick; lone items stealable */
	/* last handoff, M7 */
	uint64_t	handoffs_in, handoffs_out;
};

static struct cpu cpus[MAXCPU];

static int32_t
chunk_new(double work, int32_t prod)
{
	int32_t i;

	if (freechunk != NONE) {
		i = freechunk;
		freechunk = chunks[i].next;
	} else {
		if (nchunks == capchunks) {
			capchunks = capchunks ? capchunks * 2 : 1024;
			chunks = realloc(chunks, (size_t)capchunks * sizeof(*chunks));
		}
		i = nchunks++;
	}
	chunks[i].next = NONE;
	chunks[i].work = work;
	chunks[i].prod = prod;
	return (i);
}

static bool
thread_busy(int c)
{
	struct cpu *p = &cpus[c];

	return ((p->bj != J_NONE && p->ws == W_RUN) || p->hog || p->on > 0);
}

static bool
sib_busy(int c)
{
	int k = cpu_core[c], i, s;

	for (i = 0; i < core_nthr[k]; i++) {
		s = core_cpu[k][i];
		if (s != c && thread_busy(s))
			return (true);
	}
	return (false);
}

/* Advance one thread's jobs to now at their current rates. */
static void
thread_advance(int c)
{
	struct cpu *p = &cpus[c];
	double dt = now - p->last_t, a, fb, fo;

	if (dt <= 0)
		return;
	fb = (p->bj != J_NONE && p->ws == W_RUN && p->bvb > 0) ? p->brate / p->bvb : 0;
	fo = (p->ovo > 0) ? p->orate / p->ovo : 0;
	if (p->bj != J_NONE && p->ws == W_RUN) {
		p->brem -= p->brate * dt;
		p->bcpu += fb * dt;
		p->t_bulk += fb * dt;
		if ((p->hog || p->on > 0) && p->bj == J_ITEM)
			p->cap_used += fb * dt;
	}
	if (p->on > 0) {
		p->orem -= p->orate * dt;
		p->t_other += fo * dt;
	} else if (p->hog)
		p->t_other += fo * dt;
	if (!thread_busy(c))
		p->t_idle += dt;
	if ((int64_t)(now / C.cap_period) != p->cap_k) {
		p->cap_k = (int64_t)(now / C.cap_period);
		p->cap_used = 0;
		p->cap_over = false;
	} else if (p->cap_used >= p->capx * C.cap_period - 1e-6)
		p->cap_over = true;
	/* the load figure's averages, tau 10 ms */
	a = 1 - exp(-dt / (10 * MS));
	p->busy_e += a * (fo - p->busy_e);
	if (fb > 0)
		p->share_e += a * (fb - p->share_e);
	p->last_t = now;
}

static void
thread_rates(int c)
{
	struct cpu *p = &cpus[c];
	bool b = p->bj != J_NONE && p->ws == W_RUN, o = p->hog || p->on > 0;
	bool s = sib_busy(c);
	double vb = s ? C.smt_bulk : 1, vo = s ? C.smt_other : 1;

	p->bvb = vb;
	p->ovo = vo;
	if (b && o && p->bj == J_OVH) {
		/* outside a pass: not charged to the share limit (kwq_steal()) */
		p->brate = vb;
		p->orate = 0;
	} else if (b && o) {
		/*
		 * The share limit (SCHED.md S6.3): BULK runs at full speed
		 * until it has used capx of the period while other threads
		 * wanted the CPU, then waits for the next period.
		 */
		p->brate = p->cap_over ? 0 : vb;
		p->orate = p->cap_over ? vo : 0;
	} else {
		p->brate = b ? vb : 0;
		p->orate = o ? vo : 0;
	}
}

static void
core_resched(int k)
{
	int i, c;
	struct cpu *p;

	for (i = 0; i < core_nthr[k]; i++)
		thread_advance(core_cpu[k][i]);
	for (i = 0; i < core_nthr[k]; i++) {
		c = core_cpu[k][i];
		p = &cpus[c];
		thread_rates(c);
		if (p->bj != J_NONE && p->ws == W_RUN) {
			p->bseq++;
			if (p->brate > 0)
				ev_push(now + (p->brem > 0 ? p->brem : 0) / p->brate,
				    EV_DONE_B, c, p->bseq);
		}
		if (p->on > 0) {
			p->oseq++;
			if (p->orate > 0)
				ev_push(now + (p->orem > 0 ? p->orem : 0) / p->orate,
				    EV_DONE_O, c, p->oseq);
		}
		/* contended: the allowance runs out, or the period ends */
		if (p->bj == J_ITEM && p->ws == W_RUN && (p->hog || p->on > 0)) {
			double pend = (double)(p->cap_k + 1) * C.cap_period, t = pend;

			if (!p->cap_over && p->brate > 0 &&
			    now + (p->capx * C.cap_period - p->cap_used) / (p->brate / p->bvb) < t)
				t = now + (p->capx * C.cap_period - p->cap_used) / (p->brate / p->bvb);
			p->capseq++;
			ev_push(t > now ? t : now + 1, EV_CAP, c, p->capseq);
		}
	}
}

#define	RESCHED(c)	core_resched(cpu_core[(c)])

/* Other threads' work on a CPU: a FIFO of chunks. */
static void
other_add(int c, double work, int32_t prod)
{
	struct cpu *p = &cpus[c];
	int32_t ch = chunk_new(work, prod);

	if (p->otail == NONE)
		p->ohead = ch;
	else
		chunks[p->otail].next = ch;
	p->otail = ch;
	if (p->on++ == 0) {
		thread_advance(c);
		p->orem = work;
		RESCHED(c);
	}
}

/* Scheme cost of a producer, as other threads' time on its CPU. */
static void
charge_other(int c, double ns)
{
	struct cpu *p = &cpus[c];

	p->t_scheme += ns;
	p->debt_o += ns;
	if (p->debt_o >= 1 * US) {
		other_add(c, p->debt_o, NONE);
		p->debt_o = 0;
	}
}

/* Scheme cost of the worker, added to its next BULK job. */
static void
charge_bulk(int c, double ns)
{
	cpus[c].t_scheme += ns;
	cpus[c].debt_b += ns;
}

/* ------------------------------------------------------------------ */
/* Metrics (S3.4) */

#define	HB	8	/* buckets per octave */
#define	NHB	(HB * 44)

struct hist {
	uint64_t	n;
	double		sum, max;
	uint64_t	b[NHB];
};

static void
h_add(struct hist *h, double x)
{
	int i;

	if (x < 0)
		x = 0;
	i = (int)(HB * log2(x + 1));
	if (i >= NHB)
		i = NHB - 1;
	h->b[i]++;
	h->n++;
	h->sum += x;
	if (x > h->max)
		h->max = x;
}

static double
h_q(const struct hist *h, double q)
{
	uint64_t want, acc = 0;
	int i;

	if (h->n == 0)
		return (0);
	want = (uint64_t)ceil(q * (double)h->n);
	if (want == 0)
		want = 1;
	for (i = 0; i < NHB; i++) {
		acc += h->b[i];
		if (acc >= want) {
			/* the bucket's upper edge, but never above the maximum */
			double v = exp2((double)(i + 1) / HB) - 1;

			return (v < h->max ? v : h->max);
		}
	}
	return (h->max);
}

static double
h_mean(const struct hist *h)
{
	return (h->n ? h->sum / (double)h->n : 0);
}

static struct {
	struct hist	wait[3];	/* small, large, all */
	struct hist	regret, age, herd, disc, react;
	uint64_t	place, good, sampled, local[5], steps[8], checks, fails;
	uint64_t	expired, moves[5], handoffs, steals, steal_tries, pingpong;
	uint64_t	refused, ops, merges, spills, selfs;
	double		work, idle_l3, idle_any, cap_t;
	double		disc_hop_sum[32], disc_hop_max[32];
	uint64_t	disc_hop_n[32];
	uint64_t	xfers0, reads0;
	double		t0, bulk0, oth0, sch0;
} M;

/* the 1 ms series, for the reaction time (M5) */
#define	SERIES_MAX	200000
static struct {
	uint32_t	place, good;
	float		idle;
} *ser;
static int	nser;
static double	steps_t[64];
static int	nsteps;

static bool
warm(void)
{
	return (now >= C.warm);
}

/* ------------------------------------------------------------------ */
/* What the oracle knows: when an item placed now on c would start */

static double
rate_est(int c)
{
	struct cpu *p = &cpus[c];
	double v = sib_busy(c) ? C.smt_bulk : 1;

	return (v * ((p->hog || p->on > 0) ? p->capx : 1));
}

static double
est_start(int c, int q)
{
	struct cpu *p = &cpus[c];
	double r = rate_est(c), t = 0, own, rest;

	if (r <= 1e-6)
		return (1e15);
	if (p->ws == W_SLEEP)
		t += now - p->sleep_t < C.wl_thresh ? C.wl_short :
		    now - p->sleep_t < C.wl_deep_thresh ? C.wl_long : C.wl_deep;
	else if (p->ws == W_WAKING)
		t += C.wl_short;
	if (p->bj != J_NONE) {
		double rem = p->brem - (p->ws == W_RUN ? p->brate * (now - p->last_t) : 0);

		t += (rem > 0 ? rem : 0) / r;
	}
	/* DRR: its own queue's backlog, at most a quantum of each other one */
	own = QL(q, c)->work + (p->pq == q ? p->bwork : 0);
	rest = p->qwork - own;
	if (rest > (double)p->rn * C.quantum)
		rest = (double)p->rn * C.quantum;
	return (t + (own + rest) / r);
}

static int
oracle(int q, int from, bool l3only)
{
	double best = 1e18, e;
	int c, bc = from, ties = 0;

	for (c = 0; c < maxid; c++) {
		if (!cpu_present[c] || (l3only && cpu_l3[c] != cpu_l3[from]))
			continue;
		e = est_start(c, q);
		if (e < best - 1) {
			best = e;
			bc = c;
			ties = 1;
		} else if (e <= best + 1 && rnd64(&gs) % (uint64_t)++ties == 0)
			bc = c;
	}
	return (bc);
}

/* ------------------------------------------------------------------ */
/* Policies */

static bool
queue_ok(void *arg, int c)
{
	int q = *(int *)arg;
	struct qlist *l = QL(q, c);

	cur_cost += line_read(cur_cpu, LISTLINE(q, c));
	return (l->depth < C.limit &&
	    (double)l->depth * l->cost_e < (double)C.kp.h_us * US);
}

/*
 * P7/P8 without an idle worker in reach: the own CPU unless it is a
 * donor (backlog of H or more), else the nearest CPU in reach that is
 * not, else kwq_pick_any()'s shallowest of the own list and four
 * probes.  One read of the donor line, cached while nobody changes.
 */
static int
avoid_donors(int q, int me, int reach)
{
	int i, c, best = me;
	u_int bd = QL(q, me)->depth;

	cur_cost += line_read(me, L_DONOR);
	if (!cpus[me].donor)
		return (me);
	for (i = 0; i < reach; i++)
		if (!cpus[order[me][i]].donor)
			return (order[me][i]);
	for (i = 0; i < reach && i < 4 && bd != 0; i++) {
		c = order[me][i];
		cur_cost += line_read(me, LISTLINE(q, c));
		if (QL(q, c)->depth < bd) {
			best = c;
			bd = QL(q, c)->depth;
		}
	}
	return (best);
}

/* P8: the idle mask's group of a CPU, and its line */
static int
shard_of(int c)
{
	return (c / (C.group > 0 ? C.group : 16));
}

static int
idle_line(int c)
{
	return (C.policy == P_SHARD ? L_SHARD + shard_of(c) : L_IDLE);
}

/*
 * P8, a donor without an idle worker in the near groups: up to far_probe
 * more groups' lines, from a cursor that stays on a group while it has
 * an idle worker and moves on when it has none.  An island whose
 * overflow exceeds its neighbourhood so still reaches the idle rest of
 * the machine, at one line per placement and only while it overflows.
 * Each producer starts at its own group and bit, so producers spread.
 */
static int
pick_far(int q, int me, const int *seen, int ns)
{
	struct cpu *p = &cpus[me];
	int gsz = C.group > 0 ? C.group : 16, ng = (maxid + gsz - 1) / gsz;
	int k, j, i, c, tries = 0;

	if (p->far_g < 0)
		p->far_g = (shard_of(me) + 1 + me) % ng;
	for (k = 0; k < C.far_probe && tries < ng; k++) {
		for (; tries < ng; tries++, p->far_g = (p->far_g + 1) % ng) {
			for (i = 0; i < ns && seen[i] != p->far_g; i++)
				;
			if (i == ns)
				break;
		}
		if (tries == ng)
			break;
		cur_cost += line_read(me, L_SHARD + p->far_g);
		for (j = 0; j < gsz; j++) {
			c = p->far_g * gsz + (me + j) % gsz;
			if (c >= maxid || !cpu_present[c] || !cpus[c].inmask)
				continue;
			cur_cost += line_read(me, LISTLINE(q, c));
			if (QL(q, c)->depth == 0)
				return (c);
		}
		tries++;
		p->far_g = (p->far_g + 1) % ng;
	}
	return (NONE);
}

/*
 * P8: the nearest idle worker with this queue empty, in the own group's
 * line, then in up to probe_groups other groups' lines (the nearest by
 * topology order); else, for a donor, pick_far(); else kwq_pick_any()'s
 * own CPU and probes.
 */
static int
pick_shard(int q, int me)
{
	int i, c, g, seen[MAXCPU / 1 + 1], ns = 0, k, best = me;
	u_int bd;
	bool read;

	for (i = -1; i < order_n[me]; i++) {
		c = i < 0 ? me : order[me][i];
		g = shard_of(c);
		read = false;
		for (k = 0; k < ns; k++)
			if (seen[k] == g)
				read = true;
		if (!read) {
			if (ns > C.probe_groups)
				break;
			seen[ns++] = g;
			cur_cost += line_read(me, L_SHARD + g);
		}
		if (c == me || !cpus[c].inmask)
			continue;
		cur_cost += line_read(me, LISTLINE(q, c));
		if (QL(q, c)->depth == 0)
			return (c);
	}
	if (C.far_probe > 0 && (C.far_us < 0 ? cpus[me].donor : cpus[me].qwork /
	    (cpus[me].share_e > 1.0 / 16 ? cpus[me].share_e : 1.0 / 16) >= C.far_us * US) &&
	    (c = pick_far(q, me, seen, ns)) != NONE)
		return (c);
	cur_cost += line_read(me, LISTLINE(q, me));
	bd = QL(q, me)->depth;
	if (C.donor_avoid)
		return (avoid_donors(q, me, order_n[me] < 16 ? order_n[me] : 16));
	for (i = 0; i < order_n[me] && i < 4 && bd != 0; i++) {
		c = order[me][i];
		cur_cost += line_read(me, LISTLINE(q, c));
		if (QL(q, c)->depth < bd) {
			best = c;
			bd = QL(q, c)->depth;
		}
	}
	return (best);
}

/* P1/P2: kwq_pick_any() */
static int
pick_any(int q, int me)
{
	int reach = order_n[me] < (int)C.steal_scan ? order_n[me] : (int)C.steal_scan;
	int i, c, best = me;
	u_int bd;

	cur_cost += line_read(me, L_IDLE);
	for (i = 0; i < reach; i++) {
		c = order[me][i];
		if (!cpus[c].inmask)
			continue;
		cur_cost += line_read(me, LISTLINE(q, c));
		if (QL(q, c)->depth == 0)
			return (c);
	}
	cur_cost += line_read(me, LISTLINE(q, me));
	bd = QL(q, me)->depth;
	if (C.policy == P_DONOR && C.donor_avoid)
		return (avoid_donors(q, me, reach));
	for (i = 0; i < reach && i < 4 && bd != 0; i++) {
		c = order[me][i];
		cur_cost += line_read(me, LISTLINE(q, c));
		if (QL(q, c)->depth < bd) {
			best = c;
			bd = QL(q, c)->depth;
		}
	}
	return (best);
}

/* P3: the producer's thread and two random cores of its L3 */
static int
pick_two(int q, int me)
{
	int cand[3], n = 1, tries = 0, c, k, i, best;
	double e, be;

	cand[0] = me;
	while (n < 3 && tries++ < 16) {
		c = (int)(rnd64(&gs) % (uint64_t)maxid);
		if (!cpu_present[c] || cpu_l3[c] != cpu_l3[me] || cpu_core[c] == cpu_core[me])
			continue;
		k = cpu_core[c];
		c = core_cpu[k][rnd64(&gs) % (uint64_t)core_nthr[k]];
		cand[n++] = c;
		cur_cost += xfer_cost(me, c);
	}
	best = me;
	be = est_start(me, q);
	for (i = 1; i < n; i++) {
		e = est_start(cand[i], q);
		if (e < be) {
			be = e;
			best = cand[i];
		}
	}
	return (best);
}

/*
 * P4: group words.  Each L3 keeps an array of its threads' published
 * loads, 16 to a line; a thread writes its slot when its bucket changes
 * (the status word's rule), a producer reads its own L3's lines and
 * takes the least loaded thread (an idle core first, then the idle
 * thread, then the lowest load).  Complete within the L3, blind beyond.
 */
static u_int	grp_load[MAXCPU];
static int	grp_idx[MAXCPU];		/* the thread's slot in its L3 */
static int	grp_cpus[MAXCPU][MAXCPU], grp_n[MAXCPU];
static int	nl3;

static void
grp_update(int c, u_int load)
{
	cur_cost += line_write(c, L_GROUP + cpu_l3[c] * 16 + grp_idx[c] / 16);
	grp_load[c] = load;
}

static int
pick_group(int q, int me)
{
	int g = cpu_l3[me], i, c, s, bc = me, qq = q, ties = 0;
	double best = 1e18, sc;
	bool sidle;

	for (i = 0; i < grp_n[g]; i += 16)
		cur_cost += line_read(me, L_GROUP + g * 16 + i / 16);
	for (i = 0; i < grp_n[g]; i++) {
		c = grp_cpus[g][i];
		sidle = true;
		for (s = 0; s < core_nthr[cpu_core[c]]; s++)
			if (core_cpu[cpu_core[c]][s] != c &&
			    grp_load[core_cpu[cpu_core[c]][s]] >= C.kp.idle_us)
				sidle = false;
		/* score: load, a busy sibling counts a quarter quantum */
		sc = grp_load[c] + (sidle ? 0 : 250) + (c == me ? 0 : 1);
		if (sc < best - 0.5) {
			best = sc;
			bc = c;
			ties = 1;
		} else if (sc < best + 0.5 && rnd64(&gs) % (uint64_t)++ties == 0)
			bc = c;
	}
	if (bc != me && !queue_ok(&qq, bc))
		bc = me;
	return (bc);
}

/* P5 */
static struct kp_pick lastpick;
static int64_t	pending_retries;

/*
 * The resubmitting worker's load without the item it just finished: its
 * lists and what is left of its pass (the kernel has both at hand in the
 * handler: the figure of the pass start less the pass's done items).
 */
static u_int
self_load(int c)
{
	struct cpu *p = &cpus[c];
	struct qlist *l;
	double b = 0, w;
	int k, q;

	for (k = 0; k < p->rn; k++) {
		q = p->ring[(p->rhead + k) % nq];
		l = QL(q, c);
		w = (double)(l->depth + (p->pq == q ? p->bn : 0)) * l->cost_e;
		b += w < C.quantum ? w : C.quantum;
	}
	return (kp_load_us((uint64_t)b, (u_int)(p->share_e * 1024),
	    C.busyterm ? (u_int)(p->busy_e * 1024) : 0, (u_int)(C.quantum / US)));
}

static void merge_at(int c, bool worker);

static int
pick_gossip(int q, int me, bool self)
{
	int qq = q;
	double cost = QL(q, me)->cost_e;

	merge_at(me, self);
	cur_cpu = me;
	if (C.verbose >= 2 && self && now > C.warm && now < C.warm + 2 * MS) {
		struct kp_core *kc = &blocks[cpu_core[me]];
		int j;

		fprintf(stderr, "t=%.0fus cpu %d self %u cost %.0f st:", now / US, me,
		    self_load(me), cost);
		for (j = 0; j < core_nthr[cpu_core[me]]; j++)
			fprintf(stderr, " %u", kp_st_load(kc->kc_st.ks_w[j]));
		fprintf(stderr, " tb:");
		for (j = 0; j < KP_TABLE_MAX; j++)
			if (kp_e_valid(kc->kc_tb.kt_e[j]))
				fprintf(stderr, " c%u/i%u/l%u/age%u", kp_e_core(kc->kc_tb.kt_e[j]),
				    kp_e_idle(kc->kc_tb.kt_e[j]), kp_e_load(kc->kc_tb.kt_e[j]),
				    KP_T2US(kpnow() - kp_e_time(kc->kc_tb.kt_e[j])));
		fprintf(stderr, " merges %ju ival %u\n", (uintmax_t)kc->kc_ctl.kx_merges,
		    KP_T2US(kc->kc_ctl.kx_ival));
	}
	return (kp_place(&T, me, kpnow(), (uint64_t)cost,
	    self && C.selfstep ? (int)self_load(me) : -1, &C.kp, &cpus[me].rng,
	    queue_ok, &qq, &lastpick));
}

/* ------------------------------------------------------------------ */
/* Producers */

enum { PK_OPEN, PK_CLOSED };

struct prod {
	int		kind, q, fixed;
	int		home, *set, nset;
	double		rate;		/* items per ns (open) */
	bool		on;
	int		inflight;
	double		think, other_ns;
	int		other_place;	/* 0 completion CPU, 1 two choices, 2 the worker resubmits */
	double		c0, c1, plarge, alpha;
	uint64_t	issued, done;
	uint64_t	epoch;		/* bumps when the arrival process is restarted */
};

static struct prod P[MAXPROD];
static int	nprod;
static int	stealq[MAXQ], nsteal;

static double
draw_cost(struct prod *pr)
{
	double x;

	if (pr->alpha > 1) {
		double xm = pr->c0 * (pr->alpha - 1) / pr->alpha, u = rndu(&gs);

		x = xm / pow(u > 0 ? u : 1e-12, 1 / pr->alpha);
		return (x < C.quantum ? x : C.quantum);
	}
	x = rndu(&gs) < pr->plarge ? pr->c1 : pr->c0;
	if (C.expcost)
		return (rndexp(&gs, x));
	return (x * (0.9 + 0.2 * rndu(&gs)));
}

/* ------------------------------------------------------------------ */
/* Workers */

static void	enqueue_item(int32_t it, int from, int fixed, bool self);
static void	issue(int pi, int from, bool self);

static void
ring_add(int c, int q)
{
	struct cpu *p = &cpus[c];

	p->ring[(p->rhead + p->rn) % nq] = q;
	p->rn++;
	QL(q, c)->active = true;
	QL(q, c)->fresh = true;
}

static void
ring_pop(int c)
{
	struct cpu *p = &cpus[c];

	QL(p->ring[p->rhead], c)->active = false;
	p->rhead = (p->rhead + 1) % nq;
	p->rn--;
}

static void
ring_rotate(int c)
{
	struct cpu *p = &cpus[c];
	int q = p->ring[p->rhead];

	p->rhead = (p->rhead + 1) % nq;
	p->ring[(p->rhead + p->rn - 1) % nq] = q;
	QL(q, c)->fresh = true;
}

static void	steal_wake(int c, int from, double *cost);
static void	mark_moved(int32_t h);

/*
 * P7: a CPU whose stealable backlog is at or above H flags itself in
 * the donor line (written on the transitions only) and names its
 * longest stealable list in a hint line of its own; becoming a donor it
 * wakes the nearest sleeping worker, which steals from it.
 */
static void
donor_point(int c)
{
	struct cpu *p = &cpus[c];
	struct qlist *l;
	double b = 0, share = p->share_e > 1.0 / 16 ? p->share_e : 1.0 / 16, cost = 0;
	u_int bd = 0;
	int k, q, bq = NONE;
	bool d;

	p->dec_n++;
	p->starved = false;
	for (k = 0; k < p->rn; k++) {
		q = p->ring[(p->rhead + k) % nq];
		l = QL(q, c);
		if (!Q[q].stealable)
			continue;
		b += (double)l->depth * l->cost_e;
		if (l->depth > bd) {
			bd = l->depth;
			bq = q;
		}
	}
	/*
	 * donor_lone: a lone item waits behind the pass's batch and the item
	 * running; with that work it is a backlog too, for idle thieves
	 * (today's kwq_steal_lone_ok(): inside a pass).  Longer lists count
	 * their own work only: with the work ahead, short lists of a
	 * saturated machine hopped from CPU to CPU (S15).
	 */
	if (C.donor_lone && bd == 1 && (p->bn > 0 || p->bj == J_ITEM)) {
		double rem = p->bj == J_ITEM ? p->brem - (p->ws == W_RUN ?
		    p->brate * (now - p->last_t) : 0) : 0;

		b += p->bwork + (rem > 0 ? rem : 0);
		d = b / share >= C.donor_us * US;
	} else
		d = bd >= 2 && b / share >= C.donor_us * US;
	if (d && bq != p->dq) {
		cost += line_write(c, L_DHINT + c);
		p->dq = bq;
	}
	if (d != p->donor) {
		cost += line_write(c, L_DONOR) + C.c_atomic;
		p->donor = d;
		if (d)
			steal_wake(c, c, &cost);
	}
	charge_bulk(c, cost);
}

/*
 * P8: a per-CPU tick, armed while the CPU holds stealable work.  At
 * softclock priority it runs when the worker gets no time; a worker
 * that reached no decision point since the tick was armed is starved,
 * and the tick flags it a donor (lone items included) and wakes a
 * thief, every tick until the worker runs again (S12).
 */
static void
watch_arm(int c)
{
	struct cpu *p = &cpus[c];

	if (C.policy != P_SHARD || C.watch_us <= 0 || p->watch_armed)
		return;
	p->watch_armed = true;
	p->watch_n = p->dec_n;
	ev_push(now + C.watch_us * US, EV_WATCH, c, 0);
}

static void
watch_fire(int c)
{
	struct cpu *p = &cpus[c];
	struct qlist *l;
	double cost = C.timer_ns;
	u_int bd = 0;
	int k, q, bq = NONE;

	p->watch_armed = false;
	for (k = 0; k < p->rn; k++) {
		q = p->ring[(p->rhead + k) % nq];
		l = QL(q, c);
		if (Q[q].stealable && l->depth > bd) {
			bd = l->depth;
			bq = q;
		}
	}
	if (bq == NONE) {
		charge_other(c, cost);
		return;
	}
	if (p->dec_n == p->watch_n) {
		if (!p->donor || p->dq != bq) {
			cost += line_write(c, L_DHINT + c);
			p->dq = bq;
		}
		if (!p->donor)
			cost += line_write(c, L_DONOR) + C.c_atomic;
		p->donor = p->starved = true;
		steal_wake(c, c, &cost);
	}
	charge_other(c, cost);
	watch_arm(c);
}

/* P7's idle path: the donors in reach, nearest first; one line each. */
static double	room_us = -1;	/* donor_pull(): the puller's room below L */

static bool
steal_donor(int c, double *costp)
{
	int reach = order_n[c] < (int)C.steal_scan ? order_n[c] : (int)C.steal_scan;
	int i, v, q;
	double cost = 0, w;
	struct qlist *l, *dl;
	u_int take;
	int32_t h, t;

	if (warm())
		M.steal_tries++;
	cost += line_read(c, L_DONOR);
	for (i = 0; i < reach; i++) {
		v = order[c][i];
		if (!cpus[v].donor)
			continue;
		cost += line_read(c, L_DHINT + v);
		q = cpus[v].dq;
		if (q == NONE)
			continue;
		l = QL(q, v);
		cost += line_write(c, LISTLINE(q, v)) + C.c_atomic;
		/*
		 * A lone item: from a starved donor, or (donor_lone) behind
		 * the donor's running work, for an idle thief only; a puller
		 * is busy itself, and in saturation pullers passed a lone item
		 * from tail to tail (crypto q8x4: 144 items moved 4 times).
		 */
		if (l->depth < (cpus[v].starved || (C.donor_lone && room_us < 0 &&
		    (cpus[v].bn > 0 || cpus[v].bj == J_ITEM)) ? 1u : 2u))
			continue;
		take = cpus[v].starved ? l->depth : (l->depth + 1) / 2;
		if (take > C.steal_max)
			take = C.steal_max;
		/*
		 * No more than leaves the taker below a donor's backlog (and
		 * a puller below L), so a batch is not taken on again before
		 * new arrivals lift its holder (B4).
		 */
		if (C.donor_bound && l->cost_e > 0) {
			double room = (room_us >= 0 ? room_us : C.donor_us) * US / l->cost_e;
			u_int lim = room > 2 ? (u_int)room - 1 : 1;

			if (take > lim)
				take = lim;
		}
		h = list_cut(l, take, &t, &w);
		cpus[v].qwork -= w;
		mark_moved(h);
		dl = QL(q, c);
		cost += line_write(c, LISTLINE(q, c)) + C.c_atomic;
		list_append_chain(dl, h, t, take, w);
		cpus[c].qwork += w;
		if (!dl->active)
			ring_add(c, q);
		if (warm())
			M.steals++;
		/* the donor still holds H or more: one more helper (a cascade) */
		if (C.donor_cascade && (double)l->depth * l->cost_e >= C.donor_us * US)
			steal_wake(v, c, &cost);
		*costp = cost;
		cpus[c].t_scheme += cost;
		return (true);
	}
	*costp = cost;
	cpus[c].t_scheme += cost;
	return (false);
}

/*
 * P7/P8: a worker whose own backlog has fallen below L pulls from a
 * donor at the end of a pass, before it runs dry (the hand-off's
 * threshold, from the other side; one read of the donor line, cached
 * while no CPU changes its donor state).
 */
static void
donor_pull(int c)
{
	struct cpu *p = &cpus[c];
	double b = 0, cost = 0, share;
	int k;

	if ((C.policy != P_DONOR && C.policy != P_SHARD) || !C.donor_pull || nsteal == 0)
		return;
	for (k = 0; k < p->rn; k++)
		b += (double)QL(p->ring[(p->rhead + k) % nq], c)->depth *
		    QL(p->ring[(p->rhead + k) % nq], c)->cost_e;
	share = p->share_e > 1.0 / 16 ? p->share_e : 1.0 / 16;
	if (b / share >= (double)C.kp.l_us * US)
		return;
	room_us = C.kp.l_us - b / share / US;
	(void)steal_donor(c, &cost);
	room_us = -1;
	charge_bulk(c, cost);
	cpus[c].t_scheme -= cost;	/* steal_donor() counted it */
}

/* P5's load figure at a decision point (S2.3); P4 publishes the same figure. */
static void
load_point(int c)
{
	struct cpu *p = &cpus[c];
	struct qlist *l;
	double b = 0, w;
	uint64_t word;
	u_int us;
	int k, q;

	if (C.policy == P_DONOR || C.policy == P_SHARD) {
		donor_point(c);
		return;
	}
	if (C.policy != P_GOSSIP && C.policy != P_GROUP)
		return;
	for (k = 0; k < p->rn; k++) {
		q = p->ring[(p->rhead + k) % nq];
		l = QL(q, c);
		w = (double)(l->depth + (p->pq == q ? p->bn : 0)) * l->cost_e;
		b += w < C.quantum ? w : C.quantum;
	}
	us = kp_load_us((uint64_t)b, (u_int)(p->share_e * 1024),
	    C.busyterm ? (u_int)(p->busy_e * 1024) : 0, (u_int)(C.quantum / US));
	cur_cpu = c;
	cur_cost = 0;
	if (kp_load_update(&p->kl, kpnow(), us, &C.kp, &word)) {
		if (C.policy == P_GOSSIP)
			KP_STORE64(&blocks[cpu_core[c]].kc_st.ks_w[cpu_slot[c]], word);
		else
			grp_update(c, us);
	}
	charge_bulk(c, cur_cost);
}

static void
merge_at(int c, bool worker)
{
	struct kp_mres r;
	int k = cpu_core[c];

	if (C.policy != P_GOSSIP || !kp_merge_due(&T, (u_int)k, kpnow()))
		return;
	cur_cpu = c;
	cur_cost = 0;
	cur_cost += line_write(c, L_BLOCK + 3 * k + 2);	/* the try-flag */
	if (!KP_TRYFLAG(&blocks[k].kc_ctl))
		return;
	kp_merge(&T, (u_int)k, kpnow(), &C.kp, &r);
	KP_CLRFLAG(&blocks[k].kc_ctl);
	cur_cost += C.merge_ns;
	if (warm())
		M.merges++;
	if (worker)
		charge_bulk(c, cur_cost);
	else
		charge_other(c, cur_cost);
}

/* Something was placed on or handed to c: M4's discovery ends. */
static void
discovered(int c)
{
	struct cpu *p = &cpus[c];
	int h;

	if (p->idle_seen <= 0)
		return;
	if (warm()) {
		h_add(&M.disc, now - p->idle_seen);
		h = p->idle_hops < 31 ? p->idle_hops : 31;
		if (h >= 0) {
			M.disc_hop_sum[h] += now - p->idle_seen;
			M.disc_hop_n[h]++;
			if (now - p->idle_seen > M.disc_hop_max[h])
				M.disc_hop_max[h] = now - p->idle_seen;
		}
	}
	p->idle_seen = 0;
}

static double
wake_lat(double slept)
{
	return (slept < C.wl_thresh ? C.wl_short : slept < C.wl_deep_thresh ?
	    C.wl_long : C.wl_deep);
}

static void
wake_worker(int c)
{
	struct cpu *p = &cpus[c];

	if (p->ws != W_SLEEP)
		return;
	p->ws = W_WAKING;
	p->pubseq++;
	ev_push(now + wake_lat(now - p->sleep_t), EV_WAKE, c, 0);
}

/* P1: does a BULK worker near c sleep (the idle mask)? */
static bool
idle_near_p1(int c)
{
	int reach = order_n[c] < (int)C.steal_scan ? order_n[c] : (int)C.steal_scan, i;

	if (C.policy == P_SHARD) {
		/* the own group's line: a helper nearby is one in the group */
		charge_bulk(c, line_read(c, idle_line(c)));
		for (i = 0; i < reach; i++)
			if (shard_of(order[c][i]) == shard_of(c) && cpus[order[c][i]].inmask)
				return (true);
		return (false);
	}
	charge_bulk(c, line_read(c, L_IDLE));
	for (i = 0; i < reach; i++)
		if (cpus[order[c][i]].inmask)
			return (true);
	return (false);
}

static bool
steal_crossed(u_int old, u_int new)
{
	u_int t = C.steal_wake_at;

	if (t == 0 || new < t)
		return (false);
	while (t <= old && t < (1u << 31))
		t <<= 1;
	return (t > old && t <= new);
}

static void
steal_wake(int c, int from, double *cost)
{
	int reach = order_n[c] < (int)C.steal_scan ? order_n[c] : (int)C.steal_scan, i, v;

	*cost += line_read(from, C.policy == P_SHARD ? idle_line(c) : L_IDLE);
	for (i = 0; i < reach; i++) {
		v = order[c][i];
		if (!cpus[v].inmask || (C.policy == P_SHARD && shard_of(v) != shard_of(c)))
			continue;
		*cost += line_write(from, L_WMTX + v) + C.c_atomic;
		if (cpus[v].ws == W_SLEEP) {
			wake_worker(v);
			return;
		}
	}
}

static void
mark_moved(int32_t h)
{
	for (; h != NONE; h = items[h].next)
		if (items[h].moves < 255)
			items[h].moves++;
}

/* P1: the idle-path steal (kwq_steal()) */
static bool
steal(int c, double *costp)
{
	int reach = order_n[c] < (int)C.steal_scan ? order_n[c] : (int)C.steal_scan;
	int i, j, v, q;
	double cost = 0, w;
	struct qlist *l, *dl;
	struct cpu *vp;
	u_int n, take;
	int32_t h, t;

	if (warm())
		M.steal_tries++;
	for (i = 0; i < reach; i++) {
		v = order[c][i];
		vp = &cpus[v];
		for (j = 0; j < nsteal; j++) {
			q = stealq[j];
			l = QL(q, v);
			cost += C.scan_entry + line_read(c, LISTLINE(q, v));
			n = l->depth;
			/* kwq_steal_lone_ok(): asleep, or inside a pass */
			if (n == 0 || (n == 1 && !(vp->ws == W_SLEEP || vp->bj == J_ITEM ||
			    vp->pq != NONE)))
				continue;
			take = n == 1 ? 1 : (n + 1) / 2;
			if (take > C.steal_max)
				take = C.steal_max;
			cost += line_write(c, LISTLINE(q, v)) + C.c_atomic;
			h = list_cut(l, take, &t, &w);
			vp->qwork -= w;
			mark_moved(h);
			dl = QL(q, c);
			cost += line_write(c, LISTLINE(q, c)) + C.c_atomic;
			list_append_chain(dl, h, t, take, w);
			cpus[c].qwork += w;
			if (!dl->active)
				ring_add(c, q);
			if (warm())
				M.steals++;
			cpus[c].t_scheme += cost;
			*costp = cost;
			return (true);
		}
	}
	cpus[c].t_scheme += cost;
	*costp = cost;
	return (false);
}

/* The worker's own time outside a pass, as a job of its own. */
static void
start_ovh(int c, double ns)
{
	struct cpu *p = &cpus[c];

	thread_advance(c);
	p->bj = J_OVH;
	p->bitem = NONE;
	p->brem = ns;
	p->bcpu = 0;
	RESCHED(c);
}

static int	last_ho_to[MAXCPU];
static double	last_ho_t[MAXCPU];

/* P5: hand-off at the end of a pass (S2.7) */
static bool handoff_one(int c);

static void
handoff_check(int c)
{
	int i;

	for (i = 0; i < C.handoff_n && handoff_one(c); i++)
		;
}

static bool
handoff_one(int c)
{
	struct cpu *p = &cpus[c];
	struct kp_pick pk;
	struct qlist *l, *dl;
	double b = 0, w, share;
	int k, q, bq = NONE, t;
	u_int bd = 0, take;
	int32_t h, tl;

	if (C.policy != P_GOSSIP || !C.handoff || nsteal == 0)
		return (false);
	for (k = 0; k < p->rn; k++) {
		q = p->ring[(p->rhead + k) % nq];
		l = QL(q, c);
		b += (double)l->depth * l->cost_e;
		if (Q[q].stealable && l->depth > bd) {
			bd = l->depth;
			bq = q;
		}
	}
	share = p->share_e > 1.0 / 16 ? p->share_e : 1.0 / 16;
	if (bq == NONE || bd < 2 || b / share < (double)C.kp.h_us * US)
		return (false);
	cur_cpu = c;
	cur_cost = 0;
	t = kp_handoff_target(&T, c, kpnow(), &C.kp, &p->rng, &pk);
	if (t < 0 || t == c) {
		charge_bulk(c, cur_cost);
		return (false);
	}
	l = QL(bq, c);
	take = kp_handoff_batch(l->depth, (uint64_t)l->cost_e, C.steal_max, &C.kp);
	if (take >= l->depth)
		take = l->depth / 2 > 0 ? l->depth / 2 : 1;
	cur_cost += line_write(c, LISTLINE(bq, c)) + C.c_atomic;
	h = list_cut(l, take, &tl, &w);
	p->qwork -= w;
	mark_moved(h);
	dl = QL(bq, t);
	cur_cost += line_write(c, LISTLINE(bq, t)) + C.c_atomic;
	list_append_chain(dl, h, tl, take, w);
	cpus[t].qwork += w;
	if (!dl->active)
		ring_add(t, bq);
	if (warm()) {
		M.handoffs++;
		if (last_ho_to[t] == c && now - last_ho_t[t] < 10 * MS)
			M.pingpong++;
	}
	last_ho_to[c] = t;
	last_ho_t[c] = now;
	p->handoffs_out++;
	cpus[t].handoffs_in++;
	discovered(t);
	wake_worker(t);
	charge_bulk(c, cur_cost);
	return (true);
}

/*
 * A sleeping worker with a fall pending sleeps until the hold has passed
 * (rounded up) and publishes then; woken early, it sleeps again for the
 * rest.  The kernel's timed sleep needs the same re-arm: a callout may
 * fire a little before its time.
 */
static void
arm_publish(int c)
{
	struct cpu *p = &cpus[c];
	kp_time_t left;

	if (C.policy != P_GOSSIP || (left = kp_load_hold_left(&p->kl, kpnow(), &C.kp)) == 0)
		return;
	p->pubseq++;
	ev_push(now + ceil((double)left * (SEC / 1048576.0)) + 1, EV_PUBLISH, c, p->pubseq);
	charge_bulk(c, C.timer_ns);
}

static void
go_sleep(int c)
{
	struct cpu *p = &cpus[c];

	load_point(c);
	arm_publish(c);
	if (C.policy == P_TODAY || C.policy == P_NOSTEAL || C.policy == P_DONOR ||
	    C.policy == P_SHARD) {
		p->inmask = true;
		charge_bulk(c, line_write(c, idle_line(c)) + C.c_atomic);
	}
	p->ws = W_SLEEP;
	p->sleep_t = now;
	RESCHED(c);
}

/* Start a pass on the DRR ring's front queue; false if it had nothing. */
static bool
pass_start(int c)
{
	struct cpu *p = &cpus[c];
	int q = p->ring[p->rhead];
	struct qlist *l = QL(q, c);
	u_int take;
	bool helper;

	if (l->depth == 0) {
		ring_pop(c);
		return (false);
	}
	if (l->fresh) {
		l->deficit = (l->deficit < 0 ? l->deficit : 0) + C.quantum;
		l->fresh = false;
	}
	if (l->deficit <= 0) {
		ring_rotate(c);
		return (false);
	}
	take = l->depth;
	if (Q[q].stealable && take > 1 && (C.policy == P_TODAY || C.policy == P_DONOR ||
	    C.policy == P_SHARD || (C.policy == P_GOSSIP && C.handoff))) {
		if (C.policy == P_TODAY || C.policy == P_DONOR || C.policy == P_SHARD)
			helper = idle_near_p1(c);
		else {
			cur_cpu = c;
			cur_cost = 0;
			helper = kp_idle_near(&T, c, kpnow(), &C.kp);
			charge_bulk(c, cur_cost);
		}
		if (take > (helper ? 1 : C.steal_batch))
			take = helper ? 1 : C.steal_batch;
	}
	charge_bulk(c, line_write(c, LISTLINE(q, c)) + C.c_atomic);
	p->bhead = list_cut(l, take, &p->btail, &p->bwork);
	p->bn = take;
	p->pq = q;
	p->tried = false;
	load_point(c);
	return (true);
}

static void
pass_end(int c)
{
	struct cpu *p = &cpus[c];
	int q = p->pq;
	struct qlist *l = QL(q, c);

	if (p->bn > 0)
		list_prepend_chain(l, p->bhead, p->btail, p->bn, p->bwork);
	p->bhead = p->btail = NONE;
	p->bn = 0;
	p->bwork = 0;
	p->pq = NONE;
	if (l->depth > 0 && l->deficit > 0)
		;				/* another pass in this round */
	else if (l->depth > 0)
		ring_rotate(c);
	else {
		ring_pop(c);
		if (l->deficit > 0)
			l->deficit = 0;
	}
	load_point(c);
	handoff_check(c);
	merge_at(c, true);
	donor_pull(c);
}

static void
start_item(int c)
{
	struct cpu *p = &cpus[c];
	int32_t it = p->bhead;
	struct item *x = &items[it];
	double wait;

	p->bhead = x->next;
	if (p->bhead == NONE)
		p->btail = NONE;
	x->next = NONE;
	p->bn--;
	p->bwork -= x->cost;
	p->qwork -= x->cost;
	thread_advance(c);
	p->bj = J_ITEM;
	p->bitem = it;
	p->brem = x->cost + (cpu_core[c] != x->core0 ? C.move_ns : 0) + p->debt_b;
	p->debt_b = 0;
	p->bcpu = 0;
	if (warm() && x->t_first >= C.warm) {
		wait = now - x->t_first;
		h_add(&M.wait[x->cls], wait);
		h_add(&M.wait[2], wait);
	}
	RESCHED(c);
}

static void
worker_kick(int c)
{
	struct cpu *p = &cpus[c];
	int guard;

	for (guard = 0; guard < 1000000; guard++) {
		if (p->ws != W_RUN || p->bj != J_NONE)
			return;
		if (p->pq != NONE) {
			if (p->bn > 0 && QL(p->pq, c)->deficit > 0) {
				start_item(c);
				return;
			}
			pass_end(c);
			continue;
		}
		if (p->rn > 0) {
			(void)pass_start(c);
			continue;
		}
		if ((C.policy == P_TODAY || C.policy == P_DONOR || C.policy == P_SHARD) &&
		    !p->tried && nsteal > 0) {
			double cost = 0;

			p->tried = true;
			if (C.policy == P_DONOR || C.policy == P_SHARD)
				(void)steal_donor(c, &cost);
			else
				(void)steal(c, &cost);
			if (cost >= 100) {
				start_ovh(c, cost);
				return;
			}
			charge_bulk(c, cost);
			cpus[c].t_scheme -= cost;
			continue;
		}
		p->tried = false;
		go_sleep(c);
		return;
	}
	fprintf(stderr, "worker_kick: no progress on cpu %d\n", c);
	abort();
}

static int
two_least_busy(void)
{
	int a, b, la, lb;

	do a = (int)(rnd64(&gs) % (uint64_t)maxid); while (!cpu_present[a]);
	do b = (int)(rnd64(&gs) % (uint64_t)maxid); while (!cpu_present[b]);
	la = cpus[a].on + cpus[a].hog * 4 + (cpus[a].bj != J_NONE);
	lb = cpus[b].on + cpus[b].hog * 4 + (cpus[b].bj != J_NONE);
	return (la <= lb ? a : b);
}

static void
item_done(int c)
{
	struct cpu *p = &cpus[c];
	int32_t it = p->bitem;
	struct item *x = &items[it];
	struct qlist *l = QL(x->q, c);
	struct prod *pr = &P[x->prod];
	int pi = x->prod, q = x->q;

	thread_advance(c);
	l->deficit -= p->bcpu;
	l->cost_e += (p->bcpu - l->cost_e) / 8;
	p->bj = J_NONE;
	if (warm() && x->t_first >= C.warm) {
		M.ops++;
		M.work += x->cost;
		Q[q].done_work += x->cost;
		Q[q].done++;
		M.moves[x->moves < 4 ? x->moves : 4]++;
	}
	pr->done++;
	item_free(it);
	RESCHED(c);
	if (pr->kind == PK_CLOSED && pr->on) {
		if (pr->other_place == 2) {
			if (pr->think > 0)
				ev_push(now + pr->think, EV_ISSUE, pi, (uint64_t)c);
			else
				issue(pi, c, true);
		} else
			other_add(pr->other_place == 0 ? c : two_least_busy(),
			    pr->other_ns > 0 ? pr->other_ns : 1, pi);
	}
	worker_kick(c);
}

static void
other_done(int c)
{
	struct cpu *p = &cpus[c];
	int32_t ch = p->ohead;
	int32_t pi = chunks[ch].prod;

	thread_advance(c);
	p->ohead = chunks[ch].next;
	if (p->ohead == NONE)
		p->otail = NONE;
	chunks[ch].next = freechunk;
	freechunk = ch;
	if (--p->on > 0)
		p->orem = chunks[p->ohead].work;
	RESCHED(c);
	if (pi != NONE && P[pi].on) {
		if (P[pi].think > 0)
			ev_push(now + P[pi].think, EV_ISSUE, pi, (uint64_t)c);
		else
			issue(pi, c, false);
	}
}

/* ------------------------------------------------------------------ */
/* Enqueue: placement, the list, the doorbell */

static int16_t	herd_open[MAXCPU];
static double	herd_t0[MAXCPU];
static uint64_t	herd_src[MAXCPU][HOLD_W];
#define	HERD_W	(50 * US)

static bool
core_idle(int k)
{
	int i, c;

	for (i = 0; i < core_nthr[k]; i++) {
		c = core_cpu[k][i];
		if (cpus[c].ws != W_SLEEP || thread_busy(c))
			return (false);
	}
	return (true);
}

static void
herd_close(int k)
{
	int i, n = 0;

	for (i = 0; i < HOLD_W; i++)
		n += __builtin_popcountll(herd_src[k][i]);
	if (warm())
		h_add(&M.herd, n);
	herd_open[k] = 0;
}

static void
herd_note(int t, int from)
{
	int k = cpu_core[t];

	if (herd_open[k] && now - herd_t0[k] > HERD_W)
		herd_close(k);
	if (!herd_open[k]) {
		if (!core_idle(k))
			return;
		herd_open[k] = 1;
		herd_t0[k] = now;
		memset(herd_src[k], 0, sizeof(herd_src[k]));
	}
	herd_src[k][from / 64] |= 1ULL << (from % 64);
}

static void
enqueue_item(int32_t it, int from, int fixed, bool self)
{
	struct item *x = &items[it];
	int q = x->q, t = from, i;
	struct qlist *l;
	bool any = fixed < 0, sample;
	double e, best;

	cur_cpu = from;
	cur_cost = C.place_ns;
	memset(&lastpick, 0, sizeof(lastpick));
	lastpick.step = 99;
	if (!any && C.policy == P_GOSSIP && C.spill && Q[q].stealable) {
		cur_cpu = from;
		/* the target's status, or the queue's own list there (the lock line) */
		cur_cost += line_read(from, LISTLINE(q, fixed));
		if (kp_spill(&T, fixed, &C.kp) || (double)QL(q, fixed)->depth *
		    QL(q, fixed)->cost_e >= (double)C.kp.h_us * US) {
			any = true;
			if (warm())
				M.spills++;
		}
	}
	if (!any && (C.policy == P_DONOR || C.policy == P_SHARD) && C.spill &&
	    Q[q].stealable) {
		cur_cost += line_read(from, LISTLINE(q, fixed)) + line_read(from, L_DONOR);
		if (cpus[fixed].donor || (double)QL(q, fixed)->depth * QL(q, fixed)->cost_e >=
		    C.donor_us * US) {
			any = true;
			if (warm())
				M.spills++;
		}
	}
	if (!any)
		t = fixed;
	else
		switch (C.policy) {
		case P_ORACLE: t = oracle(q, from, false); break;
		case P_ORACLE_L3: t = oracle(q, from, true); break;
		case P_TODAY: case P_NOSTEAL: case P_DONOR: t = pick_any(q, from); break;
		case P_SHARD: t = pick_shard(q, from); break;
		case P_TWO: t = pick_two(q, from); break;
		case P_GROUP: t = pick_group(q, from); break;
		case P_RANDOM:
			do t = (int)(rnd64(&gs) % (uint64_t)maxid); while (!cpu_present[t]);
			break;
		default: t = pick_gossip(q, from, self); break;
		}
	if (t < 0 || !cpu_present[t])
		t = from;
	cur_cpu = from;
	sample = warm() && (M.place % (maxid > 16 ? 16 : 4)) == 0;
	if (sample) {
		/* M2: regret against the least start delay at this instant */
		e = est_start(t, q);
		best = 1e18;
		for (i = 0; i < maxid; i++)
			if (cpu_present[i] && est_start(i, q) < best)
				best = est_start(i, q);
		M.sampled++;
		h_add(&M.regret, e - best);
		if (e - best <= fmax(50 * US, 0.1 * x->cost)) {
			M.good++;
			if (nser < SERIES_MAX && (int)((now - C.warm) / MS) < SERIES_MAX)
				ser[(int)((now - C.warm) / MS)].good++;
		}
		if ((int)((now - C.warm) / MS) < SERIES_MAX)
			ser[(int)((now - C.warm) / MS)].place++;
	}
	l = QL(q, t);
	cur_cost += line_write(from, LISTLINE(q, t)) + C.c_atomic;
	if (l->depth >= C.limit) {
		if (warm())
			M.refused++;
		ev_push(now + 1 * MS, EV_RETRY, it, (uint64_t)from);
		pending_retries++;
		if (self)
			charge_bulk(from, cur_cost);
		else
			charge_other(from, cur_cost);
		return;
	}
	if (warm()) {
		M.place++;
		M.local[cdist(from, t)]++;
		if (C.policy == P_GOSSIP && any && lastpick.step < 8) {
			M.steps[lastpick.step]++;
			M.checks += lastpick.checks;
			M.fails += lastpick.fails;
			M.expired += lastpick.expired;
			if (lastpick.step == KP_STEP_IDLE_NEAR || lastpick.step == KP_STEP_TABLE)
				h_add(&M.age, KP_T2US(lastpick.age));
		}
	}
	herd_note(t, from);
	discovered(t);
	x->t_enq = now;
	list_append(l, it);
	cpus[t].qwork += x->cost;
	if (!l->active)
		ring_add(t, q);
	if (Q[q].stealable)
		watch_arm(t);
	if ((C.policy == P_TODAY || ((C.policy == P_DONOR || C.policy == P_SHARD) &&
	    C.donor_wakes)) && Q[q].stealable && steal_crossed(l->depth - 1, l->depth))
		steal_wake(t, from, &cur_cost);
	if (self)
		charge_bulk(from, cur_cost);
	else
		charge_other(from, cur_cost);
	if (cpus[t].ws == W_SLEEP)
		wake_worker(t);
}

static void
issue(int pi, int from, bool self)
{
	struct prod *pr = &P[pi];
	int32_t it = item_new();
	struct item *x = &items[it];

	x->q = (int16_t)pr->q;
	x->prod = pi;
	x->src = (int16_t)from;
	x->core0 = (int16_t)cpu_core[from];
	x->cost = draw_cost(pr);
	x->cls = x->cost >= C.kp.csmall_ns;
	x->t_first = now;
	pr->issued++;
	Q[pr->q].offered += x->cost;
	enqueue_item(it, from, pr->fixed, self);
}

static int
prod_home(struct prod *pr)
{
	if (pr->nset > 0)
		return (pr->set[rnd64(&gs) % (uint64_t)pr->nset]);
	return (pr->home);
}

/* ------------------------------------------------------------------ */
/* The sampler: M3 idle-while-waiting, M4 discovery's start, the series */

#define	SAMPLE	(20 * US)
#define	WAITED	(100 * US)

static bool
cpu_waiting(int c)
{
	struct cpu *p = &cpus[c];
	int k, q;
	struct qlist *l;

	if (p->bn > 0 && now - items[p->bhead].t_enq > WAITED)
		return (true);
	for (k = 0; k < p->rn && k < 16; k++) {
		q = p->ring[(p->rhead + k) % nq];
		l = QL(q, c);
		if (l->head != NONE && now - items[l->head].t_enq > WAITED)
			return (true);
	}
	return (false);
}

static void
sample(void)
{
	static bool waitc[MAXCPU];
	int idle_l3[MAXCPU] = { 0 }, wait_l3[MAXCPU] = { 0 }, nidle = 0, nwait = 0;
	int c, k, j, h;
	double idle_w = 0;

	for (c = 0; c < maxid; c++) {
		if (!cpu_present[c])
			continue;
		waitc[c] = cpu_waiting(c);
		if (waitc[c]) {
			wait_l3[cpu_l3[c]]++;
			nwait++;
		}
		if (cpus[c].ws == W_SLEEP && !thread_busy(c)) {
			idle_l3[cpu_l3[c]]++;
			nidle++;
		}
	}
	for (k = 0; k < nl3; k++)
		if (wait_l3[k] > 0)
			idle_w += idle_l3[k];
	M.idle_l3 += idle_w * SAMPLE;
	if (nwait > 0)
		M.idle_any += nidle * SAMPLE;
	M.cap_t += ncpu * SAMPLE;
	if ((int)((now - C.warm) / MS) < SERIES_MAX)
		ser[(int)((now - C.warm) / MS)].idle += (float)(idle_w * SAMPLE);
	/* discovery starts for idle CPUs while a backlog waits */
	for (c = 0; c < maxid && nwait > 0; c++) {
		if (!cpu_present[c] || cpus[c].ws != W_SLEEP || thread_busy(c) ||
		    cpus[c].idle_seen > 0)
			continue;
		cpus[c].idle_seen = now;
		h = 1 << 30;
		for (j = 0; j < maxid; j++)
			if (cpu_present[j] && waitc[j] &&
			    hops[cpu_core[c]][cpu_core[j]] < h)
				h = hops[cpu_core[c]][cpu_core[j]];
		cpus[c].idle_hops = h;
	}
	if (nwait == 0)
		for (c = 0; c < maxid; c++)
			cpus[c].idle_seen = 0;
}

/* ------------------------------------------------------------------ */
/* Scenarios (S3.5) */

static int	scen_id;
static int	region[MAXCPU];		/* scenario's region of a core: 0, 1, 2 */

static int
new_queue(bool stealable, double nominal)
{
	int q = nq++;

	assert(nq <= MAXQ);
	Q[q].stealable = stealable;
	Q[q].nominal = nominal;
	if (stealable)
		stealq[nsteal++] = q;
	return (q);
}

static int
new_prod(int kind, int q, int home)
{
	struct prod *pr = &P[nprod++];

	assert(nprod <= MAXPROD);
	memset(pr, 0, sizeof(*pr));
	pr->kind = kind;
	pr->q = q;
	pr->fixed = -1;
	pr->home = home;
	pr->on = true;
	pr->c0 = C.c_small;
	pr->c1 = C.c_large;
	pr->plarge = C.p_large;
	pr->alpha = C.pareto;
	pr->other_ns = C.other_ns;
	pr->think = C.think;
	pr->other_place = C.other_place;
	return (nprod - 1);
}

static double
mean_cost(void)
{
	if (C.pareto > 1)
		return (C.c_small);
	return (C.c_small * (1 - C.p_large) + C.c_large * C.p_large);
}

/* The per-thread capacity an open load of rho is computed against. */
static double
thread_cap(void)
{
	return (1.0);
}

static void
prod_start(int pi)
{
	struct prod *pr = &P[pi];
	int i;

	pr->epoch++;
	if (pr->kind == PK_OPEN && pr->rate > 0)
		ev_push(now + rndexp(&gs, 1 / pr->rate), EV_ARRIVE, pi, pr->epoch);
	else if (pr->kind == PK_CLOSED)
		for (i = 0; i < pr->inflight; i++)
			ev_push(now + rndu(&gs) * 10 * US, EV_ISSUE, pi, (uint64_t)prod_home(pr));
}

/* Open producers on the given cores, one per thread, at rho x their capacity. */
static void
open_on(bool (*sel)(int core), double rho, bool stealable)
{
	int c, q, pi;

	for (c = 0, q = -1; c < maxid; c++) {
		if (!cpu_present[c] || !sel(cpu_core[c]))
			continue;
		if (!C.oneq || q < 0)
			q = new_queue(stealable, mean_cost());
		pi = new_prod(PK_OPEN, q, c);
		P[pi].rate = rho * thread_cap() / mean_cost();
	}
}

static bool sel_all(int k) { return (true); }
static bool sel_r0(int k) { return (region[k] == 0); }
static bool sel_r1(int k) { return (region[k] == 1); }

/* A block of k cores in torus coordinates at (r0, c0), clipped. */
static void
mark_block(int k, int r0, int c0, int val)
{
	int side = 1, i, n = 0, r, c, idx;

	while (side * side < k)
		side++;
	for (r = 0; r < side && n < k; r++)
		for (c = 0; c < side && n < k; c++) {
			idx = ((r0 + r) % (int)T.kt_rows) * (int)T.kt_cols +
			    ((c0 + c) % (int)T.kt_cols);
			if (idx >= ncores_t)
				continue;
			region[idx] = val;
			n++;
		}
	for (i = 0; n < k && i < ncores_t; i++)	/* clipped: fill in order */
		if (region[i] != val) {
			region[i] = val;
			n++;
		}
}

static void
set_hog(int c, bool on, double capx)
{
	thread_advance(c);
	cpus[c].hog = on;
	cpus[c].capx = capx;
	RESCHED(c);
}

static void
prod_rate(int pi, double rate, bool on)
{
	P[pi].on = on;
	P[pi].rate = rate;
	prod_start(pi);
}

static void
step_mark(void)
{
	if (nsteps < 64)
		steps_t[nsteps++] = now;
}

/* Phase handlers: a = phase number. */
static void
scen_phase(int a)
{
	int pi, c, k;

	switch (scen_id) {
	case 7:		/* return: on, off for the gap, then a step */
		for (pi = 0; pi < nprod; pi++) {
			if (a == 1)
				prod_rate(pi, 0, false);
			else
				prod_rate(pi, (C.factor > 0 ? C.factor : 0.9) *
				    (region[cpu_core[P[pi].home]] == 0 || C.k == 0 ? 1 : 0) /
				    mean_cost(), true);
		}
		if (a == 2)
			step_mark();
		break;
	case 8:		/* stop: region 1's producers stop */
		for (pi = 0; pi < nprod; pi++)
			if (region[cpu_core[P[pi].home]] == 1)
				prod_rate(pi, 0, false);
		step_mark();
		break;
	case 9:		/* moving hot spot: the producing core moves on */
		k = a % ncores_t;
		for (pi = 0; pi < nprod; pi++)
			P[pi].home = core_cpu[k][pi % core_nthr[k]];
		ev_push(now + C.period, EV_PHASE, a + 1, 0);
		step_mark();
		break;
	case 10:	/* on/off */
		for (pi = 0; pi < nprod; pi++) {
			bool on = ((a + (C.layout ? pi : 0)) % 2) == 0;

			prod_rate(pi, on ? 2 * C.rho / mean_cost() : 0, on);
		}
		ev_push(now + C.period / 2, EV_PHASE, a + 1, 0);
		break;
	case 11:	/* flash crowd */
		for (pi = 0; pi < nprod; pi++)
			for (k = 0; k < (int)C.burst; k++)
				issue(pi, prod_home(&P[pi]), false);
		step_mark();
		break;
	case 12:	/* rate collapse on core 0's threads, then recovery */
		for (k = 0; k < core_nthr[0]; k++)
			set_hog(core_cpu[0][k], a == 1, a == 1 ? C.share_drop : C.cap);
		if (a == 1)
			ev_push(now + C.drop_ms * MS, EV_PHASE, 2, 0);
		step_mark();
		break;
	case 13:	/* churning hogs: a new half every period */
		for (c = 0; c < maxid; c++)
			if (cpu_present[c])
				set_hog(c, (rnd64(&gs) & 1) != 0, C.cap);
		ev_push(now + C.period, EV_PHASE, a + 1, 0);
		break;
	case 15:	/* all loaded: phase 1 a hogged core is freed, 2 the load falls */
		if (a == 1)
			for (k = 0; k < core_nthr[0]; k++)
				set_hog(core_cpu[0][k], false, C.cap);
		else
			for (pi = 0; pi < nprod; pi++)
				prod_rate(pi, 0.9 / mean_cost(), true);
		step_mark();
		break;
	}
}

static void
scen_setup(void)
{
	int c, k, pi, q, i, n;
	static const char *names[] = { "uniform", "geli", "island", "idleisland",
	    "stripes", "deadband", "return", "stop", "moving", "onoff", "flash",
	    "collapse", "hogs", "smt", "full", "fixed", "crypto", NULL };
	static const int ids[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	    15, 16, 20 };

	for (i = 0; names[i] != NULL; i++)
		if (strcmp(C.scen, names[i]) == 0)
			scen_id = ids[i];
	if (scen_id == 0) {
		fprintf(stderr, "unknown scenario %s\n", C.scen);
		exit(2);
	}
	for (k = 0; k < ncores_t; k++)
		region[k] = 0;
	switch (scen_id) {
	case 1:		/* S1 uniform open load */
	case 14:	/* S14 the same, for the SMT sweeps */
		open_on(sel_all, C.rho, true);
		break;
	case 2:		/* S2 closed-loop GEOM-like providers */
		for (i = 0; i < C.extraq; i++)
			(void)new_queue(true, mean_cost());
		for (i = 0; i < C.nprov; i++) {
			q = new_queue(true, mean_cost());
			pi = new_prod(PK_CLOSED, q, -1);
			P[pi].inflight = C.inflight;
			P[pi].home = core_cpu[i % ncores_t][(i / ncores_t) % core_nthr[i % ncores_t]];
		}
		break;
	case 20:	/* kwq_crypto: queues resubmitted by the worker (V4) */
		for (i = 0; i < C.nprov; i++) {
			q = new_queue(true, mean_cost());
			pi = new_prod(PK_CLOSED, q, -1);
			P[pi].inflight = C.inflight;
			P[pi].other_place = 2;
			P[pi].home = core_cpu[i % ncores_t][0];
			if (C.fixed >= 0 && C.layout > 0)
				P[pi].fixed = C.fixed + i * C.layout;
			else if (C.fixed >= 0 && C.layout == 0 && C.k > 0)
				P[pi].fixed = C.fixed;
			if (P[pi].fixed >= 0)
				P[pi].home = P[pi].fixed;
		}
		break;
	case 3:		/* S3 a hot island of k cores at factor x capacity, ringed by idle */
		if (C.layout < 0)	/* across the wrap: from the last row and column */
			mark_block(C.k, (int)T.kt_rows - 1, (int)T.kt_cols - 1, 1);
		else
			mark_block(C.k, C.layout / 100, C.layout % 100, 1);
		open_on(sel_r1, C.factor, true);
		break;
	case 4:		/* S4 an idle island in a sea of load */
		for (k = 0; k < ncores_t; k++)
			region[k] = 1;
		mark_block(C.k, C.layout / 100, C.layout % 100, 0);
		open_on(sel_r1, C.factor > 0 ? C.factor : 1.2, true);
		break;
	case 5:		/* S5 stripes (layout 0 rows, 1 columns, 2 checkerboard) */
		for (k = 0; k < ncores_t; k++) {
			int r = T.kt_node[k].kn_row, cc = T.kt_node[k].kn_col;

			region[k] = C.layout == 0 ? r % 2 : C.layout == 1 ? cc % 2 :
			    (r + cc) % 2;
		}
		open_on(sel_r1, C.factor > 0 ? C.factor : 1.5, true);
		break;
	case 6:		/* S6 dead band: loaded | hogged, BULK asleep | idle, by columns */
		for (k = 0; k < ncores_t; k++) {
			int cc = T.kt_node[k].kn_col, w = (int)T.kt_cols;

			region[k] = cc < w / 3 ? 1 : cc < 2 * w / 3 ? 2 : 0;
		}
		open_on(sel_r1, C.factor > 0 ? C.factor : 1.5, true);
		for (c = 0; c < maxid; c++)
			if (cpu_present[c] && region[cpu_core[c]] == 2)
				cpus[c].hog = true;
		break;
	case 7:		/* S7 load coming back after an idle gap; k > 0: in an island */
		if (C.k > 0) {
			for (k = 0; k < ncores_t; k++)
				region[k] = 1;
			mark_block(C.k, 0, 0, 0);
		}
		open_on(sel_all, 0.5, true);
		ev_push(C.warm + 30 * MS, EV_PHASE, 1, 0);
		ev_push(C.warm + 30 * MS + C.gap, EV_PHASE, 2, 0);
		break;
	case 8:		/* S8 load stopping in a region */
		mark_block(C.k, 0, 0, 1);
		open_on(sel_all, C.rho > 0 ? C.rho : 0.9, true);
		ev_push(C.warm + 30 * MS, EV_PHASE, 1, 0);
		break;
	case 9:		/* S9 a moving hot spot: one core's producers at factor x */
		for (i = 0; i < (int)C.nthr; i++) {
			q = new_queue(true, mean_cost());
			pi = new_prod(PK_OPEN, q, core_cpu[0][i % core_nthr[0]]);
			P[pi].rate = C.factor / mean_cost();
		}
		ev_push(C.period, EV_PHASE, 1, 0);
		break;
	case 10:	/* S10 on/off with period, in phase (layout 0) or not (1) */
		open_on(sel_all, C.rho, true);
		ev_push(C.period / 2, EV_PHASE, 1, 0);
		break;
	case 11:	/* S11 flash crowd: burst items from k producers on CPU 0 */
		n = C.k > 0 ? C.k : 1;
		for (i = 0; i < n; i++) {
			q = new_queue(true, mean_cost());
			pi = new_prod(PK_OPEN, q, n == 1 ? 0 : core_cpu[i % ncores_t][0]);
			if (C.fixed >= 0)
				P[pi].fixed = C.fixed;
		}
		ev_push(C.warm + 5 * MS, EV_PHASE, 1, 0);
		break;
	case 12:	/* S12 rate collapse on core 0 while loaded */
		open_on(sel_all, C.rho > 0 ? C.rho : 0.7, true);
		ev_push(C.warm + 30 * MS, EV_PHASE, 1, 0);
		break;
	case 13:	/* S13 hogs on half the cores; period > 0: churning */
		open_on(sel_all, C.rho, true);
		for (c = 0; c < maxid; c++)
			if (cpu_present[c] && cpu_core[c] % 2 == 1)
				cpus[c].hog = true;
		if (C.layout == 1)
			ev_push(C.period, EV_PHASE, 1, 0);
		break;
	case 15:	/* S15 all loaded at rho, core 0 hogged at share 0, then freed */
		open_on(sel_all, C.rho > 0 ? C.rho : 1.2, true);
		if (C.layout == 1)	/* steady overload: no needle, no recovery */
			break;
		for (k = 0; k < core_nthr[0]; k++) {
			cpus[core_cpu[0][k]].hog = true;
			cpus[core_cpu[0][k]].capx = 0;
		}
		ev_push(C.warm + 30 * MS, EV_PHASE, 1, 0);
		ev_push(C.warm + 60 * MS, EV_PHASE, 2, 0);
		break;
	case 16:	/* S16 producers with a fixed CPU on stealable queues */
		for (c = 0; c < maxid; c++) {
			if (!cpu_present[c])
				continue;
			q = new_queue(true, mean_cost());
			pi = new_prod(PK_OPEN, q, c);
			P[pi].fixed = (c % 2 == 0 || C.layout == 1) ? (C.fixed >= 0 ? C.fixed : 0) : c;
			P[pi].rate = C.rho / mean_cost();
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* The run and the report */

static void
out(const char *metric, double v)
{
	printf("%s\t%s\t%d\t%ju\t%s\t%.6g\n", C.scen, C.topo, C.policy,
	    (uintmax_t)C.seed, metric, v);
}

static double
reaction(double t, double *m3)
{
	int w, i, ok = 0, w0 = (int)((t - C.warm) / MS);
	double g, idle;

	for (w = w0 > 0 ? w0 : 0; w < nser; w++) {
		g = ser[w].place ? (double)ser[w].good / ser[w].place : 1;
		idle = ser[w].idle / (ncpu * MS);
		if (g >= 0.95 && idle < 0.02) {
			if (++ok == 2)
				return ((w - 1 - w0) * MS);
		} else
			ok = 0;
		(void)i;
	}
	*m3 = 0;
	return (-1);
}

static void
report(void)
{
	static const char *cls[] = { "small", "large", "all" };
	double span = now - C.warm, bulk = 0, oth = 0, sch = 0, jn = 0, jd = 0, x, m3;
	char name[64];
	int i, c, nj = 0;

	for (c = 0; c < maxid; c++) {
		if (!cpu_present[c])
			continue;
		thread_advance(c);
		bulk += cpus[c].t_bulk;
		oth += cpus[c].t_other;
		sch += cpus[c].t_scheme;
	}
	bulk -= M.bulk0;
	oth -= M.oth0;
	sch -= M.sch0;
	for (i = 0; i < 3; i++) {
		if (M.wait[i].n == 0)
			continue;
		snprintf(name, sizeof(name), "wait_%s_mean_us", cls[i]);
		out(name, h_mean(&M.wait[i]) / US);
		snprintf(name, sizeof(name), "wait_%s_p50_us", cls[i]);
		out(name, h_q(&M.wait[i], 0.5) / US);
		snprintf(name, sizeof(name), "wait_%s_p99_us", cls[i]);
		out(name, h_q(&M.wait[i], 0.99) / US);
		snprintf(name, sizeof(name), "wait_%s_p999_us", cls[i]);
		out(name, h_q(&M.wait[i], 0.999) / US);
		snprintf(name, sizeof(name), "wait_%s_max_us", cls[i]);
		out(name, M.wait[i].max / US);
	}
	out("good_frac", M.sampled ? (double)M.good / M.sampled : 1);
	out("regret_p99_us", h_q(&M.regret, 0.99) / US);
	out("idle_wait_l3_frac", M.cap_t > 0 ? M.idle_l3 / M.cap_t : 0);
	out("idle_wait_any_frac", M.cap_t > 0 ? M.idle_any / M.cap_t : 0);
	out("disc_n", (double)M.disc.n);
	out("disc_p50_us", h_q(&M.disc, 0.5) / US);
	out("disc_p99_us", h_q(&M.disc, 0.99) / US);
	out("disc_max_us", M.disc.max / US);
	for (i = 0; i < 32; i++)
		if (M.disc_hop_n[i] > 0) {
			snprintf(name, sizeof(name), "disc_hop%d_max_us", i);
			out(name, M.disc_hop_max[i] / US);
			snprintf(name, sizeof(name), "disc_hop%d_mean_us", i);
			out(name, M.disc_hop_sum[i] / M.disc_hop_n[i] / US);
		}
	for (i = 0; i < nsteps; i++) {
		snprintf(name, sizeof(name), "react%d_ms", i);
		out(name, (x = reaction(steps_t[i], &m3)) >= 0 ? x / MS : -1);
	}
	out("herd_p999", h_q(&M.herd, 0.999));
	out("herd_max", M.herd.max);
	x = (double)(M.moves[0] + M.moves[1] + M.moves[2] + M.moves[3] + M.moves[4]);
	out("moved1_frac", x > 0 ? M.moves[1] / x : 0);
	out("moved2p_frac", x > 0 ? (M.moves[2] + M.moves[3] + M.moves[4]) / x : 0);
	out("moved4p", (double)M.moves[4]);
	out("handoffs_per_s_core", span > 0 ? M.handoffs / (span / SEC) / ncores_t : 0);
	out("pingpong", (double)M.pingpong);
	out("steal_tries_per_op", M.ops ? (double)M.steal_tries / M.ops : 0);
	out("steals_per_op", M.ops ? (double)M.steals / M.ops : 0);
	out("xfers_per_place", M.place ? (double)(st_xfers - M.xfers0) / M.place : 0);
	out("reads_per_place", M.place ? (double)(st_reads - M.reads0) / M.place : 0);
	out("line_waits_per_s", span > 0 ? st_waits / (now / SEC) : 0);
	out("line_wait_ns_per_place", M.place ? st_wait_ns / M.place : 0);
	out("scheme_cpu_frac", (bulk + oth) > 0 ? sch / (bulk + oth) : 0);
	out("merges_per_s_core", span > 0 ? M.merges / (span / SEC) / ncores_t : 0);
	out("age_p50_us", h_q(&M.age, 0.5));
	out("age_max_us", M.age.max);
	out("fail_frac", M.checks ? (double)M.fails / M.checks : 0);
	out("expired_used", (double)M.expired);
	for (i = 0; i < 8; i++)
		if (M.steps[i]) {
			snprintf(name, sizeof(name), "step%d_frac", i);
			out(name, (double)M.steps[i] / M.place);
		}
	out("local_self_frac", M.place ? (double)M.local[0] / M.place : 0);
	out("local_sib_frac", M.place ? (double)M.local[1] / M.place : 0);
	out("local_l3_frac", M.place ? (double)M.local[2] / M.place : 0);
	out("local_far_frac", M.place ? (double)(M.local[3] + M.local[4]) / M.place : 0);
	out("spills", (double)M.spills);
	out("ops_per_s", span > 0 ? M.ops / (span / SEC) : 0);
	out("work_frac", span > 0 ? M.work / (span * ncpu) : 0);
	out("cpu_per_op_us", M.ops ? (bulk + oth) / M.ops / US : 0);
	out("bulk_cpu_per_op_us", M.ops ? bulk / M.ops / US : 0);
	out("refused", (double)M.refused);
	for (i = 0; i < nq; i++)
		if (Q[i].offered > 0) {
			x = Q[i].done_work / Q[i].offered;
			jn += x;
			jd += x * x;
			nj++;
		}
	out("jain", nj > 0 && jd > 0 ? jn * jn / (nj * jd) : 1);
	{
		uint64_t issued = 0, done = 0, inq = 0, ho_in = 0, ho_out = 0;

		for (i = 0; i < nprod; i++) {
			issued += P[i].issued;
			done += P[i].done;
		}
		for (c = 0; c < maxid; c++) {
			if (!cpu_present[c])
				continue;
			inq += cpus[c].bn + (cpus[c].bj == J_ITEM);
			ho_in += cpus[c].handoffs_in;
			ho_out += cpus[c].handoffs_out;
			for (i = 0; i < nq; i++)
				inq += QL(i, c)->depth;
		}
		inq += (uint64_t)pending_retries;
		/* V2: conservation */
		if (issued != done + inq || ho_in != ho_out) {
			fprintf(stderr, "V2: issued %ju done %ju queued %ju; handoffs in %ju out %ju\n",
			    (uintmax_t)issued, (uintmax_t)done, (uintmax_t)inq,
			    (uintmax_t)ho_in, (uintmax_t)ho_out);
			st_violations++;
		}
	}
	out("violations", (double)st_violations);
	if (C.verbose)
		for (c = 0; c < maxid; c++)
			if (cpu_present[c])
				fprintf(stderr, "cpu %d: bulk %.1f%% other %.1f%% scheme %.3f%%\n", c,
				    100 * cpus[c].t_bulk / now, 100 * cpus[c].t_other / now,
				    100 * cpus[c].t_scheme / now);
}

static void
usage(void)
{
	printf("placesim key=value ...: topo=guest|lab|a07|t4|odd6|odd12|odd127 "
	    "policy=0|10|1|2|3|4|5 scen=uniform|geli|island|idleisland|stripes|"
	    "deadband|return|stop|moving|onoff|flash|collapse|hogs|smt|full|fixed|"
	    "crypto seed dur_ms warm_ms rho small_us large_us p_large pareto "
	    "other_us think_us nprov inflight k factor layout fixed period_ms gap_ms "
	    "burst share_drop drop_ms other_place smt_bulk smt_other cap move_us "
	    "steal_scan steal_batch sib_last shape=0|1|2 noself nosmt nocheck "
	    "norand symhyst fixival nosplit norepair handoff spill imin_us imax_us "
	    "texp_us hold_us h_us l_us ladv_us llocal_us csmall_us table checks "
	    "c_l3 c_sib scan_entry wl_short_us wl_long_us absent verbose series\n");
}

static void
parse(int argc, char **argv)
{
	int i;
	char *k, *v;
	double d;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "help") == 0) {
			usage();
			exit(0);
		}
		k = argv[i];
		v = strchr(k, '=');
		if (v == NULL) {
			fprintf(stderr, "not key=value: %s\n", k);
			exit(2);
		}
		*v++ = '\0';
		d = atof(v);
#define	KEY(n)	(strcmp(k, (n)) == 0)
		if (KEY("topo")) snprintf(C.topo, sizeof(C.topo), "%s", v);
		else if (KEY("scen")) snprintf(C.scen, sizeof(C.scen), "%s", v);
		else if (KEY("policy")) C.policy = (int)d;
		else if (KEY("seed")) C.seed = strtoull(v, NULL, 0);
		else if (KEY("dur_ms")) C.dur = d * MS;
		else if (KEY("warm_ms")) C.warm = d * MS;
		else if (KEY("rho")) C.rho = d;
		else if (KEY("small_us")) C.c_small = d * US;
		else if (KEY("large_us")) C.c_large = d * US;
		else if (KEY("p_large")) C.p_large = d;
		else if (KEY("pareto")) C.pareto = d;
		else if (KEY("other_us")) C.other_ns = d * US;
		else if (KEY("think_us")) C.think = d * US;
		else if (KEY("nprov")) C.nprov = (int)d;
		else if (KEY("inflight")) C.inflight = (int)d;
		else if (KEY("k")) C.k = (int)d;
		else if (KEY("factor")) C.factor = d;
		else if (KEY("layout")) C.layout = (int)d;
		else if (KEY("fixed")) C.fixed = (int)d;
		else if (KEY("period_ms")) C.period = d * MS;
		else if (KEY("gap_ms")) C.gap = d * MS;
		else if (KEY("burst")) C.burst = d;
		else if (KEY("share_drop")) C.share_drop = d;
		else if (KEY("drop_ms")) C.drop_ms = d;
		else if (KEY("other_place")) C.other_place = (int)d;
		else if (KEY("smt_bulk")) C.smt_bulk = d;
		else if (KEY("smt_other")) C.smt_other = d;
		else if (KEY("cap")) C.cap = d;
		else if (KEY("cap_period_us")) C.cap_period = d * US;
		else if (KEY("move_us")) C.move_ns = d * US;
		else if (KEY("steal_scan")) C.steal_scan = (u_int)d;
		else if (KEY("steal_batch")) C.steal_batch = (u_int)d;
		else if (KEY("steal_max")) C.steal_max = (u_int)d;
		else if (KEY("sib_last")) C.topo_sib_last = (int)d;
		else if (KEY("shape")) C.shape = (u_int)d;
		else if (KEY("noself")) C.selfstep = !(int)d;
		else if (KEY("nosmt")) C.kp.flags |= (int)d ? KP_F_NOSMT : 0;
		else if (KEY("nocheck")) C.kp.flags |= (int)d ? KP_F_NOCHECK : 0;
		else if (KEY("norand")) C.kp.flags |= (int)d ? KP_F_NORAND : 0;
		else if (KEY("symhyst")) C.kp.flags |= (int)d ? KP_F_SYMHYST : 0;
		else if (KEY("fixival")) C.kp.flags |= (int)d ? KP_F_FIXIVAL : 0;
		else if (KEY("nosplit")) C.kp.flags |= (int)d ? KP_F_NOSPLIT : 0;
		else if (KEY("norepair")) C.kp.flags |= (int)d ? KP_F_NOREPAIR : 0;
		else if (KEY("handoff")) C.handoff = (int)d;
		else if (KEY("handoff_n")) C.handoff_n = (int)d;
		else if (KEY("busyterm")) C.busyterm = (int)d;
		else if (KEY("contend")) C.contend = (int)d;
		else if (KEY("remerge")) C.kp.flags |= (int)d ? KP_F_REMERGE : 0;
		else if (KEY("noremerge")) C.kp.flags |= (int)d ? KP_F_NOREMERGE : 0;
		else if (KEY("tomb")) C.kp.flags |= (int)d ? KP_F_TOMB : 0;
		else if (KEY("group")) C.group = (int)d;
		else if (KEY("donor_cascade")) C.donor_cascade = (int)d;
		else if (KEY("donor_pull")) C.donor_pull = (int)d;
		else if (KEY("donor_wakes")) C.donor_wakes = (int)d;
		else if (KEY("donor_avoid")) C.donor_avoid = (int)d;
		else if (KEY("donor_us")) C.donor_us = d;
		else if (KEY("donor_bound")) C.donor_bound = (int)d;
		else if (KEY("watch_us")) C.watch_us = d;
		else if (KEY("donor_lone")) C.donor_lone = (int)d;
		else if (KEY("probe_groups")) C.probe_groups = (int)d;
		else if (KEY("far_probe")) C.far_probe = (int)d;
		else if (KEY("far_us")) C.far_us = d;
		else if (KEY("spill")) C.spill = (int)d;
		else if (KEY("imin_us")) C.kp.imin = KP_US2T(d);
		else if (KEY("imax_us")) C.kp.imax = KP_US2T(d);
		else if (KEY("texp_us")) C.kp.texp = KP_US2T(d);
		else if (KEY("hold_us")) C.kp.hold = KP_US2T(d);
		else if (KEY("h_us")) C.kp.h_us = (u_int)d;
		else if (KEY("l_us")) C.kp.l_us = (u_int)d;
		else if (KEY("ladv_us")) C.kp.ladv_us = (u_int)d;
		else if (KEY("llocal_us")) C.kp.llocal_us = (u_int)d;
		else if (KEY("csmall_us")) C.kp.csmall_ns = (u_int)(d * US);
		else if (KEY("table")) C.kp.table_n = (u_int)d;
		else if (KEY("checks")) C.kp.checks = (u_int)d;
		else if (KEY("c_l3")) C.c_l3 = d;
		else if (KEY("c_sib")) C.c_sib = d;
		else if (KEY("scan_entry")) C.scan_entry = d;
		else if (KEY("wl_short_us")) C.wl_short = d * US;
		else if (KEY("wl_long_us")) C.wl_long = d * US;
		else if (KEY("wl_deep_us")) C.wl_deep = d * US;
		else if (KEY("absent")) C.absent = (u_int)d;
		else if (KEY("verbose")) C.verbose = (int)d;
		else if (KEY("expcost")) C.expcost = (int)d;
		else if (KEY("oneq")) C.oneq = (int)d;
		else if (KEY("extraq")) C.extraq = (int)d;
		else if (KEY("c_hit")) C.c_hit = d;
		else if (KEY("c_atomic")) C.c_atomic = d;
		else if (KEY("place_ns")) C.place_ns = d;
		else if (KEY("merge_ns")) C.merge_ns = d;
		else if (KEY("series")) C.series = (int)d;
		else {
			fprintf(stderr, "unknown key %s\n", k);
			exit(2);
		}
#undef KEY
	}
}

int
main(int argc, char **argv)
{
	struct ev e;
	int c, q, g;
	uint64_t w;

	cfg_default();
	C.fixed = -1;
	parse(argc, argv);
	gs = C.seed * 0x9e3779b97f4a7c15ULL + 1;
	topo_build();
	nl3 = 0;
	for (c = 0; c < maxid; c++)
		if (cpu_present[c] && cpu_l3[c] + 1 > nl3)
			nl3 = cpu_l3[c] + 1;
	scen_setup();
	if (nq == 0) {
		fprintf(stderr, "no queues\n");
		return (2);
	}
	ql = calloc((size_t)nq * (size_t)maxid, sizeof(*ql));
	for (q = 0; q < nq; q++)
		for (c = 0; c < maxid; c++) {
			QL(q, c)->head = QL(q, c)->tail = NONE;
			QL(q, c)->cost_e = Q[q].nominal;
		}
	lines_init(nq);
	ser = calloc(SERIES_MAX, sizeof(*ser));
	nser = (int)((C.dur - C.warm) / MS) + 1;
	if (nser > SERIES_MAX)
		nser = SERIES_MAX;
	for (c = 0; c < maxid; c++) {
		grp_load[c] = 0;
		if (!cpu_present[c])
			continue;
		g = cpu_l3[c];
		grp_idx[c] = grp_n[g];
		grp_cpus[g][grp_n[g]++] = c;
	}
	for (c = 0; c < maxid; c++) {
		struct cpu *p = &cpus[c];

		last_ho_to[c] = -1;
		if (!cpu_present[c])
			continue;
		p->ring = calloc((size_t)nq, sizeof(int));
		p->pq = NONE;
		p->bhead = p->btail = p->ohead = p->otail = NONE;
		if (p->capx == 0 && !p->hog)
			p->capx = C.cap;
		else if (p->hog && p->capx == 0 && scen_id != 15)
			p->capx = C.cap;
		p->share_e = 1;
		p->dq = NONE;
		p->far_g = -1;
		p->rng = (uint32_t)(C.seed * 2654435761u + (uint32_t)c * 40503u + 1);
		p->ws = W_SLEEP;
		p->sleep_t = -10 * SEC;
		p->inmask = C.policy == P_TODAY || C.policy == P_NOSTEAL || C.policy == P_DONOR ||
		    C.policy == P_SHARD;
		if (C.policy == P_GOSSIP) {
			cur_cpu = c;
			kp_load_update(&p->kl, kpnow(), 0, &C.kp, &w);
			blocks[cpu_core[c]].kc_st.ks_w[cpu_slot[c]] = w;
		}
		if (C.policy == P_GROUP)
			grp_update(c, 0);
	}
	for (c = 0; c < ncores_t; c++)
		core_resched(c);
	for (int pi = 0; pi < nprod; pi++)
		if (scen_id != 11)
			prod_start(pi);
	ev_push(SAMPLE, EV_SAMPLE, 0, 0);
	ev_push(C.dur, EV_END, 0, 0);
	while (nheap > 0) {
		e = ev_pop();
		now = e.t;
		if (e.type == EV_END)
			break;
		if (!M.t0 && warm()) {
			M.t0 = now;
			M.xfers0 = st_xfers;
			M.reads0 = st_reads;
			for (c = 0; c < maxid; c++) {
				if (!cpu_present[c])
					continue;
				thread_advance(c);
				M.bulk0 += cpus[c].t_bulk;
				M.oth0 += cpus[c].t_other;
				M.sch0 += cpus[c].t_scheme;
			}
		}
		switch (e.type) {
		case EV_DONE_B:
			if (e.b != cpus[e.a].bseq || cpus[e.a].bj == J_NONE)
				break;
			if (cpus[e.a].bj == J_OVH) {
				thread_advance(e.a);
				cpus[e.a].bj = J_NONE;
				RESCHED(e.a);
				worker_kick(e.a);
			} else
				item_done(e.a);
			break;
		case EV_DONE_O:
			if (e.b == cpus[e.a].oseq && cpus[e.a].on > 0)
				other_done(e.a);
			break;
		case EV_WAKE: {
			struct cpu *p = &cpus[e.a];

			if (p->ws != W_WAKING)
				break;
			p->ws = W_RUN;
			if (p->inmask) {
				p->inmask = false;
				charge_bulk(e.a, line_write(e.a, idle_line(e.a)) + C.c_atomic);
			}
			if (C.policy == P_GOSSIP && now - p->sleep_t > KP_T2US(C.kp.imax) * US)
				merge_at(e.a, true);
			load_point(e.a);
			RESCHED(e.a);
			worker_kick(e.a);
			break;
		}
		case EV_PUBLISH:
			if (e.b == cpus[e.a].pubseq && cpus[e.a].ws == W_SLEEP) {
				load_point(e.a);
				arm_publish(e.a);
			}
			break;
		case EV_ARRIVE:
			if (P[e.a].on && P[e.a].kind == PK_OPEN && e.b == P[e.a].epoch &&
			    P[e.a].rate > 0) {
				issue(e.a, prod_home(&P[e.a]), false);
				ev_push(now + rndexp(&gs, 1 / P[e.a].rate), EV_ARRIVE, e.a, e.b);
			}
			break;
		case EV_ISSUE:
			if (P[e.a].on)
				issue(e.a, (int)e.b, false);
			break;
		case EV_RETRY:
			pending_retries--;
			enqueue_item(e.a, (int)e.b, P[items[e.a].prod].fixed, false);
			break;
		case EV_PHASE:
			scen_phase(e.a);
			break;
		case EV_CAP:
			if (e.b == cpus[e.a].capseq)
				RESCHED(e.a);
			break;
		case EV_WATCH:
			watch_fire(e.a);
			break;
		case EV_SAMPLE:
			if (warm())
				sample();
			ev_push(now + SAMPLE, EV_SAMPLE, 0, 0);
			break;
		}
	}
	for (g = 0; g < ncores_t; g++)
		if (herd_open[g])
			herd_close(g);
	report();
	return (0);
}
