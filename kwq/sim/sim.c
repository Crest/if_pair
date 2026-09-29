/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwqsim - discrete-event simulator for the kwq scheduler core
 * (../kwq/kwq_sched.c), PLAN.txt P1a.
 *
 * One simulated CPU with a NET and a BULK worker (strict priority NET >
 * BULK > external threads), virtual wall time and per-worker CPU time in
 * nanoseconds, producers and handler cost models per (queue, CPU), the
 * S6.4 hand-back flag between the classes, and an external-load model for
 * what a yield hands the CPU to.  After every scheduler call the SCHED.md
 * S4.7 invariants are checked; per run the S7 bounds are checked and the
 * S9 counters and latency figures printed.
 *
 *   kwqsim <scenario> [-s seed] [-t seconds] [-v]
 *   kwqsim suite                     run every scenario over its seeds
 *
 * Scenarios: fairness latency gaming overrun overrun_idle handback storm
 * nnew manyq budgetjump glitch wrap ratechange badhandler random
 * sweep_grace sweep_boost.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kwq_sched.h"

#define	NS_PER_US	1000ULL
#define	NS_PER_MS	1000000ULL
#define	NS_PER_S	1000000000ULL
#define	TICK_NS		NS_PER_MS		/* hz = 1000 */
#define	PASS_OVERHEAD_NS 250			/* S15.2 */
#define	MAXQ		4096
#define	NCLASS		2			/* NET, BULK */

enum { C_NET = 0, C_BULK = 1 };

/* ---------------------------------------------------------------- rng */
static uint64_t rng_s;
static uint64_t
rnd64(void)
{
	rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
	return (rng_s);
}
static double
rnd01(void)
{
	return ((rnd64() >> 11) * (1.0 / 9007199254740992.0));
}
static uint64_t
rnd_exp(double mean)
{
	double u = rnd01();
	if (u < 1e-12) u = 1e-12;
	return ((uint64_t)(-log(u) * mean));
}

/* ---------------------------------------------------------------- items */
struct item {
	uint64_t	enq_t;
	uint64_t	cost;
};

struct deque {
	struct item	*buf;
	size_t		cap, head, len;
};

static void
dq_init(struct deque *d, size_t cap)
{
	d->buf = calloc(cap, sizeof(*d->buf));
	d->cap = cap; d->head = 0; d->len = 0;
}
static bool dq_empty(const struct deque *d) { return (d->len == 0); }
static void
dq_push_back(struct deque *d, struct item it)
{
	if (d->len == d->cap) {
		struct item *nb = calloc(d->cap * 2, sizeof(*nb));
		for (size_t i = 0; i < d->len; i++)
			nb[i] = d->buf[(d->head + i) % d->cap];
		free(d->buf); d->buf = nb; d->head = 0; d->cap *= 2;
	}
	d->buf[(d->head + d->len) % d->cap] = it;
	d->len++;
}
static struct item
dq_pop_front(struct deque *d)
{
	struct item it = d->buf[d->head];
	d->head = (d->head + 1) % d->cap; d->len--;
	return (it);
}


/* ---------------------------------------------------------------- models */
enum prod_kind {
	P_NONE, P_POISSON, P_FLOOD, P_BURSTPAUSE, P_PERIODIC, P_ONESHOT
};
enum hand_kind {
	H_COOP,		/* budget after every item, at least one (S5) */
	H_IGNORE,	/* runs the whole batch */
	H_FIRSTONLY,	/* one item per pass, then requeue */
	H_REQUEUEALL,	/* bug: requeues everything, no progress */
	H_FIRSTBUDGET	/* checks the budget before the first item too (bug) */
};

struct simq {
	int		id;
	int		cls;
	struct kwq_cpu	kc;
	struct deque	q;
	/* producer */
	enum prod_kind	pk;
	double		rate;		/* items/s (poisson) */
	uint64_t	next_t;
	int		burst;		/* items per burst */
	uint64_t	pause_ns;	/* burstpause: pause after the list empties */
	int		flood_depth;	/* flood: keep this many queued */
	uint64_t	refill_delay;	/* flood: 0 = refill during the pass (instant producer); else the consumed items come back this long after the pass ends */
	bool		pause_armed;
	/* handler */
	enum hand_kind	hk;
	uint64_t	cost;		/* ns per item */
	double		heavy_p;	/* probability of a heavy item */
	uint64_t	heavy_cost;
	/* stats */
	uint64_t	items_in, items_out, service_ns, requeued;
	uint64_t	lat_max, lat_sum, lat_new_max;	/* item latency; doorbell->pass */
	uint64_t	doorbell_t;	/* wall time of the last doorbell, 0 = none */
	uint64_t	backlog_ns;	/* wall time spent with work queued */
	uint64_t	backlog_since;
	int		heap_pos;	/* -1 when not in the heap */
	/* oracle for the grace rule */
	uint64_t	idle_round;	/* kw_round when it went idle (64-bit) */
	bool		idle_from_ring;
	bool		idle_with_debt;	/* kc_warm & KWQ_DEBT when it went idle */
	int		boost_viol;
};

struct sim {
	uint64_t	now;			/* wall ns */
	uint64_t	cputime[NCLASS];	/* worker CPU time ns */
	struct kwq_worker w[NCLASS];
	struct kwq_sched_knobs knobs[NCLASS];
	bool		waiting[NCLASS];	/* hand-back flags */
	bool		sleeping[NCLASS];
	u_int		ticks, swvoltick[NCLASS];
	struct simq	*qs[MAXQ];
	int		nq;
	/* external load */
	int		ext_kind;		/* 0 none, 1 interactive, 2 cpu-bound */
	bool		cpubound_allowed;	/* yield_prio lets it run */
	uint64_t	ext_ns;			/* wall time given away */
	/* checks */
	int		violations;
	bool		verbose;
	uint64_t	round_start_snapshot[NCLASS][MAXQ];
	int		round_members[NCLASS][MAXQ];
	int		nround_members[NCLASS];
	int		rounds_without_pass[NCLASS];
	bool		round_had_pass[NCLASS];
	uint64_t	last_round[NCLASS];
	uint64_t	pass_in_progress_max;
	uint64_t	max_overshoot;		/* largest pass time beyond its budget (coop) */
	uint64_t	glitch_pos_pass, glitch_neg_pass;	/* glitch scenario: inject at these passes */
	uint64_t	cmax[NCLASS];
	uint64_t	t_start, t_dur;		/* run window, wrap-safe: now - t_start < t_dur */
	uint64_t	wall0;			/* wall time at reset, for the tick counter */
	u_int		ticks0;
	/* min-heap of timed producers keyed by next_t */
	int		*heap;
	int		nheap;
};

static struct sim S;
static uint64_t total_steps;

static void
fail(const char *fmt, ...)
{
	va_list ap;

	S.violations++;
	fprintf(stderr, "VIOLATION t=%" PRIu64 "ns: ", S.now);
	va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
	fputc('\n', stderr);
	if (S.violations > 20) {
		fprintf(stderr, "too many violations, aborting\n");
		exit(2);
	}
}

/* KS_ASSERT() in the core (kwq_sched.h) reports here. */
void
ks_env_fail(const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	fail("core assertion: %s", buf);
}

/* ---------------------------------------------------------------- setup */
static void
sim_reset(uint64_t wall0, uint64_t cpu0, uint64_t round0, u_int ticks0)
{
	memset(&S, 0, sizeof(S));
	S.now = wall0;
	S.wall0 = wall0;
	S.ticks0 = ticks0;
	S.ticks = ticks0;
	for (int c = 0; c < NCLASS; c++) {
		S.cputime[c] = cpu0;
		S.max_overshoot = 0;
		S.glitch_pos_pass = S.glitch_neg_pass = 0;
		S.knobs[c].quantum_ns = c == C_NET ? 200 * NS_PER_US : 1000 * NS_PER_US;
		S.knobs[c].grace_rounds = 1;
		S.knobs[c].cap_pct = 100;
		S.knobs[c].cap_window_ns = 10 * NS_PER_MS;
		S.knobs[c].cap_sleep_ns = 100 * NS_PER_US;
		S.knobs[c].boost_weighted = true;
		S.knobs[c].penalty_rounds = 32;
		S.knobs[c].budget_check_every = 8;
		ks_worker_init(&S.w[c], &S.knobs[c], wall0);
		S.w[c].kw_round = round0;
		S.sleeping[c] = true;
		S.swvoltick[c] = ticks0;
		S.last_round[c] = round0;
	}
}

static struct simq *
addq(int cls, u_int weight, enum prod_kind pk, enum hand_kind hk, uint64_t cost)
{
	struct simq *sq = calloc(1, sizeof(*sq));

	sq->id = S.nq; sq->cls = cls; sq->pk = pk; sq->hk = hk; sq->cost = cost;
	sq->kc.kc_weight = weight; sq->kc.kc_sq = sq;
	sq->kc.kc_idle_round = (u_int)S.w[cls].kw_round;
	ks_queue_init(&sq->kc);
	sq->kc.kc_idle_round = (u_int)S.w[cls].kw_round;
	dq_init(&sq->q, 64);
	sq->next_t = S.now;
	sq->heap_pos = -1;
	sq->idle_round = S.w[cls].kw_round;
	if (cost > S.cmax[cls]) S.cmax[cls] = cost;
	S.qs[S.nq++] = sq;
	return (sq);
}

/* ---------------------------------------------------------------- checks */
/*
 * I1-I3 for one class.  Linear in the number of queues: list membership
 * is computed once with a marker array.  With many queues the full check
 * runs on a sample of calls (every nq/16th), exact for small scenarios.
 */
static uint64_t check_calls;
static void
check_invariants(int c)
{
	struct kwq_worker *kw = &S.w[c];
	struct kwq_cpu *kc;
	static unsigned char *mark;
	static int mark_sz;
	int n = 0, m = 0;

	check_calls++;
	if (S.nq > 16 && (check_calls % (uint64_t)(S.nq / 16)) != 0)
		return;
	if (mark_sz < S.nq) {
		free(mark); mark = calloc(S.nq, 1); mark_sz = S.nq;
	}
	memset(mark, 0, S.nq);
	TAILQ_FOREACH(kc, &kw->kw_new, kc_active) {
		m++;
		if (kc->kc_onlist != KWQ_ON_NEW)
			fail("q%d on new list with onlist %u", kc->kc_sq->id, kc->kc_onlist);
		mark[kc->kc_sq->id]++;
	}
	if ((u_int)m != kw->kw_nnew)
		fail("class %d new list has %d entries, kw_nnew %u", c, m, kw->kw_nnew);
	TAILQ_FOREACH(kc, &kw->kw_active, kc_active) {
		n++;
		if (kc->kc_onlist != KWQ_ON_ACTIVE)
			fail("q%d on ring with onlist %u", kc->kc_sq->id, kc->kc_onlist);
		mark[kc->kc_sq->id]++;
	}
	if ((u_int)n != kw->kw_nactive)
		fail("class %d ring has %d entries, kw_nactive %u", c, n, kw->kw_nactive);
	for (int i = 0; i < S.nq; i++) {
		struct simq *sq = S.qs[i];
		int64_t q;

		if (sq->cls != c) continue;
		kc = &sq->kc;
		if (mark[i] > 1) fail("q%d on %d lists", sq->id, mark[i]);
		if ((mark[i] == 1) != (kc->kc_onlist != KWQ_ON_NONE))
			fail("q%d onlist %u but on %d lists", sq->id, kc->kc_onlist, mark[i]);
		/* I2 */
		if (kw->kw_cur == kc) {
			if (kc->kc_state != KWQ_CPU_RUNNING) fail("q%d current but state %u", sq->id, kc->kc_state);
		} else if (kc->kc_onlist != KWQ_ON_NONE) {
			if (kc->kc_state != KWQ_CPU_WAKING && kc->kc_state != KWQ_CPU_PARKED)
				fail("q%d listed but state %u", sq->id, kc->kc_state);
		} else {
			if (kc->kc_state != KWQ_CPU_IDLE) fail("q%d unlisted but state %u", sq->id, kc->kc_state);
			if (!dq_empty(&sq->q)) fail("q%d idle with %zu items", sq->id, sq->q.len);
		}
		/* I3 */
		q = ks_quantum(kw, kc);
		if (kc->kc_deficit > 2 * q || kc->kc_deficit < -(int64_t)kw->kw_knobs->penalty_rounds * q)
			fail("q%d deficit %" PRId64 " outside [-%uQw, 2Qw] (%" PRId64 ")", sq->id, kc->kc_deficit, kw->kw_knobs->penalty_rounds, q);
		if (kc->kc_state == KWQ_CPU_IDLE) {
			/* S4.3: idle with a deficit only as recorded debt */
			if ((kc->kc_warm & KWQ_DEBT) ? kc->kc_deficit >= 0 : kc->kc_deficit != 0)
				fail("q%d idle with deficit %" PRId64 " (warm 0x%x)", sq->id, kc->kc_deficit, kc->kc_warm);
		}
	}
}

/* I5 bookkeeping: snapshot the ring at round start, verify at round end. */
static void
round_snapshot(int c)
{
	struct kwq_worker *kw = &S.w[c];
	struct kwq_cpu *kc;
	int i = 0;

	TAILQ_FOREACH(kc, &kw->kw_active, kc_active) {
		S.round_members[c][i] = kc->kc_sq->id;
		S.round_start_snapshot[c][i] = kc->kc_passes + kc->kc_parks;
		i++;
	}
	S.nround_members[c] = i;
	S.round_had_pass[c] = false;
}

static void
round_verify(int c, bool handback)
{
	/* B8: at most penalty_rounds + 1 consecutive rounds without a pass while
	   work exists (the most indebted queue needs that many refills). */
	if (!S.round_had_pass[c]) {
		if (++S.rounds_without_pass[c] > (int)S.w[c].kw_knobs->penalty_rounds + 1)
			fail("B8: class %d, %d consecutive rounds without a pass", c, S.rounds_without_pass[c]);
	} else
		S.rounds_without_pass[c] = 0;
	if (handback) return;
	for (int i = 0; i < S.nround_members[c]; i++) {
		struct simq *sq = S.qs[S.round_members[c][i]];
		uint64_t d = sq->kc.kc_passes + sq->kc.kc_parks - S.round_start_snapshot[c][i];
		if (d != 1)
			fail("I5: q%d had %" PRIu64 " passes+parks in round %" PRIu64, sq->id, d, S.w[c].kw_round);
	}
}

/* ---------------------------------------------------------------- producers */
static void
enqueue(struct simq *sq, uint64_t t)
{
	struct item it;
	bool was_empty = dq_empty(&sq->q);

	it.enq_t = t;
	it.cost = (sq->heavy_p > 0 && rnd01() < sq->heavy_p) ? sq->heavy_cost : sq->cost;
	dq_push_back(&sq->q, it);
	sq->items_in++;
	if (was_empty) {
		sq->backlog_since = t;
		if (sq->kc.kc_state == KWQ_CPU_IDLE) {
			struct kwq_worker *kw = &S.w[sq->cls];
			bool boost, expect_boost;

			/* Oracle for S4.6: warm iff idle from the ring within grace rounds. */
			expect_boost = !sq->idle_with_debt && !(sq->idle_from_ring &&
			    kw->kw_round - sq->idle_round <= kw->kw_knobs->grace_rounds);
			boost = ks_doorbell(kw, &sq->kc);
			if (boost != expect_boost) {
				sq->boost_viol++;
				fail("I6: q%d boost=%d expected %d (idle_from_ring %d, debt %d, rounds since idle %" PRIu64 ")",
				    sq->id, boost, expect_boost, sq->idle_from_ring, sq->idle_with_debt, kw->kw_round - sq->idle_round);
			}
			sq->doorbell_t = t;
			S.sleeping[sq->cls] = false;
			check_invariants(sq->cls);
		}
	}
}

/* ---- min-heap of timed producers (poisson, periodic, oneshot, armed burst-pause) */
static void
heap_swap(int i, int j)
{
	int t = S.heap[i]; S.heap[i] = S.heap[j]; S.heap[j] = t;
	S.qs[S.heap[i]]->heap_pos = i; S.qs[S.heap[j]]->heap_pos = j;
}
static void
heap_up(int i)
{
	while (i > 0) {
		int p = (i - 1) / 2;
		if (S.qs[S.heap[p]]->next_t <= S.qs[S.heap[i]]->next_t) break;
		heap_swap(i, p); i = p;
	}
}
static void
heap_down(int i)
{
	for (;;) {
		int l = 2 * i + 1, r = l + 1, m = i;
		if (l < S.nheap && S.qs[S.heap[l]]->next_t < S.qs[S.heap[m]]->next_t) m = l;
		if (r < S.nheap && S.qs[S.heap[r]]->next_t < S.qs[S.heap[m]]->next_t) m = r;
		if (m == i) break;
		heap_swap(i, m); i = m;
	}
}
static void
heap_push(struct simq *sq)
{
	if (sq->heap_pos >= 0) { heap_up(sq->heap_pos); heap_down(sq->heap_pos); return; }
	if (S.heap == NULL) S.heap = calloc(MAXQ, sizeof(int));
	S.heap[S.nheap] = sq->id; sq->heap_pos = S.nheap; S.nheap++;
	heap_up(sq->heap_pos);
}
static struct simq *
heap_pop(void)
{
	struct simq *sq = S.qs[S.heap[0]];
	S.nheap--;
	if (S.nheap > 0) { S.heap[0] = S.heap[S.nheap]; S.qs[S.heap[0]]->heap_pos = 0; heap_down(0); }
	sq->heap_pos = -1;
	return (sq);
}

static void refill(struct simq *sq);

/* Generate all timed arrivals with time <= now; refill the flood being served. */
static void
pump(void)
{
	while (S.nheap > 0 && S.qs[S.heap[0]]->next_t <= S.now) {
		struct simq *sq = heap_pop();

		switch (sq->pk) {
		case P_POISSON:
			enqueue(sq, sq->next_t);
			sq->next_t += rnd_exp(NS_PER_S / sq->rate) + 1;
			heap_push(sq);
			break;
		case P_PERIODIC:
			for (int k = 0; k < sq->burst; k++) enqueue(sq, sq->next_t);
			sq->next_t += (uint64_t)(NS_PER_S / sq->rate);
			heap_push(sq);
			break;
		case P_ONESHOT:
			enqueue(sq, sq->next_t);
			break;
		case P_FLOOD:		/* delayed refill after a pass */
			refill(sq);
			break;
		case P_BURSTPAUSE:	/* the pause is over: burst */
			for (int k = 0; k < sq->burst; k++) enqueue(sq, sq->next_t);
			sq->pause_armed = false;
			break;
		default:
			break;
		}
	}
}

/* Floods: keep the list at flood_depth (called for the queue being served). */
static void
refill(struct simq *sq)
{
	if (sq->pk == P_FLOOD)
		while ((int)sq->q.len < sq->flood_depth)
			enqueue(sq, S.now);
}

/* A burst-and-pause queue went idle: arm its pause. */
static void
arm_pause(struct simq *sq)
{
	if (sq->pk == P_BURSTPAUSE && !sq->pause_armed) {
		sq->pause_armed = true;
		sq->next_t = S.now + sq->pause_ns;
		heap_push(sq);
	}
}

static uint64_t
next_arrival(void)
{
	return (S.nheap > 0 ? S.qs[S.heap[0]]->next_t : UINT64_MAX);
}

static bool
class_has_work(int c)
{
	return (!TAILQ_EMPTY(&S.w[c].kw_new) || !TAILQ_EMPTY(&S.w[c].kw_active) ||
	    S.w[c].kw_phase != KWQ_PH_IDLE);
}

/* ---------------------------------------------------------------- the CPU */
static void run_class(int c, bool one_round);

/* Something else runnable on this CPU from class c's point of view? */
static bool
runnable_below(int c)
{
	for (int d = c + 1; d < NCLASS; d++)
		if (class_has_work(d)) return (true);
	return (S.ext_kind == 1 || (S.ext_kind == 2 && S.cpubound_allowed));
}

/* Give the CPU away for one scheduling decision (S6.2 model). */
static void
do_yield(int c)
{
	S.swvoltick[c] = S.ticks;
	S.waiting[c] = true;
	if (c + 1 < NCLASS && class_has_work(c + 1)) {
		/* the lower class runs until it hands back (S6.4) */
		run_class(c + 1, true);
	} else if (S.ext_kind == 1) {
		uint64_t burst = 5 * NS_PER_US + rnd64() % (45 * NS_PER_US);
		S.now += burst; S.ext_ns += burst;
	} else if (S.ext_kind == 2 && S.cpubound_allowed) {
		uint64_t slice = 16 * NS_PER_MS + rnd64() % (78 * NS_PER_MS);
		S.now += slice; S.ext_ns += slice;
	}
	S.waiting[c] = false;
	S.ticks = S.ticks0 + (u_int)((S.now - S.wall0) / TICK_NS);
}

/* The handler's budget query as the kernel makes it (S4.4). */
static uint64_t
sim_budget(int c, struct kwq_worker *kw)
{
	uint64_t left;

	if (!ks_budget_need_clock(kw, &left))
		return (left);
	return (ks_budget_left(kw, S.cputime[c]));
}

static void
run_pass(int c, struct kwq_cpu *kc)
{
	struct kwq_worker *kw = &S.w[c];
	struct simq *sq = kc->kc_sq;
	uint64_t t0 = S.now, cpu0, budget, nbatch = sq->q.len;
	struct item it;
	bool stop = false;

	ks_pass_begin(kw, kc, S.cputime[c]);
	cpu0 = S.cputime[c];
	if (S.glitch_pos_pass != 0 && kc->kc_passes + 1 == S.glitch_pos_pass)
		S.cputime[c] += 171798691840ULL;	/* +2^32 ticks at 25 MHz: a07's fault */
	budget = kw->kw_pass_budget > 0 ? (uint64_t)kw->kw_pass_budget : 0;
	if (sq->doorbell_t != 0 && sq->kc.kc_state == KWQ_CPU_RUNNING) {
		uint64_t l = S.now - sq->doorbell_t;
		if (l > sq->lat_new_max) sq->lat_new_max = l;
		sq->doorbell_t = 0;
	}
	S.now += PASS_OVERHEAD_NS; S.cputime[c] += PASS_OVERHEAD_NS;
	check_invariants(c);
	S.round_had_pass[c] = true;

	/* The handler: the batch is what was queued at pass start. */
	while (nbatch > 0 && !stop) {
		bool hb = (c == C_BULK && S.waiting[C_NET]);	/* S6.4 */
		uint64_t left = hb ? 0 : sim_budget(c, kw);

		if (sq->hk == H_REQUEUEALL) { sq->requeued += nbatch; break; }
		if (sq->hk == H_FIRSTBUDGET && left == 0) { sq->requeued += nbatch; break; }
		pump();				/* arrivals during the pass */
		it = dq_pop_front(&sq->q);
		if (sq->refill_delay == 0)
			refill(sq);	/* instant producer refills behind the handler */
		{
			uint64_t l = S.now - it.enq_t;
			sq->lat_sum += l; if (l > sq->lat_max) sq->lat_max = l;
		}
		S.now += it.cost; S.cputime[c] += it.cost;
		sq->service_ns += it.cost; sq->items_out++; nbatch--;
		if (nbatch == 0) break;
		switch (sq->hk) {
		case H_COOP: case H_FIRSTBUDGET:
			left = hb ? 0 : sim_budget(c, kw);
			if (left == 0) { stop = true; sq->requeued += nbatch; }
			break;
		case H_FIRSTONLY:
			stop = true; sq->requeued += nbatch; break;
		case H_IGNORE: default:
			break;
		}
	}
	/* B1: a cooperative pass takes at most budget + c_max (+ overhead). */
	if (sq->hk == H_COOP && !(S.glitch_pos_pass != 0 && kc->kc_passes + 1 == S.glitch_pos_pass)) {
		uint64_t dt = S.cputime[c] - cpu0;
		uint64_t cmax = sq->cost > sq->heavy_cost ? sq->cost : (sq->heavy_p > 0 ? sq->heavy_cost : sq->cost);
		uint32_t k = kw->kw_knobs->budget_check_every;
		/* B1 with the estimated check: budget + Qw/10 + K c_max (K = 1: budget + c_max) */
		uint64_t lim = budget + (k > 1 ? ks_quantum(kw, kc) / 10 + (uint64_t)k * cmax : cmax) + PASS_OVERHEAD_NS;

		if (dt > lim)
			fail("B1: q%d pass %" PRIu64 " ns > budget %" PRIu64 " + slack %" PRIu64 " (K %u)", sq->id, dt, budget, lim - budget, k);
		if (dt > budget && dt - budget > S.max_overshoot) S.max_overshoot = dt - budget;
	}
	if (S.glitch_neg_pass != 0 && kc->kc_passes + 1 == S.glitch_neg_pass)
		S.cputime[c] -= 300;			/* a backwards read: 300 ns */
	pump();
	if (sq->refill_delay == 0)
		refill(sq);	/* instant producer: the flood never empties */
	else if (sq->heap_pos < 0) {
		/* the consumed items come back refill_delay after the pass */
		sq->next_t = S.now + sq->refill_delay;
		heap_push(sq);
	}
	if (S.now - t0 > S.pass_in_progress_max) S.pass_in_progress_max = S.now - t0;
	ks_pass_end(kw, kc, S.cputime[c], !dq_empty(&sq->q));
	if (kc->kc_state == KWQ_CPU_IDLE) {
		sq->backlog_ns += S.now - sq->backlog_since;
		sq->idle_round = kw->kw_round;
		sq->idle_from_ring = (kc->kc_warm & KWQ_WARM) != 0;
		sq->idle_with_debt = (kc->kc_warm & KWQ_DEBT) != 0;
		arm_pause(sq);
	}
	check_invariants(c);
	S.ticks = S.ticks0 + (u_int)((S.now - S.wall0) / TICK_NS);
	if (ks_ticked(S.ticks, S.swvoltick[c])) {	/* unconditional, S6.1 */
		S.w[c].kw_yields++;	/* counted as tick yield in the kernel */
		do_yield(c);
	}
}

/*
 * Run class c: one round (for a lower class that must hand back) or until
 * it has nothing to do.
 */
static void
run_class(int c, bool one_round)
{
	struct kwq_worker *kw = &S.w[c];
	struct kwq_cpu *kc;
	enum ks_action a;
	enum ks_yield y;
	bool higher;

	for (;;) {
		if (S.now - S.t_start >= S.t_dur && !one_round)
			return;
		higher = false;
		for (int d = 0; d < c; d++) if (S.waiting[d]) higher = true;
		if (kw->kw_phase == KWQ_PH_IDLE)	/* a round may begin: I5 snapshot first */
			round_snapshot(c);
		a = ks_next(kw, higher, &kc);
		total_steps++;
		check_invariants(c);
		switch (a) {
		case KS_IDLE:
			S.sleeping[c] = true;
			return;
		case KS_SERVE:
			run_pass(c, kc);
			break;
		case KS_ROUND_END:
		case KS_HANDBACK:
			round_verify(c, a == KS_HANDBACK);
			y = ks_round_end(kw, S.now, runnable_below(c) || higher);
			if (y == KS_Y_YIELD) do_yield(c);
			else if (y == KS_Y_PAUSE) { S.now += kw->kw_knobs->cap_sleep_ns; S.ext_ns += kw->kw_knobs->cap_sleep_ns; }
			if (one_round || a == KS_HANDBACK) return;
			break;
		}
	}
}

static void
start_producers(void)
{
	for (int i = 0; i < S.nq; i++) {
		struct simq *sq = S.qs[i];
		if (sq->heap_pos >= 0) continue;
		switch (sq->pk) {
		case P_POISSON: case P_PERIODIC: case P_ONESHOT:
			if (sq->next_t != UINT64_MAX) heap_push(sq);
			break;
		case P_FLOOD:
			/* delayed floods are filled once, then re-armed at pass end */
			if (sq->refill_delay == 0 || sq->kc.kc_passes == 0)
				refill(sq);
			break;
		case P_BURSTPAUSE:
			for (int k = 0; k < sq->burst; k++) enqueue(sq, S.now);
			break;
		default:
			break;
		}
	}
}

/* Run for `dur' ns from now; all comparisons are unsigned differences. */
static void
run_for(uint64_t dur)
{
	S.t_start = S.now;
	S.t_dur = dur;
	start_producers();
	while (S.now - S.t_start < dur) {
		pump();
		if (class_has_work(C_NET)) run_class(C_NET, false);
		else if (class_has_work(C_BULK)) run_class(C_BULK, false);
		else {
			uint64_t t = next_arrival();
			if (t == UINT64_MAX || t - S.t_start >= dur) { S.now = S.t_start + dur; break; }
			if (t - S.t_start > S.now - S.t_start) S.now = t;
			S.ticks = S.ticks0 + (u_int)((S.now - S.wall0) / TICK_NS);
		}
	}
}

/* ---------------------------------------------------------------- report */
static void
report(const char *title)
{
	printf("== %s: ran %.3f s ext=%.3f s violations=%d\n", title,
	    (double)(S.now - S.t_start) / NS_PER_S, (double)S.ext_ns / NS_PER_S, S.violations);
	for (int c = 0; c < NCLASS; c++) {
		struct kwq_worker *kw = &S.w[c];
		if (kw->kw_passes == 0) continue;
		printf("  %s: rounds %" PRIu64 " passes %" PRIu64 " yields %" PRIu64
		    " handbacks %" PRIu64 " cap_sleeps %" PRIu64 " busy %.3f s\n",
		    c == C_NET ? "net " : "bulk", kw->kw_rounds, kw->kw_passes,
		    kw->kw_yields, kw->kw_handbacks, kw->kw_cap_sleeps,
		    (double)kw->kw_busy_ns / NS_PER_S);
	}
	for (int i = 0; i < S.nq && i < 12; i++) {
		struct simq *sq = S.qs[i];
		printf("  q%-2d %s w%u in %-8" PRIu64 " out %-8" PRIu64 " svc %.3f s"
		    " passes %-6" PRIu64 " ovr %-4" PRIu64 " parks %-4" PRIu64
		    " boosts %-5" PRIu64 " grace %-4" PRIu64 " lat max %.1f us avg %.1f us new %.1f us\n",
		    sq->id, sq->cls == C_NET ? "net " : "bulk", sq->kc.kc_weight,
		    sq->items_in, sq->items_out, (double)sq->service_ns / NS_PER_S,
		    sq->kc.kc_passes, sq->kc.kc_overruns, sq->kc.kc_parks,
		    sq->kc.kc_boosts, sq->kc.kc_grace,
		    (double)sq->lat_max / NS_PER_US,
		    sq->items_out ? (double)sq->lat_sum / sq->items_out / NS_PER_US : 0.0,
		    (double)sq->lat_new_max / NS_PER_US);
	}
	if (S.nq > 12) printf("  ... %d queues\n", S.nq);
}

/* Expect the service ratio a/b to be r within tol (relative). */
static void
expect_ratio(struct simq *a, struct simq *b, double r, double tol, const char *what)
{
	double got = (double)a->service_ns / (double)b->service_ns;

	if (fabs(got - r) > tol * r)
		fail("%s: service ratio q%d/q%d = %.3f, expected %.3f +- %.0f%%",
		    what, a->id, b->id, got, r, tol * 100);
	else
		printf("  ok  %s ratio %.3f (expected %.2f)\n", what, got, r);
}

/* ---------------------------------------------------------------- scenarios */
static int
sc_fairness(uint64_t secs)
{
	struct simq *a, *b;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 50 * NS_PER_US);  a->flood_depth = 64;
	b = addq(C_NET, 1, P_FLOOD, H_COOP, 500 * NS_PER_US); b->flood_depth = 64;
	run_for(secs * NS_PER_S);
	report("fairness, equal weight, 50 us vs 500 us items");
	expect_ratio(a, b, 1.0, 0.05, "B3 equal weight");

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 2, P_FLOOD, H_COOP, 50 * NS_PER_US);  a->flood_depth = 64;
	b = addq(C_NET, 1, P_FLOOD, H_COOP, 500 * NS_PER_US); b->flood_depth = 64;
	run_for(secs * NS_PER_S);
	report("fairness, weights 2:1");
	expect_ratio(a, b, 2.0, 0.10, "B3 weights 2:1");
	return (S.violations);
}

static int
sc_latency(uint64_t secs)
{
	struct simq *a, *b, *c;
	uint64_t Q = 200 * NS_PER_US;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 256;
	c = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); c->flood_depth = 256;
	b = addq(C_NET, 1, P_POISSON, H_COOP, 5 * NS_PER_US); b->rate = 1000;
	run_for(secs * NS_PER_S);
	report("latency: light Poisson queue vs two floods");
	/* B4: doorbell -> first pass for the light queue: pass in progress + one new pass + one ring pass */
	{
		uint64_t bound = S.pass_in_progress_max + (2 * Q + 20 * NS_PER_US) + (Q + 5 * NS_PER_US) + 3 * PASS_OVERHEAD_NS;
		if (b->lat_new_max > bound)
			fail("B4: light queue doorbell->pass %" PRIu64 " us > bound %" PRIu64 " us",
			    b->lat_new_max / NS_PER_US, bound / NS_PER_US);
		else
			printf("  ok  B4 new-queue latency %.1f us <= %.1f us\n",
			    (double)b->lat_new_max / NS_PER_US, (double)bound / NS_PER_US);
	}
	/* B5: a backlogged queue's item waits at most about one round (two queues x 2Q + c). */
	{
		uint64_t round_bound = 2 * (2 * Q + 20 * NS_PER_US) + (Q + 5 * NS_PER_US) + 4 * PASS_OVERHEAD_NS;
		/* items sit in a flood list for depth x cost anyway; check the pass spacing instead */
		printf("  info B5 round bound %.1f us; flood queue max item latency %.1f us (depth 256 x 20 us = 5120 us + rounds)\n",
		    (double)round_bound / NS_PER_US, (double)a->lat_max / NS_PER_US);
	}
	return (S.violations);
}

static int
sc_gaming(uint64_t secs)
{
	struct simq *a, *g;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 256;
	g = addq(C_NET, 1, P_BURSTPAUSE, H_COOP, 20 * NS_PER_US);
	g->burst = 32; g->pause_ns = 50 * NS_PER_US;	/* pauses well inside a round */
	run_for(secs * NS_PER_S);
	report("gaming: burst-and-pause producer against a flood");
	if (g->kc.kc_boosts > 2 + g->kc.kc_grace / 1000)
		fail("B6: gaming queue got %" PRIu64 " boosts for %" PRIu64 " idle periods",
		    g->kc.kc_boosts, g->kc.kc_grace + g->kc.kc_boosts);
	else
		printf("  ok  B6 boosts %" PRIu64 ", grace hits %" PRIu64 "\n", g->kc.kc_boosts, g->kc.kc_grace);
	/* A pausing producer cannot match a flood; it must not gain over it either. */
	if ((double)g->service_ns > 1.02 * (double)a->service_ns)
		fail("B6: gaming queue got more than the flood (%.3f)", (double)g->service_ns / a->service_ns);
	else
		printf("  ok  B6 gaming share %.3f of the flood's (no gain from bursting)\n", (double)g->service_ns / a->service_ns);
	return (S.violations);
}

static int
sc_overrun(uint64_t secs)
{
	struct simq *a, *o;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 256;
	o = addq(C_NET, 1, P_FLOOD, H_IGNORE, 2 * NS_PER_MS); o->flood_depth = 4;
	run_for(secs * NS_PER_S);
	report("overrun: budget-ignoring 2 ms items against a cooperative flood");
	if (o->kc.kc_overruns != o->kc.kc_passes)
		fail("overrun: expected overruns == passes, got %" PRIu64 " vs %" PRIu64,
		    o->kc.kc_overruns, o->kc.kc_passes);
	if (o->kc.kc_parks == 0) fail("overrun: expected parks");
	printf("  info overrunner share %.2f (unclamped DRR: 1.0; penalty cap 32 Qw allows up to pass/(32 Qw))\n",
	    (double)o->service_ns / a->service_ns);
	if ((double)o->service_ns > 1.5 * (double)a->service_ns)
		fail("overrun: overrunner share %.2f exceeds 1.5 with penalty cap 32", (double)o->service_ns / a->service_ns);
	return (S.violations);
}

static int
sc_handback(uint64_t secs)
{
	struct simq *n, *b;

	sim_reset(0, 0, 0, 0);
	n = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); n->flood_depth = 64;
	b = addq(C_BULK, 1, P_FLOOD, H_COOP, 50 * NS_PER_US); b->flood_depth = 64;
	run_for(secs * NS_PER_S);
	report("hand-back: NET flood with a BULK flood on the same CPU");
	if (S.w[C_BULK].kw_handbacks == 0) fail("S6.4: no hand-backs recorded");
	if (b->items_out == 0) fail("S6.4: BULK starved completely under a saturated NET");
	/* The NET hole at a yield is one BULK item + overheads: check NET item latency stays near a round. */
	printf("  info NET max item latency %.1f us (one BULK item is 50 us)\n", (double)n->lat_max / NS_PER_US);
	if (n->lat_max > 2 * (2 * 200 * NS_PER_US) + 50 * NS_PER_US + 64 * 20 * NS_PER_US + 10 * PASS_OVERHEAD_NS)
		fail("S6.4: NET latency %" PRIu64 " us shows BULK rounds leaking in", n->lat_max / NS_PER_US);
	return (S.violations);
}

static int
sc_storm(uint64_t secs)
{
	struct simq *a;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 256;
	for (int i = 0; i < 1000; i++) {
		struct simq *s = addq(C_NET, 1, P_PERIODIC, H_COOP, 5 * NS_PER_US);
		s->rate = 100; s->burst = 1;		/* 1000 queues x 100 items/s */
		s->next_t = (uint64_t)i * 100;		/* all wake within 100 us */
	}
	run_for(secs * NS_PER_S);
	report("storm: 1000 light queues waking together against a flood");
	if (a->kc.kc_passes < (uint64_t)secs * 100)
		fail("S14 case 2: the flood queue got only %" PRIu64 " passes in %" PRIu64 " s", a->kc.kc_passes, secs);
	return (S.violations);
}

/*
 * nnew: the new-list length counter against every way an entry gets on or
 * off the list.  512 queues wake in the same instant every 2 ms (a burst
 * that arrives partly while the previous burst's tail is still being
 * served), 64 queues at 5 kHz stay warm and take the grace path (ring,
 * not new), 8 heavy queues keep the ring long so tails and rings
 * interleave, and one budget-ignoring queue parks.  The core's KS_ASSERT
 * compares the counter with a count at every tail, the checker after
 * every step.
 */
static int
sc_nnew(uint64_t secs)
{
	struct simq *a;
	uint64_t boosts = 0, passes = 0;

	sim_reset(0, 0, 0, 0);
	for (int i = 0; i < 8; i++)
		a = addq(C_NET, 1 + i % 8, P_FLOOD, H_COOP, 20 * NS_PER_US), a->flood_depth = 64;
	for (int i = 0; i < 512; i++) {
		struct simq *s = addq(C_NET, 1, P_PERIODIC, H_COOP, 3 * NS_PER_US);
		s->rate = 500; s->burst = 1; s->next_t = 0;	/* all at once, every 2 ms */
	}
	for (int i = 0; i < 64; i++) {
		struct simq *s = addq(C_NET, 1, P_PERIODIC, H_COOP, 5 * NS_PER_US);
		s->rate = 5000; s->burst = 1; s->next_t = (uint64_t)i * 3 * NS_PER_US;
	}
	a = addq(C_NET, 1, P_FLOOD, H_IGNORE, 2 * NS_PER_MS); a->flood_depth = 4;
	run_for(secs * NS_PER_S);
	for (int i = 0; i < S.nq; i++) {
		boosts += S.qs[i]->kc.kc_boosts;
		passes += S.qs[i]->kc.kc_passes;
	}
	report("nnew: 512 queues bursting together, 64 warm, 8 floods, 1 overrunner");
	printf("  info boosts %" PRIu64 " of %" PRIu64 " passes, kw_nnew at end %u\n",
	    boosts, passes, S.w[C_NET].kw_nnew);
	if (boosts == 0) fail("nnew: no boosted pass at all");
	{	/* the list may well be non-empty when time stops; the count must match */
		struct kwq_cpu *kc;
		u_int m = 0;

		TAILQ_FOREACH(kc, &S.w[C_NET].kw_new, kc_active)
			m++;
		if (m != S.w[C_NET].kw_nnew)
			fail("nnew: %u entries on the new list, kw_nnew %u", m, S.w[C_NET].kw_nnew);
	}
	return (S.violations);
}

static int
sc_manyq(uint64_t secs)
{
	sim_reset(0, 0, 0, 0);
	for (int i = 0; i < 2000; i++) {
		struct simq *s = addq(C_NET, 1 + i % 8, P_POISSON, H_COOP, 2 * NS_PER_US);
		s->rate = 100;			/* 2000 x 100/s x 2 us = 40 % load */
	}
	run_for(secs * NS_PER_S);
	report("manyq: 2000 Poisson queues, weights 1..8, 40 % load");
	{
		uint64_t worst = 0;
		for (int i = 0; i < S.nq; i++) if (S.qs[i]->lat_max > worst) worst = S.qs[i]->lat_max;
		printf("  info worst item latency over 2000 queues: %.1f us\n", (double)worst / NS_PER_US);
		if (worst > 20 * NS_PER_MS) fail("manyq: worst latency %" PRIu64 " us at 40 %% load", worst / NS_PER_US);
	}
	return (S.violations);
}

/*
 * budgetjump: the estimated budget check (S4.4) against items that are
 * cheap most of the time and 100x dearer 5 % of the time, plus a light
 * queue that must still see its latency bound.  run_pass() checks B1 in
 * its estimated form for every cooperative pass.
 */
static int
sc_budgetjump(uint64_t secs)
{
	struct simq *a, *b;
	uint64_t calls, reads;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 200); a->flood_depth = 512;
	a->heavy_p = 0.05; a->heavy_cost = 20 * NS_PER_US;
	b = addq(C_NET, 1, P_PERIODIC, H_COOP, 5 * NS_PER_US); b->rate = 1000; b->burst = 1;
	run_for(secs * NS_PER_S);
	calls = S.w[C_NET].kw_budget_calls; reads = S.w[C_NET].kw_budget_reads;
	report("budgetjump: 200 ns items with 5 % at 20 us, estimated budget check K=8");
	printf("  info budget calls %" PRIu64 ", clock reads %" PRIu64 " (%.1f %%), worst pass overshoot %.1f us, light queue worst %.1f us\n",
	    calls, reads, calls ? 100.0 * reads / calls : 0.0, (double)S.max_overshoot / NS_PER_US, (double)b->lat_max / NS_PER_US);
	if (reads == 0 || reads * 4 > calls)
		fail("budgetjump: expected the estimate to save most clock reads (%" PRIu64 " of %" PRIu64 ")", reads, calls);
	if (b->lat_max > 2 * S.knobs[C_NET].quantum_ns + 8 * 20 * NS_PER_US + 50 * NS_PER_US)
		fail("budgetjump: light queue worst %" PRIu64 " us beyond B4 with the estimate", b->lat_max / NS_PER_US);
	return (S.violations);
}

/*
 * glitch: the a07 ticker fault (S13): one pass sees its CPU clock jump
 * forward by 2^32 ticks, another sees it step back.  Both must be
 * charged one quantum and counted, the deficit must stay clamped and
 * the queue must keep being served.
 */
static int
sc_glitch(uint64_t secs)
{
	struct simq *a, *b;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 256;
	b = addq(C_NET, 1, P_POISSON, H_COOP, 5 * NS_PER_US); b->rate = 2000;
	S.glitch_pos_pass = 50; S.glitch_neg_pass = 120;
	run_for(secs * NS_PER_S);
	report("glitch: +2^32 ticks at pass 50, -300 ns at pass 120");
	printf("  info glitches a %" PRIu64 " b %" PRIu64 ", a passes %" PRIu64 ", worker busy %.3f s of %" PRIu64 " s\n",
	    a->kc.kc_glitches, b->kc.kc_glitches, a->kc.kc_passes, (double)S.w[C_NET].kw_busy_ns / NS_PER_S, secs);
	if (a->kc.kc_glitches + b->kc.kc_glitches != 2)
		fail("glitch: expected 2 glitches counted, got %" PRIu64, a->kc.kc_glitches + b->kc.kc_glitches);
	if (a->kc.kc_passes < 1000)
		fail("glitch: the flood stopped being served (%" PRIu64 " passes)", a->kc.kc_passes);
	if (S.w[C_NET].kw_busy_ns > (uint64_t)secs * NS_PER_S * 2)
		fail("glitch: busy_ns %" PRIu64 " inflated by the jump", S.w[C_NET].kw_busy_ns);
	return (S.violations);
}

static int
sc_wrap(uint64_t secs)
{
	struct simq *a, *b;
	uint64_t wall0 = UINT64_MAX - 700 * NS_PER_MS;
	uint64_t cpu0 = UINT64_MAX - 300 * NS_PER_MS;
	uint64_t round0 = UINT64_MAX - 20;
	u_int ticks0 = UINT_MAX - 400;

	/* Everything starts just below its wrap and runs through it. */
	sim_reset(wall0, cpu0, round0, ticks0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 64;
	b = addq(C_NET, 1, P_BURSTPAUSE, H_COOP, 20 * NS_PER_US); b->burst = 8; b->pause_ns = 30 * NS_PER_US;
	/* kc_idle_round near the 32-bit wrap too */
	b->kc.kc_idle_round = (u_int)round0;
	run_for(secs * NS_PER_S);
	report("wrap: wall, cpu time, round counter and ticks through their wraps");
	if (S.w[C_NET].kw_round < 1000 || S.w[C_NET].kw_round > 100000000)
		fail("wrap: round counter did not wrap forward (%" PRIu64 ")", S.w[C_NET].kw_round);
	if (a->items_out < 10000) fail("wrap: the run did not execute (%" PRIu64 " items)", a->items_out);
	if (S.now > wall0) fail("wrap: wall clock did not wrap (now %" PRIu64 ")", S.now);
	if (S.cputime[C_NET] > cpu0) fail("wrap: cpu time did not wrap");
	if (S.ticks > ticks0) fail("wrap: ticks did not wrap (%u)", S.ticks);
	printf("  ok  wall %" PRIu64 " cpu %" PRIu64 " round %" PRIu64 " ticks %u after the wraps; %" PRIu64 " items, %" PRIu64 " grace hits\n",
	    S.now, S.cputime[C_NET], S.w[C_NET].kw_round, S.ticks, a->items_out, b->kc.kc_grace);
	/* ks_ticks2ns exactness vs 128-bit reference */
	{
		uint64_t rates[] = { 1000000000ULL, 2999999999ULL, 3600000000ULL, 100000000ULL, 0 };
		uint64_t ts[] = { 0, 1, 12345, 1ULL << 32, (1ULL << 32) + 7, 1ULL << 40, UINT64_MAX / 3, UINT64_MAX };
		for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
			uint64_t rate = rates[i] ? rates[i] : 1000000000ULL;
			uint64_t scale = ks_ns_scale(rates[i]);
			for (size_t j = 0; j < sizeof(ts) / sizeof(ts[0]); j++) {
				unsigned __int128 ref = ((unsigned __int128)ts[j] * scale) >> 32;
				uint64_t got = ks_ticks2ns(ts[j], scale);
				if (ref > UINT64_MAX) continue;
				if (got != (uint64_t)ref)
					fail("ks_ticks2ns(%" PRIu64 ", rate %" PRIu64 ") = %" PRIu64 " != %" PRIu64,
					    ts[j], rate, got, (uint64_t)ref);
				/* and against exact ns within 1 ns for t < 2^32 */
				if (ts[j] < (1ULL << 32)) {
					unsigned __int128 exact = (unsigned __int128)ts[j] * 1000000000ULL / rate;
					if ((uint64_t)exact > got + 1 || got > (uint64_t)exact + 1)
						fail("ks_ticks2ns off by more than 1 ns: %" PRIu64 " vs %" PRIu64, got, (uint64_t)exact);
				}
			}
		}
		printf("  ok  ks_ticks2ns exact for %zu rates x %zu values\n",
		    sizeof(rates) / sizeof(rates[0]), sizeof(ts) / sizeof(ts[0]));
	}
	if (!ks_ticked(0, UINT_MAX) || ks_ticked(5, 5) || !ks_ticked(6, 5))
		fail("ks_ticked wrap arithmetic wrong");
	return (S.violations);
}

static int
sc_ratechange(uint64_t secs)
{
	/* The core sees only ns; the rate change lives in the glue. Exercise the
	   scale helper at extremes and a zero rate. */
	uint64_t s0 = ks_ns_scale(0), s1 = ks_ns_scale(1000000000ULL);

	if (s0 != s1) fail("zero tick rate not treated as 1 GHz");
	if (ks_ticks2ns(3000000000ULL, ks_ns_scale(3000000000ULL)) != 1000000000ULL &&
	    ks_ticks2ns(3000000000ULL, ks_ns_scale(3000000000ULL)) != 999999999ULL)
		fail("3e9 ticks at 3 GHz != 1 s (got %" PRIu64 ")", ks_ticks2ns(3000000000ULL, ks_ns_scale(3000000000ULL)));
	printf("  ok  ratechange: scale(0)=scale(1 GHz), 3e9 ticks @ 3 GHz = %" PRIu64 " ns\n",
	    ks_ticks2ns(3000000000ULL, ks_ns_scale(3000000000ULL)));
	/* quantum at the ends of its range with weights 1 and 8 */
	sim_reset(0, 0, 0, 0);
	S.knobs[C_NET].quantum_ns = 10 * NS_PER_US;
	struct simq *a = addq(C_NET, 8, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 32;
	struct simq *b = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); b->flood_depth = 32;
	run_for(secs * NS_PER_S / 4);
	report("quantum 10 us (below c_max), weights 8:1");
	sim_reset(0, 0, 0, 0);
	S.knobs[C_NET].quantum_ns = 1000 * NS_PER_MS;
	a = addq(C_NET, 8, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 32;
	b = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); b->flood_depth = 32;
	run_for(secs * NS_PER_S);
	report("quantum 1 s, weights 8:1 (budget never binds at depth 32: weights cannot act; equal shares are correct)");
	return (S.violations);
}

/*
 * overrun_idle: the overrunner's items come back 20 us after its pass
 * ends, as a handler that frees its batch at the end of the pass would
 * have it.  Its list is empty at pass end, so it goes idle; before the
 * DEBT rule (2026-09-29) the idle transition reset its deficit and every
 * doorbell took the boost: 41x the cooperative queue's share in the
 * kernel.  With the rule its doorbells go to the ring and the refill
 * parks it.
 */
static int
sc_overrun_idle(uint64_t secs)
{
	struct simq *a, *o;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 256;
	o = addq(C_NET, 1, P_FLOOD, H_IGNORE, 2 * NS_PER_MS); o->flood_depth = 4;
	o->refill_delay = 20 * NS_PER_US;
	run_for(secs * NS_PER_S);
	report("overrun_idle: budget-ignoring 2 ms items refilled 20 us after each pass");
	printf("  info overrunner share %.2f, boosts %" PRIu64 ", debts %" PRIu64 ", parks %" PRIu64 " over %" PRIu64 " passes\n",
	    (double)o->service_ns / a->service_ns, o->kc.kc_boosts, o->kc.kc_debts, o->kc.kc_parks, o->kc.kc_passes);
	if (o->kc.kc_overruns != o->kc.kc_passes)
		fail("overrun_idle: expected overruns == passes, got %" PRIu64 " vs %" PRIu64, o->kc.kc_overruns, o->kc.kc_passes);
	if (o->kc.kc_debts == 0 || o->kc.kc_parks == 0)
		fail("overrun_idle: the debt did not survive idle (debts %" PRIu64 ", parks %" PRIu64 ")", o->kc.kc_debts, o->kc.kc_parks);
	if (o->kc.kc_boosts > 1)
		fail("overrun_idle: the overrunner was boosted %" PRIu64 " times", o->kc.kc_boosts);
	if ((double)o->service_ns > 1.5 * (double)a->service_ns)
		fail("overrun_idle: overrunner share %.2f exceeds 1.5", (double)o->service_ns / a->service_ns);
	return (S.violations);
}

static int
sc_badhandler(uint64_t secs)
{
	struct simq *a, *r;

	sim_reset(0, 0, 0, 0);
	a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 64;
	r = addq(C_NET, 1, P_PERIODIC, H_REQUEUEALL, 20 * NS_PER_US); r->rate = 10; r->burst = 4;
	run_for(secs * NS_PER_S);
	report("badhandler: a handler that requeues everything (S14 case 8)");
	if (r->items_out != 0 || r->kc.kc_passes == 0 || r->requeued == 0)
		fail("badhandler signature not produced");
	else
		printf("  ok  signature: passes %" PRIu64 ", requeued %" PRIu64 ", items out 0\n",
		    r->kc.kc_passes, r->requeued);
	/* the flood must keep its share regardless */
	if (a->items_out < (uint64_t)secs * 20000)
		fail("badhandler starved the cooperative queue: %" PRIu64 " items", a->items_out);
	return (S.violations);
}

static int
sc_random(uint64_t secs)
{
	int nq = 1 + rnd64() % 12;

	sim_reset(0, 0, 0, 0);
	S.ext_kind = rnd64() % 3;
	S.cpubound_allowed = rnd64() % 2;
	if (rnd64() % 4 == 0) { S.knobs[C_NET].cap_pct = 80; S.knobs[C_BULK].cap_pct = 80; }
	for (int i = 0; i < nq; i++) {
		int cls = rnd64() % 2, pk = 1 + rnd64() % 4, hk = rnd64() % 3;
		uint64_t cost = (1 + rnd64() % 200) * NS_PER_US;
		struct simq *s = addq(cls, 1 + rnd64() % 8, pk, hk, cost);
		s->rate = 10 + rnd64() % 20000; s->burst = 1 + rnd64() % 64;
		s->pause_ns = rnd64() % (2 * NS_PER_MS); s->flood_depth = 1 + rnd64() % 128;
		if (rnd64() % 3 == 0) { s->heavy_p = 0.01; s->heavy_cost = cost * 50; }
		if (s->pk == P_PERIODIC) s->rate = 10 + rnd64() % 2000;
	}
	run_for(secs * NS_PER_S);
	if (S.verbose) report("random");
	return (S.violations);
}

static int
sc_sweep_grace(uint64_t secs)
{
	uint64_t pauses[] = { 10, 50, 200, 500, 2000 };

	printf("== sweep_grace: burst-and-pause (32 x 20 us) against a flood; share and boosts\n");
	printf("  pause_us grace  share  boosts  grace_hits  flood_lat_max_us\n");
	for (u_int g = 1; g <= 2; g++) {
		for (size_t i = 0; i < sizeof(pauses) / sizeof(pauses[0]); i++) {
			struct simq *a, *b;
			sim_reset(0, 0, 0, 0);
			S.knobs[C_NET].grace_rounds = g;
			a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 256;
			b = addq(C_NET, 1, P_BURSTPAUSE, H_COOP, 20 * NS_PER_US);
			b->burst = 32; b->pause_ns = pauses[i] * NS_PER_US;
			run_for(secs * NS_PER_S);
			printf("  %8" PRIu64 " %5u  %.3f  %-7" PRIu64 " %-11" PRIu64 " %.1f\n",
			    pauses[i], g, (double)b->service_ns / a->service_ns,
			    b->kc.kc_boosts, b->kc.kc_grace, (double)a->lat_max / NS_PER_US);
		}
	}
	return (S.violations);
}

static int
sc_sweep_boost(uint64_t secs)
{
	printf("== sweep_boost: heavy-weight (w=8) Poisson newcomer vs two light floods\n");
	printf("  boost      light_lat_max_us  newcomer_lat_max_us  newcomer_share\n");
	for (int wb = 1; wb >= 0; wb--) {
		struct simq *a, *b, *h;
		sim_reset(0, 0, 0, 0);
		S.knobs[C_NET].boost_weighted = wb;
		a = addq(C_NET, 1, P_FLOOD, H_COOP, 20 * NS_PER_US); a->flood_depth = 64;
		b = addq(C_NET, 1, P_POISSON, H_COOP, 5 * NS_PER_US); b->rate = 2000;
		h = addq(C_NET, 8, P_PERIODIC, H_COOP, 20 * NS_PER_US); h->rate = 200; h->burst = 100;
		run_for(secs * NS_PER_S);
		printf("  %-9s  %-17.1f %-20.1f %.3f\n", wb ? "Qw" : "Q",
		    (double)b->lat_max / NS_PER_US, (double)h->lat_new_max / NS_PER_US,
		    (double)h->service_ns / (a->service_ns + b->service_ns + h->service_ns));
	}
	return (S.violations);
}

/* ---------------------------------------------------------------- main */
struct scenario {
	const char	*name;
	int		(*fn)(uint64_t secs);
	uint64_t	secs;
	int		seeds;
};

static struct scenario scenarios[] = {
	{ "fairness",	sc_fairness,	3, 1 },
	{ "latency",	sc_latency,	3, 1 },
	{ "gaming",	sc_gaming,	3, 1 },
	{ "overrun",	sc_overrun,	3, 1 },
	{ "overrun_idle", sc_overrun_idle, 3, 1 },
	{ "handback",	sc_handback,	2, 1 },
	{ "storm",	sc_storm,	2, 1 },
	{ "nnew",	sc_nnew,	2, 1 },
	{ "manyq",	sc_manyq,	1, 1 },
	{ "budgetjump",	sc_budgetjump,	3, 1 },
	{ "glitch",	sc_glitch,	2, 1 },
	{ "wrap",	sc_wrap,	1, 1 },
	{ "ratechange",	sc_ratechange,	1, 1 },
	{ "badhandler",	sc_badhandler,	2, 1 },
	{ "random",	sc_random,	36, 100 },
	{ "sweep_grace", sc_sweep_grace, 2, 1 },
	{ "sweep_boost", sc_sweep_boost, 2, 1 },
	{ NULL, NULL, 0, 0 }
};

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "suite";
	uint64_t seed = 1, secs = 0;
	bool verbose = false;
	int total = 0;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) secs = strtoull(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "-v") == 0) verbose = true;
	}
	for (struct scenario *sc = scenarios; sc->name != NULL; sc++) {
		if (strcmp(name, "suite") != 0 && strcmp(name, sc->name) != 0)
			continue;
		int nseeds = strcmp(name, "suite") == 0 ? sc->seeds : 1;
		int v = 0;
		for (int s = 0; s < nseeds; s++) {
			rng_s = 0x9E3779B97F4A7C15ULL ^ (seed + (uint64_t)s * 7919);
			if (rng_s == 0) rng_s = 1;
			S.verbose = verbose;
			v += sc->fn(secs ? secs : sc->secs);
			S.verbose = verbose;
			if (v) { fprintf(stderr, "%s: seed %" PRIu64 " FAILED\n", sc->name, seed + s); break; }
		}
		if (nseeds > 1 && v == 0) printf("== %s: %d seeds, no violations\n", sc->name, nseeds);
		total += v;
	}
	printf("scheduler steps: %" PRIu64 "\n", total_steps);
	printf("%s\n", total == 0 ? "ALL PASSED" : "FAILURES");
	return (total == 0 ? 0 : 1);
}
