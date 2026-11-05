/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq placement by load gossip on a torus of cores: ../SPEC-p6g.md S2,
 * the code the kernel and the simulator share (kwq_place.h).
 */

#include "kwq_place.h"

void
kp_params_default(struct kp_params *p)
{
	p->imin = KP_US2T(1000);
	p->imax = KP_US2T(8000);
	p->texp = KP_US2T(32000);
	p->hold = KP_US2T(100);
	p->h_us = 2000;
	p->l_us = 500;
	p->ladv_us = 500;
	p->llocal_us = 50;
	p->idle_us = 8;
	p->csmall_ns = 20000;
	p->dnear = 1;
	p->table_n = 4;
	p->checks = 2;
	p->flags = 0;
}

/*
 * The load figure (S2.3): the active lists' backlog in us of work
 * (each queue counted for at most one quantum, the caller's sum), over
 * BULK's share while it had work, plus the share of the CPU other
 * threads took, counted as up to one quantum.
 */
u_int
kp_load_us(uint64_t backlog_ns, u_int share1024, u_int busy1024,
    u_int quantum_us)
{
	uint64_t us;

	if (share1024 < 64)
		share1024 = 64;
	if (share1024 > 1024)
		share1024 = 1024;
	if (busy1024 > 1024)
		busy1024 = 1024;
	us = backlog_ns / 1000 * 1024 / share1024 +
	    (uint64_t)busy1024 * quantum_us / 1024;
	return (us > KP_LOAD_MAX ? KP_LOAD_MAX : (u_int)us);
}

/*
 * Buckets: 0 below idle_us, then four per octave.  The status word is
 * written when the bucket changes; within a bucket the published value
 * is at most a fifth off.
 */
u_int
kp_bucket(u_int us, const struct kp_params *p)
{
	/* 2^(k/4) in 1/1024 */
	static const u_int q[4] = { 1024, 1218, 1448, 1722 };
	uint64_t base;
	u_int b, k;

	if (us < p->idle_us)
		return (0);
	b = 1;
	base = p->idle_us;
	while ((uint64_t)us >= base * 2) {
		base *= 2;
		b += 4;
	}
	for (k = 3; k > 0; k--)
		if ((uint64_t)us * 1024 >= base * q[k])
			break;
	return (b + k);
}

/*
 * A new load figure: true, with the word to store in *word, when the
 * status word must be written.  A higher bucket is published at once, a
 * lower one once it has held for the hold time (S2.3); the caller that
 * sleeps with a fall pending sleeps for kp_load_hold_left() and calls
 * again when it wakes.
 */
bool
kp_load_update(struct kp_load *kl, kp_time_t now, u_int load_us,
    const struct kp_params *p, uint64_t *word)
{
	u_int bnew, bpub;

	if (load_us > KP_LOAD_MAX)
		load_us = KP_LOAD_MAX;
	kl->kl_cur = load_us;
	if (!kl->kl_valid) {
		kl->kl_valid = true;
		goto publish;
	}
	bnew = kp_bucket(load_us, p);
	bpub = kp_bucket(kl->kl_pub, p);
	if (bnew == bpub) {
		kl->kl_pending = false;
		return (false);
	}
	if (bnew > bpub || (p->flags & KP_F_SYMHYST) != 0)
		goto publish;
	if (!kl->kl_pending) {
		kl->kl_pending = true;
		kl->kl_since = now;
		if (p->hold != 0)
			return (false);
	}
	if (!kp_older(now, kl->kl_since, p->hold) &&
	    kp_tdiff(now, kl->kl_since) < (int32_t)p->hold)
		return (false);
publish:
	kl->kl_pending = false;
	kl->kl_pub = load_us;
	*word = kp_st_make(now, load_us);
	return (true);
}

/* Time units until a pending fall may be published, 0 if none is pending. */
kp_time_t
kp_load_hold_left(const struct kp_load *kl, kp_time_t now,
    const struct kp_params *p)
{
	int32_t left;

	if (!kl->kl_pending)
		return (0);
	if (kp_older(now, kl->kl_since, p->hold))
		return (1);
	left = (int32_t)p->hold - kp_tdiff(now, kl->kl_since);
	return (left > 0 ? (kp_time_t)left : 1);
}

/*
 * Topology distance between cores: 0 the same core, 1 the same L3, 2
 * the same package, 3 farther.
 */
u_int
kp_dist(const struct kp_topo *t, u_int a, u_int b)
{
	const struct kp_node *na = &t->kt_node[a], *nb = &t->kt_node[b];

	if (a == b)
		return (0);
	if (na->kn_l3 == nb->kn_l3)
		return (1);
	if (na->kn_pkg == nb->kn_pkg)
		return (2);
	return (3);
}

static void
kp_nb_add(struct kp_node *n, u_int self, u_int c)
{
	u_int i;

	if (c == self || n->kn_nnb >= KP_NB_MAX)
		return;
	for (i = 0; i < n->kn_nnb; i++)
		if (n->kn_nb[i] == c)
			return;
	n->kn_nb[n->kn_nnb++] = (uint16_t)c;
}

/* Node at (row, col) of the R x C layout, or -1 past the partial last row. */
static int
kp_at(const struct kp_topo *t, int r, int c)
{
	u_int i = (u_int)r * t->kt_cols + (u_int)c;

	return (i < t->kt_ncores ? (int)i : -1);
}

/* Rows that column c has: all of them, or one less past the last row's end. */
static int
kp_col_rows(const struct kp_topo *t, int c)
{
	return (kp_at(t, (int)t->kt_rows - 1, c) >= 0 ? (int)t->kt_rows :
	    (int)t->kt_rows - 1);
}

/* Columns that row r has. */
static int
kp_row_cols(const struct kp_topo *t, int r)
{
	int n = (int)t->kt_ncores - r * (int)t->kt_cols;

	return (n > (int)t->kt_cols ? (int)t->kt_cols : n);
}

/*
 * The torus (S2.1): R = floor(sqrt(n)) rows of C = ceil(n / R) cores in
 * topology order, row-major; a row wraps within the cores it has, a
 * column within the rows it has, so a partial last row leaves no core
 * without its four.  Duplicates collapse (tiny machines have fewer).
 * KP_TORUS8 adds the diagonals, KP_EXPANDER replaces the torus by four
 * random neighbours per core made symmetric (ablations).  The nodes'
 * kn_cpu, kn_nthr, kn_l3 and kn_pkg are the caller's, set before.
 */
int
kp_torus_build(struct kp_topo *t, u_int shape, uint32_t seed)
{
	struct kp_node *n;
	u_int i, k, j;
	int r, c, nr, nc, dr, dc, o;
	uint32_t rng = seed != 0 ? seed : 1;

	if (t->kt_ncores == 0 || t->kt_ncores > 0x3fff)
		return (-1);
	for (r = 1; (u_int)((r + 1) * (r + 1)) <= t->kt_ncores; r++)
		;
	t->kt_rows = (u_int)r;
	t->kt_cols = (t->kt_ncores + t->kt_rows - 1) / t->kt_rows;
	for (i = 0; i < t->kt_ncores; i++) {
		n = &t->kt_node[i];
		n->kn_nnb = 0;
		n->kn_row = (uint16_t)(i / t->kt_cols);
		n->kn_col = (uint16_t)(i % t->kt_cols);
	}
	if (shape == KP_EXPANDER) {
		/* Four random partners each, added both ways: degree 4 to 8. */
		for (i = 0; i < t->kt_ncores && t->kt_ncores > 1; i++) {
			for (k = 0, j = 0; k < 2 && j < 64; j++) {
				o = (int)(kp_rand(&rng) % t->kt_ncores);
				if ((u_int)o == i ||
				    t->kt_node[i].kn_nnb >= KP_NB_MAX - 1 ||
				    t->kt_node[o].kn_nnb >= KP_NB_MAX - 1)
					continue;
				kp_nb_add(&t->kt_node[i], i, (u_int)o);
				kp_nb_add(&t->kt_node[o], (u_int)o, i);
				k++;
			}
		}
		return (0);
	}
	for (i = 0; i < t->kt_ncores; i++) {
		n = &t->kt_node[i];
		r = n->kn_row;
		c = n->kn_col;
		nc = kp_row_cols(t, r);
		kp_nb_add(n, i, (u_int)kp_at(t, r, (c + 1) % nc));
		kp_nb_add(n, i, (u_int)kp_at(t, r, (c - 1 + nc) % nc));
		nr = kp_col_rows(t, c);
		kp_nb_add(n, i, (u_int)kp_at(t, (r + 1) % nr, c));
		kp_nb_add(n, i, (u_int)kp_at(t, (r - 1 + nr) % nr, c));
		if (shape != KP_TORUS8)
			continue;
		for (dr = -1; dr <= 1; dr += 2)
			for (dc = -1; dc <= 1; dc += 2) {
				int rr = (r + dr + nr) % nr, cc;

				cc = (c + dc + kp_row_cols(t, rr)) %
				    kp_row_cols(t, rr);
				kp_nb_add(n, i, (u_int)kp_at(t, rr, cc));
			}
	}
	return (0);
}

/*
 * A core as seen in its status block: idle threads (load below idle_us)
 * and the least loaded thread's load.  One line read.  A thread whose
 * word was never written is skipped.
 */
static void
kp_core_view(struct kp_topo *t, u_int core, const struct kp_params *p,
    u_int *idle, u_int *load, int *cpu, uint32_t *rng)
{
	const struct kp_node *n = &t->kt_node[core];
	struct kp_status *st = &t->kt_core[core]->kc_st;
	uint64_t w;
	u_int i, l, ni = 0, best = KP_LOAD_MAX + 1, ties = 0;
	int bc = -1;

	for (i = 0; i < n->kn_nthr; i++) {
		w = KP_LOAD64(&st->ks_w[i]);
		if (!kp_st_valid(w))
			continue;
		l = kp_st_load(w);
		if (l < p->idle_us) {
			ni++;
			l = 0;
		}
		if (l < best) {
			best = l;
			bc = n->kn_cpu[i];
			ties = 1;
		} else if (l == best && rng != NULL &&
		    (p->flags & KP_F_NORAND) == 0 &&
		    kp_rand(rng) % ++ties == 0)
			bc = n->kn_cpu[i];
	}
	*idle = ni;
	*load = best > KP_LOAD_MAX ? KP_LOAD_MAX : best;
	if (cpu != NULL)
		*cpu = bc;
}

/* Age of a stamp, 0 for one from the future (skew). */
static inline int32_t
kp_age(kp_time_t now, kp_time_t t)
{
	int32_t d = kp_tdiff(now, t);

	return (d > 0 ? d : 0);
}

/* A candidate during a merge or a decision. */
struct kp_cand {
	uint64_t	e;
	u_int		dist;
};

/*
 * Ranking (S2.4 step 4): more idle threads, then lower load, then
 * nearer, then fresher.  Negative if a ranks before b.  Under
 * KP_F_NOSMT the idle count only says "some".
 */
static int
kp_rank_cmp(const struct kp_cand *a, const struct kp_cand *b,
    const struct kp_params *p)
{
	u_int ia = kp_e_idle(a->e), ib = kp_e_idle(b->e);

	if ((p->flags & KP_F_NOSMT) != 0) {
		ia = ia != 0;
		ib = ib != 0;
	}
	if (ia != ib)
		return (ia > ib ? -1 : 1);
	if (kp_e_load(a->e) != kp_e_load(b->e))
		return (kp_e_load(a->e) < kp_e_load(b->e) ? -1 : 1);
	if (a->dist != b->dist)
		return (a->dist < b->dist ? -1 : 1);
	if (kp_e_time(a->e) != kp_e_time(b->e))
		return (kp_tdiff(kp_e_time(a->e), kp_e_time(b->e)) > 0 ? -1 : 1);
	return (0);
}

/* Same rank for the random choice: idle count, load bucket, distance. */
static bool
kp_rank_tie(const struct kp_cand *a, const struct kp_cand *b,
    const struct kp_params *p)
{
	return (kp_e_idle(a->e) == kp_e_idle(b->e) &&
	    kp_bucket(kp_e_load(a->e), p) == kp_bucket(kp_e_load(b->e), p) &&
	    a->dist == b->dist);
}

static void
kp_sort(struct kp_cand *c, u_int n, const struct kp_params *p)
{
	struct kp_cand x;
	u_int i, j;

	for (i = 1; i < n; i++) {
		x = c[i];
		for (j = i; j > 0 && kp_rank_cmp(&x, &c[j - 1], p) < 0; j--)
			c[j] = c[j - 1];
		c[j] = x;
	}
}

bool
kp_merge_due(struct kp_topo *t, u_int core, kp_time_t now)
{
	struct kp_ctl *x = &t->kt_core[core]->kc_ctl;

	/* never merged, or the last merge wrapped out of sight: due */
	return (x->kx_merges == 0 || kp_tdiff(now, x->kx_last) < 0 ||
	    kp_tdiff(now, x->kx_last) >= (int32_t)x->kx_ival);
}

#define	KP_MCAND	(KP_NB_MAX * (KP_TABLE_MAX + 1) + KP_TABLE_MAX)

/* Tombstones kept after the table_n entries: as many, if they fit. */
static u_int
kp_ntomb(const struct kp_params *p, u_int tn)
{
	return ((p->flags & KP_F_TOMB) != 0 && 2 * tn <= KP_TABLE_MAX ? tn : 0);
}

/*
 * Write keep[0..nk) into the own table's slots [lo, hi), slot-stable: a
 * kept core stays in its slot, an unchanged entry is renewed only when
 * older than texp / 4.  True if the set or an entry's content changed.
 */
static bool
kp_slots(struct kp_tableblk *own, const uint64_t *old, u_int lo, u_int hi,
    const struct kp_cand *keep, u_int nk, kp_time_t now,
    const struct kp_params *p, u_int *writes)
{
	bool changed = false, used[KP_TABLE_MAX], placed[KP_TABLE_MAX];
	uint64_t ne;
	u_int i, j;

	for (j = 0; j < KP_TABLE_MAX; j++)
		used[j] = false;
	for (i = 0; i < nk; i++) {
		placed[i] = false;
		for (j = lo; j < hi; j++)
			if (!used[j] && kp_e_valid(old[j]) &&
			    kp_e_core(old[j]) == kp_e_core(keep[i].e)) {
				used[j] = placed[i] = true;
				ne = keep[i].e;
				if ((ne & ~0xffffffffULL) != (old[j] & ~0xffffffffULL) ||
				    kp_older(now, kp_e_time(old[j]), p->texp / 4)) {
					if ((ne & ~0xffffffffULL) !=
					    (old[j] & ~0xffffffffULL))
						changed = true;
					KP_STORE64(&own->kt_e[j], ne);
					(*writes)++;
				}
				break;
			}
	}
	for (i = 0; i < nk; i++) {
		if (placed[i])
			continue;
		for (j = lo; j < hi && used[j]; j++)
			;
		KP_ASSERT(j < hi, ("kp_merge: no free slot"));
		used[j] = true;
		KP_STORE64(&own->kt_e[j], keep[i].e);
		(*writes)++;
		changed = true;
	}
	for (j = lo; j < hi; j++)
		if (!used[j] && old[j] != 0) {
			KP_STORE64(&own->kt_e[j], 0);
			(*writes)++;
			changed = true;
		}
	return (changed);
}

/*
 * The merge (S2.4) on `core', whose try-flag the caller holds: the
 * neighbours' status as direct observations stamped now, their tables
 * as relays one hop farther with the stamp copied, the own table; per
 * core the freshest; nothing older than texp, nothing about this core.
 * Cores at or below ladv: the best table_n, ranked.  Cores above it:
 * the freshest as tombstones in the slots after them, relayed like the
 * rest, so that the news of a core turning busy travels as far as the
 * news of it being idle did and a stale relay cannot bring it back
 * (the simulator's island case lost 84 % of its checks to such
 * entries).  Slot-stable writes, only where a slot changed.  True if the
 * table changed.  The interval is reset to imin when it did, doubled up
 * to imax when it did not.
 */
bool
kp_merge(struct kp_topo *t, u_int core, kp_time_t now,
    const struct kp_params *p, struct kp_mres *r)
{
	struct kp_cand c[KP_MCAND], busy[KP_MCAND];
	const struct kp_node *n = &t->kt_node[core];
	struct kp_tableblk *own = &t->kt_core[core]->kc_tb;
	struct kp_ctl *x = &t->kt_core[core]->kc_ctl;
	uint64_t old[KP_TABLE_MAX], e, ne;
	u_int i, j, k, nb_, nc = 0, idle, load, nb, tn, nt, w = 0;
	bool changed;

	tn = p->table_n > KP_TABLE_MAX ? KP_TABLE_MAX : p->table_n;
	nt = kp_ntomb(p, tn);
	r->reads = r->writes = r->cand = 0;
	for (i = 0; i < n->kn_nnb; i++) {
		nb = n->kn_nb[i];
		kp_core_view(t, nb, p, &idle, &load, NULL, NULL);
		r->reads++;
		c[nc].e = kp_e_make(now, nb, load, idle, 1);
		c[nc++].dist = kp_dist(t, core, nb);
		for (j = 0; j < KP_TABLE_MAX; j++) {
			e = KP_LOAD64(&t->kt_core[nb]->kc_tb.kt_e[j]);
			if (!kp_e_valid(e) || kp_e_hops(e) >= KP_HOPS_MAX)
				continue;
			c[nc].e = kp_e_make(kp_e_time(e), kp_e_core(e),
			    kp_e_load(e), kp_e_idle(e), kp_e_hops(e) + 1);
			c[nc++].dist = kp_dist(t, core, kp_e_core(e));
		}
		r->reads++;
	}
	for (j = 0; j < KP_TABLE_MAX; j++) {
		old[j] = e = KP_LOAD64(&own->kt_e[j]);
		if (kp_e_valid(e) && j < tn + nt) {
			c[nc].e = e;
			c[nc++].dist = kp_dist(t, core, kp_e_core(e));
		}
	}
	r->cand = nc;

	/* Filter: the freshest per core (ties: fewer hops), then the rules. */
	for (i = 0; i < nc; i++) {
		e = c[i].e;
		if (e == 0)
			continue;
		if (kp_e_core(e) == core || kp_older(now, kp_e_time(e), p->texp)) {
			c[i].e = 0;
			continue;
		}
		for (j = i + 1; j < nc; j++) {
			ne = c[j].e;
			if (ne == 0 || kp_e_core(ne) != kp_e_core(e))
				continue;
			if (kp_tdiff(kp_e_time(ne), kp_e_time(e)) > 0 ||
			    (kp_e_time(ne) == kp_e_time(e) &&
			    kp_e_hops(ne) < kp_e_hops(e))) {
				c[i].e = 0;
				break;
			}
			c[j].e = 0;
		}
	}
	for (i = k = nb_ = 0; i < nc; i++) {
		if (c[i].e == 0)
			continue;
		if (kp_e_load(c[i].e) <= p->ladv_us)
			c[k++] = c[i];
		else if (nt > 0) {
			/* a tombstone says only "busy at t": constant content */
			busy[nb_] = c[i];
			busy[nb_++].e = kp_e_make(kp_e_time(c[i].e),
			    kp_e_core(c[i].e), KP_E_LOAD_MAX, 0, kp_e_hops(c[i].e));
		}
	}
	kp_sort(c, k, p);
	/* tombstones: the freshest first */
	for (i = 1; i < nb_; i++) {
		struct kp_cand y = busy[i];

		for (j = i; j > 0 && kp_tdiff(kp_e_time(y.e),
		    kp_e_time(busy[j - 1].e)) > 0; j--)
			busy[j] = busy[j - 1];
		busy[j] = y;
	}
	changed = kp_slots(own, old, 0, tn, c, k < tn ? k : tn, now, p, &w);
	if (nt > 0)
		changed |= kp_slots(own, old, tn, tn + nt, busy,
		    nb_ < nt ? nb_ : nt, now, p, &w);
	for (j = tn + nt; j < KP_TABLE_MAX; j++)
		if (old[j] != 0) {
			KP_STORE64(&own->kt_e[j], 0);
			w++;
		}
	r->writes = w;

	x->kx_last = now;
	x->kx_merges++;
	x->kx_writes += r->writes;
	if (changed || (p->flags & KP_F_FIXIVAL) != 0) {
		x->kx_changed += changed;
		x->kx_ival = p->imin;
		x->kx_quiet = 0;
	} else {
		x->kx_quiet++;
		x->kx_ival = x->kx_ival * 2 > p->imax ? p->imax : x->kx_ival * 2;
		if (x->kx_ival < p->imin)
			x->kx_ival = p->imin;
	}
	r->changed = changed;
	return (changed);
}

/*
 * A check that failed: write what was seen into the own table, so every
 * queue placing from this core stops trusting the entry (S2.5 step 4).
 * Under the try-flag; skipped if a merge holds it.
 */
static void
kp_repair(struct kp_topo *t, u_int me, u_int core, kp_time_t now,
    u_int idle, u_int load, const struct kp_params *p)
{
	struct kp_core *kc = t->kt_core[me];
	uint64_t e;
	u_int j;

	u_int tn = p->table_n > KP_TABLE_MAX ? KP_TABLE_MAX : p->table_n;
	u_int nt = kp_ntomb(p, tn), k, oldest = KP_TABLE_MAX;
	uint64_t te;

	if ((p->flags & KP_F_NOREPAIR) != 0 || !KP_TRYFLAG(&kc->kc_ctl))
		return;
	for (j = 0; j < tn; j++) {
		e = KP_LOAD64(&kc->kc_tb.kt_e[j]);
		if (!kp_e_valid(e) || kp_e_core(e) != core)
			continue;
		KP_STORE64(&kc->kc_tb.kt_e[j], load > p->ladv_us ? 0 :
		    kp_e_make(now, core, load, idle, 1));
		/* and a tombstone, in an empty or the oldest slot */
		for (k = tn; load > p->ladv_us && k < tn + nt; k++) {
			te = KP_LOAD64(&kc->kc_tb.kt_e[k]);
			if (!kp_e_valid(te) || kp_e_core(te) == core) {
				oldest = k;
				break;
			}
			if (oldest == KP_TABLE_MAX || kp_tdiff(kp_e_time(
			    KP_LOAD64(&kc->kc_tb.kt_e[oldest])), kp_e_time(te)) > 0)
				oldest = k;
		}
		if (oldest < KP_TABLE_MAX)
			KP_STORE64(&kc->kc_tb.kt_e[oldest],
			    kp_e_make(now, core, KP_E_LOAD_MAX, 0, 1));
		kc->kc_ctl.kx_repairs++;
		/*
		 * An inconsistency: Trickle's reset.  The next merge comes
		 * after imin / 4 (with KP_F_REMERGE at once: a merge storm in
		 * the simulator's island case, 33000 merges/s per core).
		 */
		if ((p->flags & KP_F_REMERGE) != 0)
			kc->kc_ctl.kx_ival = 0;
		else if ((p->flags & KP_F_NOREMERGE) == 0 &&
		    kc->kc_ctl.kx_ival > p->imin / 4)
			kc->kc_ctl.kx_ival = p->imin / 4;
		break;
	}
	KP_CLRFLAG(&kc->kc_ctl);
}

/* The own table's usable entries, ranked; returns how many. */
static u_int
kp_table_read(struct kp_topo *t, u_int me, kp_time_t now,
    const struct kp_params *p, struct kp_cand *c, uint32_t *rng)
{
	struct kp_tableblk *tb = &t->kt_core[me]->kc_tb;
	struct kp_cand x;
	uint64_t e;
	u_int j, n = 0, tn, i, ties;

	tn = p->table_n > KP_TABLE_MAX ? KP_TABLE_MAX : p->table_n;
	for (j = 0; j < tn; j++) {
		e = KP_LOAD64(&tb->kt_e[j]);
		if (!kp_e_valid(e) || kp_e_core(e) == me ||
		    kp_e_core(e) >= t->kt_ncores ||
		    kp_older(now, kp_e_time(e), p->texp))
			continue;
		c[n].e = e;
		c[n++].dist = kp_dist(t, me, kp_e_core(e));
	}
	kp_sort(c, n, p);
	/* A random one of the leading ties goes first. */
	if (n > 1 && rng != NULL && (p->flags & KP_F_NORAND) == 0) {
		for (ties = 1; ties < n && kp_rank_tie(&c[0], &c[ties], p); ties++)
			;
		if (ties > 1) {
			i = kp_rand(rng) % ties;
			x = c[0];
			c[0] = c[i];
			c[i] = x;
		}
	}
	return (n);
}

/*
 * Check a candidate core: read its status block (one remote line) and
 * take its least loaded thread if that thread's load is at most `max'
 * and the caller accepts it.  On a miss the own table is repaired.
 */
static int
kp_check(struct kp_topo *t, u_int me, const struct kp_cand *cd, kp_time_t now,
    u_int max, const struct kp_params *p, uint32_t *rng, kp_ok_t *ok,
    void *arg, struct kp_pick *pk)
{
	u_int core = kp_e_core(cd->e), idle, load;
	int cpu;

	if ((p->flags & KP_F_NOCHECK) != 0) {
		/* Blind: the entry's word for it, any thread of the core. */
		cpu = t->kt_node[core].kn_cpu[kp_rand(rng) %
		    t->kt_node[core].kn_nthr];
		return (ok == NULL || ok(arg, cpu) ? cpu : -1);
	}
	pk->checks++;
	pk->reads++;
	kp_core_view(t, core, p, &idle, &load, &cpu, rng);
	if (cpu >= 0 && load <= max && (ok == NULL || ok(arg, cpu)))
		return (cpu);
	pk->fails++;
	kp_repair(t, me, core, now, idle, load, p);
	return (-1);
}

/* A table entry for a core all of whose threads are idle, within dnear. */
static bool
kp_idle_core_near(struct kp_topo *t, const struct kp_cand *c, u_int n,
    const struct kp_params *p)
{
	u_int i, nthr;

	for (i = 0; i < n; i++) {
		nthr = t->kt_node[kp_e_core(c[i].e)].kn_nthr;
		if (kp_e_idle(c[i].e) >= (nthr < 3 ? nthr : 3) &&
		    c[i].dist <= p->dnear)
			return (true);
	}
	return (false);
}

/*
 * The own core's threads other than `me': an idle one (the least loaded,
 * random among equals), -1 if none.  With rng NULL the first one.
 */
static int
kp_sibling(struct kp_topo *t, u_int me_core, int me, const struct kp_params *p,
    uint32_t *rng, kp_ok_t *ok, void *arg)
{
	const struct kp_node *n = &t->kt_node[me_core];
	struct kp_status *st = &t->kt_core[me_core]->kc_st;
	uint64_t w;
	u_int i, ties = 0;
	int best = -1;

	for (i = 0; i < n->kn_nthr; i++) {
		if (n->kn_cpu[i] == me)
			continue;
		w = KP_LOAD64(&st->ks_w[i]);
		if (!kp_st_valid(w) || kp_st_load(w) >= p->idle_us ||
		    (ok != NULL && !ok(arg, n->kn_cpu[i])))
			continue;
		if (best < 0 || (rng != NULL && (p->flags & KP_F_NORAND) == 0 &&
		    kp_rand(rng) % (ties + 1) == 0))
			best = n->kn_cpu[i];
		ties++;
	}
	return (best);
}

/*
 * Placement of a KWQ_CPU_ANY item (S2.5) by a producer on CPU `me'.
 * `cost_ns' is the queue's cost per item on this CPU (the size split).
 * `self_us' is the load of CPU me's BULK worker when that worker is the
 * producer (a handler resubmitting), its backlog without this item; -1
 * for any other producer.  `ok' says whether the queue's list on a CPU
 * is below its high mark.  Returns the CPU; *pk says how it was found.
 */
int
kp_place(struct kp_topo *t, int me, kp_time_t now, uint64_t cost_ns,
    int self_us, const struct kp_params *p, uint32_t *rng, kp_ok_t *ok,
    void *arg, struct kp_pick *pk)
{
	struct kp_cand c[KP_TABLE_MAX];
	u_int me_core, n, i, steps[5], ns, s, idle, load, best, nb, checks;
	const struct kp_node *node;
	uint64_t w;
	bool tried[KP_TABLE_MAX];
	int cpu, bc;

	pk->cpu = -1;
	pk->step = 0;
	pk->reads = pk->checks = pk->fails = 0;
	pk->age = 0;
	pk->expired = false;
	me_core = (u_int)t->kt_cpu_core[me];
	node = &t->kt_node[me_core];
	n = kp_table_read(t, me_core, now, p, c, rng);
	/*
	 * Step 0: the worker resubmitting goes on with it, warm (P6g.0),
	 * unless it shares its core with a busy sibling while the table
	 * knows a whole idle core near: then a large item moves there, or
	 * two requests would run beside each other at the SMT rate for good.
	 */
	if (self_us >= 0 && (u_int)self_us < p->llocal_us &&
	    (p->flags & KP_F_NOSELF) == 0 && (ok == NULL || ok(arg, me)) &&
	    (cost_ns < p->csmall_ns || node->kn_nthr == 1 ||
	    kp_sibling(t, me_core, me, p, NULL, NULL, NULL) >= 0 ||
	    !kp_idle_core_near(t, c, n, p))) {
		pk->step = KP_STEP_SELF;
		pk->cpu = me;
		return (me);
	}
	for (i = 0; i < n; i++)
		tried[i] = false;
	if (cost_ns < p->csmall_ns && (p->flags & KP_F_NOSPLIT) == 0) {
		steps[0] = KP_STEP_SIBLING;
		steps[1] = KP_STEP_LOCAL;
		steps[2] = KP_STEP_IDLE_NEAR;
	} else {
		steps[0] = KP_STEP_IDLE_NEAR;
		steps[1] = KP_STEP_SIBLING;
		steps[2] = KP_STEP_LOCAL;
	}
	steps[3] = KP_STEP_TABLE;
	steps[4] = KP_STEP_NEIGHBOURS;
	ns = 5;
	checks = (p->flags & KP_F_NOCHECK) != 0 ? KP_TABLE_MAX : p->checks;
	for (s = 0; s < ns; s++) {
		switch (steps[s]) {
		case KP_STEP_IDLE_NEAR:
			for (i = 0; i < n && pk->checks < checks; i++) {
				if (tried[i] || kp_e_idle(c[i].e) == 0 ||
				    c[i].dist > p->dnear)
					continue;
				tried[i] = true;
				cpu = kp_check(t, me_core, &c[i], now,
				    p->idle_us - 1, p, rng, ok, arg, pk);
				if (cpu >= 0) {
					pk->age = (kp_time_t)kp_age(now,
					    kp_e_time(c[i].e));
					goto found;
				}
			}
			break;
		case KP_STEP_SIBLING:
			cpu = kp_sibling(t, me_core, me, p, rng, ok, arg);
			if (cpu >= 0)
				goto found;
			break;
		case KP_STEP_LOCAL:
			w = KP_LOAD64(&t->kt_core[me_core]->kc_st.ks_w[
			    t->kt_cpu_slot[me]]);
			if ((!kp_st_valid(w) || kp_st_load(w) < p->llocal_us) &&
			    (ok == NULL || ok(arg, me))) {
				cpu = me;
				goto found;
			}
			break;
		case KP_STEP_TABLE:
			for (i = 0; i < n && pk->checks < checks; i++) {
				if (tried[i])
					continue;
				tried[i] = true;
				cpu = kp_check(t, me_core, &c[i], now,
				    p->ladv_us, p, rng, ok, arg, pk);
				if (cpu >= 0) {
					pk->age = (kp_time_t)kp_age(now,
					    kp_e_time(c[i].e));
					goto found;
				}
			}
			break;
		case KP_STEP_NEIGHBOURS:
			/* The least loaded thread of the own core and the neighbours. */
			bc = -1;
			best = KP_LOAD_MAX + 1;
			for (i = 0; i <= node->kn_nnb; i++) {
				nb = i < node->kn_nnb ? node->kn_nb[i] : me_core;
				kp_core_view(t, nb, p, &idle, &load, &cpu, rng);
				if (nb != me_core)
					pk->reads++;
				if (cpu < 0 || (ok != NULL && !ok(arg, cpu)))
					continue;
				if (load < best) {
					best = load;
					bc = cpu;
				}
			}
			if (bc >= 0) {
				cpu = bc;
				goto found;
			}
			break;
		}
	}
	pk->step = KP_STEP_FALLBACK;
	pk->cpu = me;
	return (me);
found:
	pk->step = steps[s];
	pk->cpu = cpu;
	if (pk->age > p->texp)
		pk->expired = true;
	return (cpu);
}

/*
 * Spill (S2.6): an item for a fixed CPU of a stealable queue goes where
 * KWQ_CPU_ANY would when that CPU's thread is at or above H.  One read
 * of the CPU's status block.
 */
bool
kp_spill(struct kp_topo *t, int cpu, const struct kp_params *p)
{
	int core = t->kt_cpu_core[cpu];
	uint64_t w;

	if (core < 0)
		return (false);
	w = KP_LOAD64(&t->kt_core[core]->kc_st.ks_w[t->kt_cpu_slot[cpu]]);
	return (kp_st_valid(w) && kp_st_load(w) >= p->h_us);
}

/*
 * Hand-off (S2.7): the target for a worker whose backlog is at or above
 * H: a checked table candidate whose least loaded thread is at or below
 * L, or -1.
 */
int
kp_handoff_target(struct kp_topo *t, int me, kp_time_t now,
    const struct kp_params *p, uint32_t *rng, struct kp_pick *pk)
{
	struct kp_cand c[KP_TABLE_MAX];
	u_int me_core = (u_int)t->kt_cpu_core[me], n, i;
	int cpu;

	pk->cpu = -1;
	pk->step = KP_STEP_TABLE;
	pk->reads = pk->checks = pk->fails = 0;
	pk->age = 0;
	pk->expired = false;
	n = kp_table_read(t, me_core, now, p, c, rng);
	for (i = 0; i < n && pk->checks < p->checks; i++) {
		if (kp_e_load(c[i].e) > p->l_us)
			continue;
		cpu = kp_check(t, me_core, &c[i], now, p->l_us, p, rng, NULL,
		    NULL, pk);
		if (cpu >= 0) {
			pk->cpu = cpu;
			pk->age = (kp_time_t)kp_age(now, kp_e_time(c[i].e));
			pk->expired = pk->age > p->texp;
			return (cpu);
		}
	}
	return (-1);
}

/*
 * Hand-off batch: the older half of n items, at most `max', and small
 * enough that the receiver stays below H - L after it (no ping-pong,
 * S3.6 B4): at most (H - L) / cost - 1 items, at least one.
 */
u_int
kp_handoff_batch(u_int n, uint64_t cost_ns, u_int max,
    const struct kp_params *p)
{
	uint64_t room;
	u_int take = (n + 1) / 2;

	if (take > max)
		take = max;
	if (cost_ns > 0 && p->h_us > p->l_us) {
		room = (uint64_t)(p->h_us - p->l_us) * 1000 / cost_ns;
		room = room > 1 ? room - 1 : 1;
		if (take > room)
			take = (u_int)room;
	}
	return (take > 0 ? take : 1);
}

/* The pass cap's test (S2.7): an idle core within dnear in the own table. */
bool
kp_idle_near(struct kp_topo *t, int me, kp_time_t now,
    const struct kp_params *p)
{
	struct kp_cand c[KP_TABLE_MAX];
	u_int n, i;

	n = kp_table_read(t, (u_int)t->kt_cpu_core[me], now, p, c, NULL);
	for (i = 0; i < n; i++)
		if (kp_e_idle(c[i].e) != 0 && c[i].dist <= p->dnear)
			return (true);
	return (false);
}
