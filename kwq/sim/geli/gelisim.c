/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * gelisim - a discrete-event model of kwq-geli on many CPUs, to try
 * changes to where a pass may stop before any of them reaches a kernel.
 * It links the real scheduler core (../../kwq/kwq_sched.c with
 * keepturn.patch), so the ring, the deficits and the share limit are the
 * kernel's; what is modelled here is around them: GELI's handler, the
 * threads a bio passes through, ULE's priorities, and what a gap between
 * two parts of a bio costs in cache misses.
 *
 * Per CPU a NET and a BULK worker and two run queues of other threads:
 * kernel range (md(4), PRIBIO) and user.  In priority order: NET, BULK,
 * kernel range, a worker yielded at PUSER (NET first), user threads, a
 * worker waiting at cap_prio.  A worker in a pass runs at its class
 * priority.  NET preempts BULK; either preempts the others; kernel range
 * and user threads preempt nothing.
 *
 * A write: user thread, the crypto in a BULK pass (the whole bio copied
 * first, then the sectors), md(4)'s copy, user thread again, the next
 * request.  A read: md(4), the crypto, user thread.
 *
 * The cache model: T = cache_mb / (traffic x the bytes per second moved
 * now).  What a bio left behind is gone by 1 - exp(-age / T).  The rest
 * of a resumed bio costs (1 + cold_crypto x gone) per sector; md(4)'s
 * copy costs (1 + cold_md x gone), taken over the ages of the sectors.
 *
 *   gelisim key=value ...           (see struct opts; one output line)
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kwq_sched.h"

#define	US	1000ULL
#define	MS	1000000ULL
#define	SEC	1000000000ULL
#define	MIB	1048576ULL
#define	MAXCPU	128
#define	PASS_NS	250		/* SCHED.md S15.2 */
#define	STEAL_SCAN 16
#define	STEAL_MAX 64

enum { NET = 0, BULK = 1, NCLS = 2 };

struct opts {
	int	ncpu, nprov, secsize, bytes, budget_sectors, k;
	int	keep, trule_us, fit, shared, owner, steal, pin, seed;
	int	quantum_us, cap_period_us, cap_permille;
	double	secs, warm;
	double	crypto_ms, copy_ms, md_ms;	/* per MiB */
	int	user_us, bio_ns;
	double	ext_aff;			/* wakes that land on the waker's CPU */
	double	jitter;				/* a unit's cost varies by +- this share */
	int	noise_hz;			/* short kernel threads per CPU per second */
	double	cache_mb, traffic, cold_crypto, cold_md;
	int	timer;				/* 1: a 1 kHz thread on CPU 1 */
	int	net;				/* 1 ping, 2 flood, 3 both */
	int	netcpu, npairs, net_item_ns, net_window, net_rtt_us;
	int	nsmall, smallq;			/* 4 KiB readers, in flight each */
	int	wq;				/* writes in flight per provider */
	int	spin;				/* 1: a user thread that never sleeps on CPU pin */
	int	mixq;				/* 1: the readers use provider 0's queue */
	int	verbose;
} O = {
	.ncpu = 16, .nprov = 64, .secsize = 4096, .bytes = 1048576,
	.budget_sectors = 16, .k = 8, .steal = 1, .pin = -1, .seed = 1,
	.quantum_us = 5000, .cap_period_us = 10000, .cap_permille = 500,
	.secs = 2.0, .warm = 0.3,
	.crypto_ms = 0.27, .copy_ms = 0.28, .md_ms = 0.32, .user_us = 300,
	.bio_ns = 200, .jitter = 0.1, .cache_mb = 32, .traffic = 3, .cold_crypto = 0.9,
	.cold_md = 0.66, .netcpu = -1, .npairs = 1, .net_item_ns = 650,
	.net_window = 64, .net_rtt_us = 20, .smallq = 1, .wq = 1,
};

/* ------------------------------------------------------------- stats */
#define	HB	2048
struct hist { uint64_t b[HB], n, max; double sum; };
static int
hbucket(uint64_t v)
{
	int e = 0;
	uint64_t m = v;

	if (v < 32)
		return ((int)v);
	while (m >= 64) { m >>= 1; e++; }
	return (e * 32 + (int)m);
}
static uint64_t
hvalue(int b)
{
	if (b < 64)
		return ((uint64_t)b);
	return ((uint64_t)(b % 32 + 32) << (b / 32 - 1));
}
static void
hadd(struct hist *h, uint64_t v)
{
	h->b[hbucket(v)]++; h->n++; h->sum += (double)v;
	if (v > h->max) h->max = v;
}
static double
hpct(const struct hist *h, double p)
{
	uint64_t want = (uint64_t)((double)h->n * p), c = 0;

	for (int i = 0; i < HB; i++) {
		c += h->b[i];
		if (c > want)
			return ((double)hvalue(i));
	}
	return ((double)h->max);
}

static struct {
	struct hist gap, crypto, e2e, late, netwait, rtt, small, partials;
	uint64_t bios, bytes, cuts, netitems, starts, starts_over, steals, stolen_part;
	uint64_t declined, skipped_timer, cut_net, cut_cap, cut_budget, passes;
	uint64_t ns_bulk, ns_net, ns_ext, ns_idle;
} st;
static uint64_t *prov_bytes, *small_ops;

/* ------------------------------------------------------------- rng */
static uint64_t rs;
static uint64_t
rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
	return (rs);
}

/* ------------------------------------------------------------- events */
enum { E_SEG, E_CAPDL, E_TIMER, E_PING, E_FLOOD, E_NOISE, E_END };
struct ev { uint64_t t, gen; int type, cpu; void *p; };
static struct ev *H;
static int hn, hcap;
static uint64_t now;

static void
ev_push(uint64_t t, int type, int cpu, uint64_t gen, void *p)
{
	int i;

	if (hn == hcap) {
		hcap = hcap ? hcap * 2 : 4096;
		H = realloc(H, sizeof(*H) * (size_t)hcap);
	}
	i = hn++;
	while (i > 0 && H[(i - 1) / 2].t > t) { H[i] = H[(i - 1) / 2]; i = (i - 1) / 2; }
	H[i] = (struct ev){ t, gen, type, cpu, p };
}
static struct ev
ev_pop(void)
{
	struct ev top = H[0], last = H[--hn];
	int i = 0;

	for (;;) {
		int c = 2 * i + 1;

		if (c >= hn) break;
		if (c + 1 < hn && H[c + 1].t < H[c].t) c++;
		if (H[c].t >= last.t) break;
		H[i] = H[c]; i = c;
	}
	H[i] = last;
	return (top);
}

/* ------------------------------------------------------------- objects */
struct bio {
	struct bio	*next;
	int		prov, nsec, done, cuts, cut_done, owner_cpu;
	uint32_t	bytes;
	bool		write, small, copied, net;
	int		stage;		/* NET item: 0 request, 1 reply, 2 flood */
	uint64_t	t_req, t_enq, t_cut, t_done, t_ping;
	double		mult;
};

struct lst {
	struct kwq_cpu	kc;
	struct bio	*head, *tail;
	int		depth, cpu, cls;
	struct qd	*q;
};
struct qd { int cls; struct lst *pc[MAXCPU]; };

enum wst { W_SLEEP, W_READY, W_YIELD, W_LOW };
struct wrk {
	struct kwq_worker kw;
	enum wst	st;
	uint64_t	cpu_ns;		/* the charged clock: CPU time; NET uses the wall */
	bool		in_pass, tried;
	struct lst	*pl;
	struct bio	*batch, *cur;
	int		nbatch;
	uint64_t	seg_left, bio_t0;
	int		seg_sec;
	bool		seg_copy;
	u_int		swvoltick;
	uint64_t	yield_t;
};

enum { R_IDLE, R_NET, R_BULK, R_EXT };
enum tk { T_MD, T_UP, T_NOISE, T_USER, T_TIMER, T_SPIN };
struct task {
	struct task	*next;
	enum tk		kind;
	struct bio	*b;
	uint64_t	left, due;
};
struct tq { struct task *h, *t; int n; };

struct cpu {
	int		id, run;
	uint64_t	gen, seg_t0, seg_len;
	struct wrk	w[NCLS];
	struct task	*ext;
	struct tq	k, u;
	int		depth;		/* BULK items queued here, a hint for steal */
	int		partials;
	uint64_t	part_t;
	double		part_area;
	struct qd	*own;		/* option 2: the queue owning the partial bio */
	double		ns_per_byte;	/* fit: learned */
	int		topo[STEAL_SCAN];
	int		reach;
} C[MAXCPU];

static struct kwq_sched_knobs kn[NCLS];
static struct qd **Q;			/* BULK queues */
static int nq;
static struct qd *netq[2][64];		/* a and b side per pair */
static uint64_t traffic_bytes, traffic_t0;
static double tau_ns = 5e6;
static int sensors_on;

void
ks_env_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, " at %" PRIu64 "\n", now);
	abort();
}

/* ------------------------------------------------------------- helpers */
static void step(struct cpu *c, int cls);
static void dispatch(struct cpu *c);

static struct qd *
newq(int cls)
{
	struct qd *q = calloc(1, sizeof(*q));

	q->cls = cls;
	for (int c = 0; c < O.ncpu; c++) {
		struct lst *l = calloc(1, sizeof(*l));

		ks_queue_init(&l->kc);
		l->kc.kc_weight = 1;
		l->kc.kc_l = l;
		l->cpu = c; l->q = q; l->cls = cls;
		q->pc[c] = l;
	}
	return (q);
}

static void
part_touch(struct cpu *c)
{
	if (sensors_on)
		c->part_area += (double)c->partials * (double)(now - c->part_t);
	c->part_t = now;
}
static void
part_add(struct cpu *c, int d)
{
	part_touch(c);
	c->partials += d;
	if (sensors_on && d > 0)
		hadd(&st.partials, (uint64_t)c->partials);
}

static bool
others_runnable(const struct cpu *c, int cls)
{
	if (c->k.n || c->u.n)
		return (true);
	if (c->run == R_EXT)		/* it would be preempted, and is runnable */
		return (true);
	if (cls == BULK && c->w[NET].st != W_SLEEP && c->run != R_NET)
		return (true);
	if (cls == NET && c->w[BULK].st != W_SLEEP && c->run != R_BULK)
		return (true);
	return (false);
}
static bool
higher_waiting(const struct cpu *c, int cls)
{
	return (cls == BULK && c->w[NET].st == W_YIELD);	/* S6.4 */
}

/* What a worker reads as its clock. */
static uint64_t
wclock(const struct cpu *c, int cls)
{
	return (cls == NET ? now : c->w[cls].cpu_ns);
}

static void
tq_push(struct tq *q, struct task *t, bool front)
{
	t->next = NULL;
	if (front) {
		t->next = q->h; q->h = t;
		if (q->t == NULL) q->t = t;
	} else {
		if (q->t) q->t->next = t; else q->h = t;
		q->t = t;
	}
	q->n++;
}
static struct task *
tq_pop(struct tq *q)
{
	struct task *t = q->h;

	if (t) {
		q->h = t->next;
		if (q->h == NULL) q->t = NULL;
		q->n--;
	}
	return (t);
}

static void
seg_begin(struct cpu *c, int run, uint64_t dur)
{
	c->run = run; c->seg_t0 = now; c->seg_len = dur;
	ev_push(now + dur, E_SEG, c->id, ++c->gen, NULL);
}

/* Take what runs on c off it, keeping what it has left. */
static void
preempt(struct cpu *c)
{
	uint64_t el = now - c->seg_t0;

	c->gen++;
	switch (c->run) {
	case R_NET:
	case R_BULK: {
		int cls = c->run == R_NET ? NET : BULK;

		c->w[cls].cpu_ns += el;
		c->w[cls].seg_left = c->seg_len - el;
		if (sensors_on)
			*(cls == NET ? &st.ns_net : &st.ns_bulk) += el;
		break;
	}
	case R_EXT:
		c->ext->left = c->seg_len - el;
		if (sensors_on) st.ns_ext += el;
		tq_push(c->ext->kind <= T_NOISE ? &c->k : &c->u, c->ext, true);
		c->ext = NULL;
		break;
	}
	c->run = R_IDLE;
}

static void
wake(struct cpu *c, int cls)
{
	c->w[cls].st = W_READY;
	c->w[cls].tried = false;
	if (c->run == R_IDLE) {
		dispatch(c);
	} else if (c->run == R_EXT || (c->run == R_BULK && cls == NET)) {
		preempt(c);
		dispatch(c);
	}
}

/* ULE's choice for a thread that wakes: an idle CPU, else the shortest queue. */
static struct cpu *
pick_ext(int avoid)
{
	struct cpu *best = NULL;
	int bl = 1 << 30, off = (int)(rnd() % (uint64_t)O.ncpu);

	for (int i = 0; i < O.ncpu; i++) {
		struct cpu *c = &C[(i + off) % O.ncpu];
		int l;

		if (c->id == avoid && O.ncpu > 1)
			continue;
		if (c->run == R_IDLE)
			return (c);
		l = c->k.n + c->u.n + (c->run == R_EXT ? 1 : 0);
		if (l < bl) { bl = l; best = c; }
	}
	return (best);
}

static void
ext_add(struct cpu *c, enum tk kind, struct bio *b, uint64_t cost, uint64_t due)
{
	struct task *t = calloc(1, sizeof(*t));

	t->kind = kind; t->b = b; t->left = cost; t->due = due;
	tq_push(kind <= T_NOISE ? &c->k : &c->u, t, false);
	if (c->run == R_IDLE)
		dispatch(c);
}
static void
ext_spawn(enum tk kind, struct bio *b, uint64_t cost, int from)
{
	struct cpu *c;

	if (O.pin < 0 && O.ext_aff > 0 && (double)(rnd() >> 11) / 9007199254740992.0 < O.ext_aff)
		c = &C[from];
	else
		c = pick_ext(O.pin);
	ext_add(c, kind, b, cost, 0);
}

static void
dispatch(struct cpu *c)
{
	struct wrk *n = &c->w[NET], *b = &c->w[BULK];
	int cls = -1;

	if (n->st == W_READY) cls = NET;
	else if (b->st == W_READY) cls = BULK;
	else if (c->k.n) goto ext;
	else if (n->st == W_YIELD) cls = NET;
	else if (b->st == W_YIELD) cls = BULK;
	else if (c->u.n) goto ext;
	else if (n->st == W_LOW) cls = NET;
	else if (b->st == W_LOW) cls = BULK;
	else return;
	c->w[cls].st = W_READY;
	c->run = cls == NET ? R_NET : R_BULK;
	if (c->w[cls].seg_left > 0)
		seg_begin(c, c->run, c->w[cls].seg_left);
	else
		step(c, cls);
	return;
ext: {
		struct task *t = tq_pop(c->k.n ? &c->k : &c->u);

		c->ext = t;
		if (t->kind == T_TIMER && t->due != 0) {
			if (sensors_on) hadd(&st.late, now - t->due);
			t->due = 0;
		}
		seg_begin(c, R_EXT, t->left);
	}
}

/* ------------------------------------------------------------- lists */
static void
list_append(struct lst *l, struct bio *b)
{
	b->next = NULL;
	if (l->tail) l->tail->next = b; else l->head = b;
	l->tail = b;
	l->depth++;
	if (l->cls == BULK) C[l->cpu].depth++;
}
static void
list_prepend(struct lst *l, struct bio *b)
{
	b->next = l->head;
	l->head = b;
	if (l->tail == NULL) l->tail = b;
	l->depth++;
	if (l->cls == BULK) C[l->cpu].depth++;
}
static struct bio *
list_pop(struct lst *l)
{
	struct bio *b = l->head;

	if (b) {
		l->head = b->next;
		if (l->head == NULL) l->tail = NULL;
		l->depth--;
		if (l->cls == BULK) C[l->cpu].depth--;
		b->next = NULL;
	}
	return (b);
}

static void
placed(struct lst *l)
{
	struct cpu *c = &C[l->cpu];
	struct wrk *w = &c->w[l->cls];

	if (l->kc.kc_state == KWQ_CPU_IDLE)
		ks_doorbell(&w->kw, &l->kc);
	if (w->st == W_SLEEP)
		wake(c, l->cls);
}

/* KWQ_CPU_ANY, kwq_pick_any(). */
static int
pick_any(struct qd *q, int me)
{
	int best = me, bd = q->pc[me]->depth;

	for (int i = 0; i < C[me].reach; i++) {
		int c = C[me].topo[i];

		if (C[c].w[BULK].st == W_SLEEP && q->pc[c]->depth == 0)
			return (c);
	}
	for (int i = 0; i < C[me].reach && i < 4 && bd != 0; i++) {
		int c = C[me].topo[i];

		if (q->pc[c]->depth < bd) { best = c; bd = q->pc[c]->depth; }
	}
	return (best);
}

static void
bio_enqueue(struct bio *b, int me)
{
	struct qd *q = Q[O.shared || (b->small && O.mixq) ? 0 : b->prov];
	int cpu = O.pin >= 0 ? O.pin : pick_any(q, me);

	b->t_enq = now;
	list_append(q->pc[cpu], b);
	placed(q->pc[cpu]);
}

/* ------------------------------------------------------------- the load */
static struct bio *
bio_new(int prov, bool small)
{
	struct bio *b = calloc(1, sizeof(*b));

	b->prov = prov;
	b->small = small;
	b->write = !small;
	b->bytes = small ? 4096 : (uint32_t)O.bytes;
	b->nsec = (int)((b->bytes + (uint32_t)O.secsize - 1) / (uint32_t)O.secsize);
	b->mult = 1.0;
	b->t_req = now;
	b->owner_cpu = -1;
	return (b);
}
static uint64_t
md_cost(uint32_t bytes)
{
	return ((uint64_t)(O.md_ms * 1e6 * bytes / (double)MIB));
}
static double
gone(uint64_t age)
{
	return (1.0 - exp(-(double)age / tau_ns));
}
static void
request(int prov, bool small, int me)
{
	struct bio *b = bio_new(prov, small);

	if (small)		/* md(4) first, then the decryption */
		ext_spawn(T_MD, b, md_cost(b->bytes) + 2 * US, me);
	else
		bio_enqueue(b, me);
}

static void
bio_crypto_done(struct cpu *c, struct bio *b)
{
	uint64_t cost;

	b->t_done = now;
	if (sensors_on) {
		hadd(&st.crypto, now - b->t_enq);
		st.cuts += (uint64_t)b->cuts;
	}
	traffic_bytes += b->bytes;
	if (b->write) {
		/* md(4)'s copy: what the bio wrote before its last cut is older */
		double pre = (double)b->cut_done / b->nsec;
		double g = pre * gone(now - b->t_cut) + (1 - pre) * gone(100 * US);

		cost = (uint64_t)((double)md_cost(b->bytes) * (1 + O.cold_md * (b->cuts ? g : 0)));
		ext_spawn(T_MD, b, cost, c->id);
	} else {
		ext_spawn(T_USER, b, 2 * US, c->id);
	}
}

static void
ext_done(struct cpu *c, struct task *t)
{
	struct bio *b = t->b;

	if (t->kind == T_SPIN) {
		t->left = 100 * US;
		tq_push(&c->u, t, false);
		return;
	}
	if (t->kind == T_TIMER || t->kind == T_NOISE) {
		free(t);
		return;
	}
	if (t->kind == T_MD) {
		if (b->small) {		/* the read's data is here */
			bio_enqueue(b, c->id);
		} else {
			traffic_bytes += b->bytes;
			ext_spawn(T_UP, b, (uint64_t)O.user_us * US / 4, c->id);
		}
		free(t);
		return;
	}
	if (t->kind == T_UP) {		/* g_up, then the user thread's turn */
		ext_spawn(T_USER, b, (uint64_t)O.user_us * US * 3 / 4, c->id);
		free(t);
		return;
	}
	/* T_USER: the request is complete, the next one goes out */
	if (sensors_on) {
		if (b->small) { hadd(&st.small, now - b->t_req); small_ops[b->prov]++; }
		else { hadd(&st.e2e, now - b->t_req); st.bios++; st.bytes += b->bytes; prov_bytes[b->prov] += b->bytes; }
	}
	{
		int prov = b->prov;
		bool small = b->small;

		free(b);
		request(prov, small, c->id);
	}
	free(t);
}

/* ------------------------------------------------------------- the budget */
enum { WHY_NONE, WHY_NET, WHY_CAP, WHY_CLAMP, WHY_DEFICIT };
static int why;

/* kwq_budget_left() of kwq.c; `why' says what the kernel's query cannot. */
static uint64_t
budget_query(struct cpu *c, int cls)
{
	struct wrk *w = &c->w[cls];
	struct kwq_worker *kw = &w->kw;
	uint64_t left;

	why = WHY_NONE;
	if (higher_waiting(c, cls)) { why = WHY_NET; return (0); }
	if (ks_cap_spent(kw) && others_runnable(c, cls)) {
		kw->kw_cap_cuts++;
		why = WHY_CAP;
		return (0);
	}
	if (!ks_budget_need_clock(kw, &left))
		return (left);
	left = ks_budget_left(kw, wclock(c, cls));
	if (left == 0)
		why = kw->kw_pass_budget < kw->kw_cur->kc_deficit ? WHY_CLAMP : WHY_DEFICIT;
	return (left);
}

/* A handler stopped: the batch goes back behind `b', if any. */
static void
hand_back(struct cpu *c, int cls, struct bio *b)
{
	struct wrk *w = &c->w[cls];
	struct lst *l = w->pl;
	struct bio *x, *arr[4096];
	int n = 0;

	for (x = w->batch; x != NULL; x = x->next)
		arr[n++] = x;
	while (n > 0)
		list_prepend(l, arr[--n]);
	w->batch = NULL; w->nbatch = 0;
	if (b != NULL)
		list_prepend(l, b);
	/* keep=1: every cut from outside keeps the turn; keep=2: NET's only */
	w->kw.kw_cut_out = why == WHY_NET ||
	    (O.keep == 1 && (why == WHY_CAP || why == WHY_CLAMP));
}

/* ------------------------------------------------------------- the passes */
static void
pass_begin(struct cpu *c, int cls, struct kwq_cpu *kc)
{
	struct wrk *w = &c->w[cls];
	struct lst *l = kc->kc_l;
	int take = l->depth;
	struct bio **tail = &w->batch;

	w->pl = l; w->in_pass = true; w->cur = NULL; w->batch = NULL; w->nbatch = 0;
	(void)ks_pass_begin(&w->kw, kc, wclock(c, cls), now, others_runnable(c, cls));
	if (cls == BULK && take > 1 && O.steal) {	/* the pass cut: help a sleeper */
		for (int i = 0; i < c->reach; i++) {
			struct cpu *v = &C[c->topo[i]];

			if (v->w[BULK].st == W_SLEEP) { take = 1; wake(v, BULK); break; }
		}
	}
	while (take-- > 0) {
		struct bio *b = list_pop(l);

		*tail = b; tail = &b->next; w->nbatch++;
	}
	if (cls == NET && sensors_on && w->batch != NULL && !w->batch->net)
		abort();
	if (sensors_on) st.passes++;
}

static void
pass_end(struct cpu *c, int cls)
{
	struct wrk *w = &c->w[cls];
	struct lst *l = w->pl;
	enum ks_cap cap;

	w->in_pass = false;
	ks_pass_end(&w->kw, &l->kc, wclock(c, cls), l->depth > 0);
	cap = ks_cap_check(&w->kw, now, others_runnable(c, cls));
	if (cap == KS_CAP_DEMOTE) {
		uint64_t left = kn[cls].cap_period_ns - (now - w->kw.kw_cap_start);

		ev_push(now + left, E_CAPDL, c->id, (uint64_t)cls, NULL);
	}
	if (cap == KS_CAP_DEMOTE || cap == KS_CAP_YIELD)
		w->st = W_LOW;
}

/*
 * GELI's handler, a step at a time: returns true with a segment started,
 * false when the pass is over.
 */
static bool
bulk_handler(struct cpu *c)
{
	struct wrk *w = &c->w[BULK];
	struct bio *b;
	uint64_t dur;
	int stop, unit;

	if (w->cur == NULL) {
		if (w->batch == NULL)
			return (false);
		/* option 2: a partial bio of another queue owns this CPU */
		if (O.owner && c->own != NULL && c->own != w->pl->q &&
		    w->batch->done == 0) {
			why = WHY_NONE;
			hand_back(c, BULK, NULL);
			w->seg_sec = 0;
			seg_begin(c, R_BULK, PASS_NS);	/* an empty pass still costs */
			return (true);
		}
		b = w->batch;
		if (O.fit && b->done == 0 && !b->small) {
			uint64_t left = budget_query(c, BULK);
			double est = c->ns_per_byte * b->bytes;

			if (est > 0 && (double)left < est &&
			    est <= (double)kn[BULK].cap_allow_ns) {
				if (sensors_on) st.declined++;
				why = WHY_NONE;
				hand_back(c, BULK, NULL);
				ks_cap_decline(&w->kw);
				w->seg_sec = 0;
				seg_begin(c, R_BULK, PASS_NS);
				return (true);
			}
		}
		w->batch = b->next; b->next = NULL; w->nbatch--;
		w->cur = b;
		w->bio_t0 = w->cpu_ns;
		if (b->done > 0 || b->cuts > 0) {	/* resumed */
			uint64_t gap = now - b->t_cut;

			if (sensors_on) hadd(&st.gap, gap);
			b->mult = 1 + O.cold_crypto * gone(gap);
			part_add(c, -1);
		} else {
			if (sensors_on) {
				st.starts++;
				if (c->partials > 0) st.starts_over++;
			}
		}
	}
	b = w->cur;
	/* the next stop: every budget_sectors sectors, none if 0 */
	if (O.budget_sectors > 0) {
		stop = (b->done / O.budget_sectors + 1) * O.budget_sectors;
		if (stop > b->nsec) stop = b->nsec;
	} else {
		stop = b->nsec;
	}
	unit = stop - b->done;
	dur = (uint64_t)((double)unit * (O.crypto_ms * 1e6 / (double)(MIB / (uint64_t)O.secsize)) * b->mult);
	if (b->done == 0 && !b->copied) {
		dur += O.bio_ns;
		if (b->write)
			dur += (uint64_t)(O.copy_ms * 1e6 * b->bytes / (double)MIB);
		b->copied = true;
	}
	w->seg_sec = unit;
	if (w->seg_left == 0)
		dur += PASS_NS / 8;
	if (O.jitter > 0)
		dur = (uint64_t)((double)dur * (1 + O.jitter *
		    ((double)(rnd() >> 11) / 4503599627370496.0 - 1)));
	seg_begin(c, R_BULK, dur);
	return (true);
}

/* A unit of a bio ended. */
static void
bulk_unit_done(struct cpu *c)
{
	struct wrk *w = &c->w[BULK];
	struct bio *b = w->cur;

	if (b == NULL)			/* an empty pass's overhead */
		return;
	b->done += w->seg_sec;
	if (b->done >= b->nsec) {
		double npb = (double)(w->cpu_ns - w->bio_t0) / b->bytes;

		c->ns_per_byte = c->ns_per_byte == 0 ? npb : c->ns_per_byte * 0.875 + npb * 0.125;
		if (b->cuts > 0 && c->own != NULL && O.owner &&
		    b->owner_cpu >= 0) {
			C[b->owner_cpu].own = NULL;
			b->owner_cpu = -1;
		}
		w->cur = NULL;
		bio_crypto_done(c, b);
		if (w->batch != NULL && budget_query(c, BULK) == 0)
			hand_back(c, BULK, NULL);
		return;
	}
	/* the check inside a bio, g_eli_privacy.c:386 */
	if (budget_query(c, BULK) != 0)
		return;
	if (O.trule_us > 0 && why != WHY_NET &&
	    w->cpu_ns - w->bio_t0 < (uint64_t)O.trule_us * US)
		return;			/* short enough to finish */
	if (sensors_on) {
		if (why == WHY_NET) st.cut_net++;
		else if (why == WHY_CAP || why == WHY_CLAMP) st.cut_cap++;
		else st.cut_budget++;
	}
	b->cuts++;
	b->t_cut = now;
	b->cut_done = b->done;
	if (O.owner && c->own == NULL) { c->own = w->pl->q; b->owner_cpu = c->id; }
	part_add(c, 1);
	w->cur = NULL;
	hand_back(c, BULK, b);
}

static bool
net_handler(struct cpu *c)
{
	struct wrk *w = &c->w[NET];
	struct bio *b;

	if (w->batch == NULL)
		return (false);
	b = w->batch;
	w->batch = b->next; b->next = NULL; w->nbatch--;
	w->cur = b;
	if (sensors_on && b->t_enq != 0) hadd(&st.netwait, now - b->t_enq);
	seg_begin(c, R_NET, (uint64_t)O.net_item_ns);
	return (true);
}
static void
net_enqueue(struct bio *b, int side, int cpu)
{
	struct qd *q = netq[side][b->prov];

	b->t_enq = now;
	list_append(q->pc[cpu], b);
	placed(q->pc[cpu]);
}
static void
net_item_done(struct cpu *c)
{
	struct wrk *w = &c->w[NET];
	struct bio *b = w->cur;

	w->cur = NULL;
	if (sensors_on) st.netitems += b->stage == 2 ? 1 : 0;
	switch (b->stage) {
	case 0:
		b->stage = 1;
		net_enqueue(b, 1, c->id);
		break;
	case 1:
		if (sensors_on) hadd(&st.rtt, now - b->t_ping);
		free(b);
		break;
	default:
		ev_push(now + (uint64_t)O.net_rtt_us * US, E_FLOOD, c->id, 0, b);
		break;
	}
	if (w->batch != NULL && budget_query(c, NET) == 0)
		hand_back(c, NET, NULL);
}

/* The worker's loop, up to its next segment or until it leaves the CPU. */
static void
step(struct cpu *c, int cls)
{
	struct wrk *w = &c->w[cls];
	struct kwq_cpu *kc;

	for (;;) {
		enum ks_action a;

		if (w->in_pass) {
			if (cls == BULK ? bulk_handler(c) : net_handler(c))
				return;
			pass_end(c, cls);
			if (w->st != W_READY) {
				c->run = R_IDLE;
				dispatch(c);
				return;
			}
			/* tick guard, S6.1 */
			if (now / MS != w->swvoltick && others_runnable(c, cls)) {
				w->swvoltick = (u_int)(now / MS);
				w->kw.kw_yields++;
				w->st = W_YIELD;
				c->run = R_IDLE;
				dispatch(c);
				return;
			}
			continue;
		}
		a = ks_next(&w->kw, higher_waiting(c, cls), &kc);
		switch (a) {
		case KS_IDLE:
			if (cls == BULK && O.steal && !w->tried) {
				bool moved = false;

				w->tried = true;
				for (int i = 0; i < c->reach && !moved; i++) {
					struct cpu *v = &C[c->topo[i]];

					if (v->depth == 0)
						continue;
					for (int q = 0; q < nq && !moved; q++) {
						struct lst *l = Q[q]->pc[v->id], *m = Q[q]->pc[c->id];
						int n = l->depth, take;
						struct wrk *vw = &v->w[BULK];

						if (n == 0 || (n == 1 && !(vw->st == W_SLEEP ||
						    vw->st == W_LOW || vw->in_pass)))
							continue;
						take = n == 1 ? 1 : (n + 1) / 2;
						if (take > STEAL_MAX) take = STEAL_MAX;
						while (take-- > 0) {
							struct bio *b = list_pop(l);

							if (b->cuts > 0 && b->done < b->nsec) {
								part_add(v, -1);
								part_add(c, 1);
								if (sensors_on) st.stolen_part++;
							}
							list_append(m, b);
						}
						if (sensors_on) st.steals++;
						if (m->kc.kc_state == KWQ_CPU_IDLE)
							ks_doorbell(&w->kw, &m->kc);
						moved = true;
					}
				}
				if (moved)
					continue;
			}
			w->tried = false;
			w->st = W_SLEEP;
			c->run = R_IDLE;
			dispatch(c);
			return;
		case KS_SERVE:
			pass_begin(c, cls, kc);
			break;
		case KS_ROUND_END:
		case KS_HANDBACK:
			if (ks_round_end(&w->kw, now, others_runnable(c, cls)) == KS_Y_YIELD) {
				w->swvoltick = (u_int)(now / MS);
				w->st = W_YIELD;
				c->run = R_IDLE;
				dispatch(c);
				return;
			}
			break;
		}
	}
}

static void
seg_done(struct cpu *c)
{
	uint64_t el = c->seg_len;

	switch (c->run) {
	case R_BULK:
		c->w[BULK].cpu_ns += el;
		c->w[BULK].seg_left = 0;
		if (sensors_on) st.ns_bulk += el;
		bulk_unit_done(c);
		step(c, BULK);
		break;
	case R_NET:
		c->w[NET].cpu_ns += el;
		c->w[NET].seg_left = 0;
		if (sensors_on) st.ns_net += el;
		net_item_done(c);
		step(c, NET);
		break;
	case R_EXT: {
		struct task *t = c->ext;

		c->ext = NULL;
		if (sensors_on) st.ns_ext += el;
		c->run = R_IDLE;
		ext_done(c, t);
		if (c->run == R_IDLE)
			dispatch(c);
		break;
	}
	}
}

/* ------------------------------------------------------------- main */
static void
opt_set(const char *kv)
{
	static const struct { const char *n; int *i; double *d; } T[] = {
#define	I(x)	{ #x, &O.x, NULL }
#define	D(x)	{ #x, NULL, &O.x }
		I(ncpu), I(nprov), I(secsize), I(bytes), I(budget_sectors), I(k), I(keep),
		I(trule_us), I(fit), I(shared), I(owner), I(steal), I(pin), I(seed),
		I(quantum_us), I(cap_period_us), I(cap_permille), D(secs), D(warm),
		D(crypto_ms), D(copy_ms), D(md_ms), I(user_us), I(bio_ns), D(cache_mb),
		D(traffic), D(cold_crypto), D(cold_md), D(ext_aff), D(jitter), I(noise_hz), I(timer), I(net), I(netcpu),
		I(npairs), I(net_item_ns), I(net_window), I(net_rtt_us), I(nsmall),
		I(smallq), I(wq), I(spin), I(mixq), I(verbose),
	};
	const char *e = strchr(kv, '=');

	if (e != NULL)
		for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
			if (strlen(T[i].n) == (size_t)(e - kv) && strncmp(T[i].n, kv, (size_t)(e - kv)) == 0) {
				if (T[i].i) *T[i].i = atoi(e + 1); else *T[i].d = atof(e + 1);
				return;
			}
	fprintf(stderr, "gelisim: unknown option %s\n", kv);
	exit(2);
}

static double
jain(const uint64_t *v, int n)
{
	double s = 0, ss = 0;

	for (int i = 0; i < n; i++) { s += (double)v[i]; ss += (double)v[i] * (double)v[i]; }
	return (ss > 0 ? s * s / (n * ss) : 0);
}

int
main(int argc, char **argv)
{
	uint64_t end, warm_t, next_tau = 0;

	for (int i = 1; i < argc; i++)
		opt_set(argv[i]);
	rs = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)O.seed * 0x2545f4914f6cdd1dULL);
	prov_bytes = calloc((size_t)O.nprov + 1, sizeof(*prov_bytes));
	small_ops = calloc((size_t)O.nprov + (size_t)O.nsmall + 1, sizeof(*small_ops));
	kn[NET] = (struct kwq_sched_knobs){ .quantum_ns = 200 * US, .budget_check_every = 8,
	    .cap_permille = 500, .cap_period_ns = 1000 * US, .cap_allow_ns = 500 * US,
	    .boost_weighted = true, .penalty_rounds = 32 };
	kn[BULK] = (struct kwq_sched_knobs){ .quantum_ns = (uint64_t)O.quantum_us * US,
	    .budget_check_every = (uint32_t)O.k, .cap_permille = (uint32_t)O.cap_permille,
	    .cap_period_ns = (uint64_t)O.cap_period_us * US,
	    .cap_allow_ns = (uint64_t)O.cap_period_us * US * (uint64_t)O.cap_permille / 1000,
	    .boost_weighted = true, .penalty_rounds = 32, .keep_turn = O.keep != 0 };
	for (int i = 0; i < O.ncpu; i++) {
		struct cpu *c = &C[i];

		c->id = i;
		for (int k = 0; k < NCLS; k++) {
			ks_worker_init(&c->w[k].kw, &kn[k], 0);
			c->w[k].st = W_SLEEP;
		}
		/* kwq_topo_order(): nearest ids first, alternating */
		c->reach = O.ncpu - 1 < STEAL_SCAN ? O.ncpu - 1 : STEAL_SCAN;
		for (int d = 0, n = 0; n < c->reach; d++) {
			int s = (d / 2 + 1) * (d % 2 ? -1 : 1);

			c->topo[n++] = ((i + s) % O.ncpu + O.ncpu) % O.ncpu;
		}
	}
	nq = O.shared ? 1 : O.nprov + O.nsmall;
	Q = calloc((size_t)nq, sizeof(*Q));
	for (int i = 0; i < nq; i++)
		Q[i] = newq(BULK);
	for (int i = 0; i < O.npairs; i++) {
		netq[0][i] = newq(NET);
		netq[1][i] = newq(NET);
	}
	for (int i = 0; i < O.nprov; i++)
		for (int j = 0; j < O.wq; j++)
			request(i, false, (i + j) % O.ncpu);
	for (int i = 0; i < O.nsmall; i++)
		for (int j = 0; j < O.smallq; j++)
			request(O.nprov + i, true, (i + j) % O.ncpu);
	if (O.spin)
		ext_add(&C[O.pin >= 0 ? O.pin : 1], T_SPIN, NULL, 100 * US, 0);
	if (O.noise_hz > 0)
		for (int i = 0; i < O.ncpu; i++)
			ev_push(rnd() % (SEC / (uint64_t)O.noise_hz), E_NOISE, i, 0, NULL);
	if (O.timer)
		ev_push(1 * MS, E_TIMER, O.pin >= 0 ? O.pin : 1, 0, NULL);
	for (int i = 0; i < O.npairs; i++) {
		int cpu = O.netcpu >= 0 ? O.netcpu : (int)(rnd() % (uint64_t)O.ncpu);

		if (O.net & 1)
			ev_push(1 * MS + (uint64_t)i * 37 * US, E_PING, cpu, (uint64_t)i, NULL);
		if (O.net & 2)
			for (int j = 0; j < O.net_window; j++) {
				struct bio *b = calloc(1, sizeof(*b));

				b->prov = i; b->net = true; b->stage = 2;
				ev_push(1 * MS + (uint64_t)j * 3 * US, E_FLOOD, cpu, 0, b);
			}
	}
	warm_t = (uint64_t)(O.warm * (double)SEC);
	end = warm_t + (uint64_t)(O.secs * (double)SEC);
	ev_push(end, E_END, 0, 0, NULL);
	while (hn > 0) {
		struct ev e = ev_pop();

		now = e.t;
		if (!sensors_on && now >= warm_t) {
			sensors_on = 1;
			for (int i = 0; i < O.ncpu; i++)
				C[i].part_t = now;
		}
		if (now >= next_tau) {		/* the cache's turnover, every 5 ms */
			double dt = (double)(now - traffic_t0) / 1e9;

			if (dt > 0) {
				double r = (double)traffic_bytes / dt * O.traffic;

				if (r > 1e6) {
					double t = O.cache_mb * 1048576.0 / r * 1e9;

					tau_ns = tau_ns * 0.8 + t * 0.2;
				}
			}
			traffic_bytes = 0; traffic_t0 = now;
			next_tau = now + 5 * MS;
		}
		switch (e.type) {
		case E_SEG:
			if (e.gen == C[e.cpu].gen)
				seg_done(&C[e.cpu]);
			break;
		case E_CAPDL: {
			struct wrk *w = &C[e.cpu].w[e.gen];

			if (w->st == W_LOW)
				wake(&C[e.cpu], (int)e.gen);
			break;
		}
		case E_TIMER: {
			struct cpu *c = &C[e.cpu];
			bool pending = false;

			for (struct task *t = c->u.h; t != NULL; t = t->next)
				if (t->kind == T_TIMER) pending = true;
			if (pending) {
				if (sensors_on) st.skipped_timer++;
			} else {
				ext_add(c, T_TIMER, NULL, 3 * US, now);
			}
			ev_push(now + 1 * MS, E_TIMER, e.cpu, 0, NULL);
			break;
		}
		case E_PING: {
			struct bio *b = calloc(1, sizeof(*b));

			b->prov = (int)e.gen; b->net = true; b->stage = 0; b->t_ping = now;
			net_enqueue(b, 0, e.cpu);
			ev_push(now + 1 * MS, E_PING, e.cpu, e.gen, NULL);
			break;
		}
		case E_FLOOD:
			net_enqueue(e.p, 0, e.cpu);
			break;
		case E_NOISE:
			ext_add(&C[e.cpu], T_NOISE, NULL, 3 * US, 0);
			ev_push(now + SEC / (uint64_t)O.noise_hz / 2 + rnd() % (SEC / (uint64_t)O.noise_hz),
			    E_NOISE, e.cpu, 0, NULL);
			break;
		case E_END:
			hn = 0;
			break;
		}
	}
	for (int i = 0; i < O.ncpu; i++)
		part_touch(&C[i]);
	{			/* the books: every counted partial bio is on a list or in a batch */
		int counted = 0, found = 0;

		for (int i = 0; i < O.ncpu; i++) {
			counted += C[i].partials;
			if (C[i].partials < 0)
				ks_env_fail("cpu %d: %d partial bios", i, C[i].partials);
			for (struct bio *b = C[i].w[BULK].batch; b != NULL; b = b->next)
				found += b->cuts > 0 && b->done < b->nsec;
			for (int q = 0; q < nq; q++)
				for (struct bio *b = Q[q]->pc[i]->head; b != NULL; b = b->next)
					found += b->cuts > 0 && b->done < b->nsec;
		}
		if (counted != found)
			ks_env_fail("partial bios: %d counted, %d found", counted, found);
	}
	{
		double dur = O.secs, area = 0, ov = 0, it = 0;
		uint64_t over = 0, cuts = 0, passes = 0;

		for (int i = 0; i < O.ncpu; i++) {
			area += C[i].part_area;
			cuts += C[i].w[BULK].kw.kw_cap_demotions;
		}
		for (int q = 0; q < nq; q++)
			for (int c = 0; c < O.ncpu; c++) {
				over += Q[q]->pc[c]->kc.kc_overruns;
				passes += Q[q]->pc[c]->kc.kc_passes;
			}
		(void)ov; (void)it;
		printf("mbps=%.1f bios_s=%.1f cuts_bio=%.3f cut_cap=%" PRIu64 " cut_net=%" PRIu64
		    " cut_budget=%" PRIu64 " partials_mean=%.3f partials_p99=%.0f partials_max=%" PRIu64
		    " starts=%" PRIu64 " starts_over=%" PRIu64 " gap_p50_us=%.0f gap_p99_us=%.0f gap_max_us=%.0f"
		    " gap_mean_us=%.0f crypto_p50_us=%.0f crypto_p99_us=%.0f e2e_p50_us=%.0f e2e_p99_us=%.0f"
		    " late_p50_us=%.0f late_p99_us=%.0f late_max_us=%.0f netwait_p99_us=%.0f netwait_max_us=%.0f"
		    " rtt_p50_us=%.0f rtt_p99_us=%.0f rtt_max_us=%.0f net_mitems_s=%.3f small_p50_us=%.0f"
		    " small_p99_us=%.0f small_ops_s=%.0f jain=%.4f overruns=%" PRIu64 " passes=%" PRIu64
		    " steals=%" PRIu64 " stolen_part=%" PRIu64 " declined=%" PRIu64 " demotions=%" PRIu64
		    " bulk_cpu=%.4f net_cpu=%.4f ext_cpu=%.4f tau_ms=%.2f cpu_ms_mib=%.3f\n",
		    (double)st.bytes / dur / 1e6, (double)st.bios / dur,
		    st.crypto.n ? (double)st.cuts / (double)st.crypto.n : 0, st.cut_cap, st.cut_net,
		    st.cut_budget, area / (dur * 1e9 * O.ncpu), hpct(&st.partials, 0.99), st.partials.max,
		    st.starts, st.starts_over, hpct(&st.gap, .5) / 1e3, hpct(&st.gap, .99) / 1e3,
		    (double)st.gap.max / 1e3, st.gap.n ? st.gap.sum / (double)st.gap.n / 1e3 : 0,
		    hpct(&st.crypto, .5) / 1e3, hpct(&st.crypto, .99) / 1e3, hpct(&st.e2e, .5) / 1e3,
		    hpct(&st.e2e, .99) / 1e3, hpct(&st.late, .5) / 1e3, hpct(&st.late, .99) / 1e3,
		    (double)st.late.max / 1e3, hpct(&st.netwait, .99) / 1e3, (double)st.netwait.max / 1e3,
		    hpct(&st.rtt, .5) / 1e3, hpct(&st.rtt, .99) / 1e3, (double)st.rtt.max / 1e3,
		    (double)st.netitems / dur / 1e6, hpct(&st.small, .5) / 1e3, hpct(&st.small, .99) / 1e3,
		    (double)st.small.n / dur, jain(prov_bytes, O.nprov), over, passes, st.steals,
		    st.stolen_part, st.declined, cuts, (double)st.ns_bulk / (dur * 1e9 * O.ncpu),
		    (double)st.ns_net / (dur * 1e9 * O.ncpu), (double)st.ns_ext / (dur * 1e9 * O.ncpu),
		    tau_ns / 1e6, st.bytes ? (double)(st.ns_bulk + st.ns_ext) / 1e6 / ((double)st.bytes / (double)MIB) : 0);
	}
	return (0);
}
