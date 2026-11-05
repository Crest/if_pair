/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kp_test - unit tests of the shared placement code (../../kwq/kwq_place.c,
 * SPEC-p6g.md S3.1): the word encodings, the buckets and the hysteresis,
 * the torus for every topology of S3.5, the merge rules, expiry and the
 * wrap of time, placement and hand-off on small hand-built states.
 * Plain memory stands in for the cache-line model.  Exit status 0 when
 * every check passes.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kwq_place.h"

static int fails, checks;

uint64_t kp_env_load(const uint64_t *p) { return (*p); }
void kp_env_store(uint64_t *p, uint64_t v) { *p = v; }
bool kp_env_tryflag(u_int *f) { if (*f) return (false); *f = 1; return (true); }
void kp_env_clrflag(u_int *f) { *f = 0; }

void
kp_env_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "assert: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	fails++;
}

#define	CHECK(e, ...) do {						\
	checks++;							\
	if (!(e)) {							\
		fails++;						\
		fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #e);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

/* A machine: ncores cores of nthr threads, l3 cores per L3, pkg L3s per package. */
static struct kp_topo *
mk_topo(u_int ncores, u_int nthr, u_int per_l3, u_int l3_per_pkg)
{
	struct kp_topo *t = calloc(1, sizeof(*t));
	u_int i, j;

	t->kt_ncores = ncores;
	t->kt_maxcpu = ncores * nthr;
	t->kt_node = calloc(ncores, sizeof(*t->kt_node));
	t->kt_core = calloc(ncores, sizeof(*t->kt_core));
	t->kt_cpu_core = calloc(t->kt_maxcpu, sizeof(int));
	t->kt_cpu_slot = calloc(t->kt_maxcpu, 1);
	for (i = 0; i < ncores; i++) {
		t->kt_core[i] = aligned_alloc(KP_LINE, sizeof(struct kp_core));
		memset(t->kt_core[i], 0, sizeof(struct kp_core));
		t->kt_node[i].kn_nthr = (uint8_t)nthr;
		t->kt_node[i].kn_l3 = (uint16_t)(i / per_l3);
		t->kt_node[i].kn_pkg = (uint16_t)(i / per_l3 / l3_per_pkg);
		for (j = 0; j < nthr; j++) {
			t->kt_node[i].kn_cpu[j] = (int)(i * nthr + j);
			t->kt_cpu_core[i * nthr + j] = (int)i;
			t->kt_cpu_slot[i * nthr + j] = (uint8_t)j;
		}
	}
	return (t);
}

static void
free_topo(struct kp_topo *t)
{
	u_int i;

	for (i = 0; i < t->kt_ncores; i++)
		free(t->kt_core[i]);
	free(t->kt_core);
	free(t->kt_node);
	free(t->kt_cpu_core);
	free(t->kt_cpu_slot);
	free(t);
}

static void
test_encoding(void)
{
	uint64_t e, w;

	e = kp_e_make(0xdeadbeef, 12345, 999, 2, 7);
	CHECK(kp_e_valid(e) && kp_e_time(e) == 0xdeadbeef && kp_e_core(e) == 12345 &&
	    kp_e_load(e) == 999 && kp_e_idle(e) == 2 && kp_e_hops(e) == 7, "entry");
	e = kp_e_make(1, 0x3fff, 5000, 9, 99);
	CHECK(kp_e_core(e) == 0x3fff && kp_e_load(e) == KP_E_LOAD_MAX &&
	    kp_e_idle(e) == 3 && kp_e_hops(e) == KP_HOPS_MAX, "entry saturation");
	w = kp_st_make(77, 4000);
	CHECK(kp_st_valid(w) && kp_st_time(w) == 77 && kp_st_load(w) == 4000, "status");
	CHECK(kp_st_load(kp_st_make(1, 99999)) == KP_LOAD_MAX, "status saturation");
	CHECK(!kp_st_valid(0) && !kp_e_valid(0), "zero is invalid");
	CHECK(kp_tdiff(5, 0xfffffffb) == 10, "time wrap");
	CHECK(KP_T2US(KP_US2T(1000)) >= 999 && KP_T2US(KP_US2T(1000)) <= 1000, "units");
}

static void
test_buckets(void)
{
	struct kp_params p;
	u_int us, b, prev = 0;

	kp_params_default(&p);
	CHECK(kp_bucket(0, &p) == 0 && kp_bucket(p.idle_us - 1, &p) == 0, "idle");
	CHECK(kp_bucket(p.idle_us, &p) == 1, "first");
	for (us = 0; us <= KP_LOAD_MAX; us++) {
		b = kp_bucket(us, &p);
		CHECK(b >= prev && b <= prev + 1, "monotone at %u: %u after %u", us, b, prev);
		prev = b;
	}
	/* Four per octave: 8 -> 1, 16 -> 5, 32 -> 9. */
	CHECK(kp_bucket(16, &p) == 5 && kp_bucket(32, &p) == 9, "octaves");
	CHECK(kp_load_us(0, 1024, 0, 1000) == 0, "empty");
	CHECK(kp_load_us(1000000, 512, 0, 1000) == 2000, "share halves");
	CHECK(kp_load_us(0, 1024, 512, 1000) == 500, "busy");
	CHECK(kp_load_us(100000000, 1024, 0, 1000) == KP_LOAD_MAX, "saturates");
}

static void
test_hysteresis(void)
{
	struct kp_params p;
	struct kp_load kl;
	uint64_t w = 0;
	kp_time_t t0 = 0xfffff000u;	/* across the wrap */

	kp_params_default(&p);
	memset(&kl, 0, sizeof(kl));
	CHECK(kp_load_update(&kl, t0, 0, &p, &w) && kp_st_load(w) == 0, "first write");
	CHECK(kp_load_update(&kl, t0 + 1, 900, &p, &w) && kp_st_load(w) == 900, "rise at once");
	CHECK(!kp_load_update(&kl, t0 + 2, 920, &p, &w), "same bucket");
	CHECK(!kp_load_update(&kl, t0 + 3, 0, &p, &w), "fall held");
	CHECK(kp_load_hold_left(&kl, t0 + 3, &p) == p.hold, "hold left");
	CHECK(!kp_load_update(&kl, t0 + 3 + p.hold - 1, 0, &p, &w), "still held");
	CHECK(kp_load_update(&kl, t0 + 3 + p.hold, 0, &p, &w) && kp_st_load(w) == 0,
	    "fall after the hold");
	CHECK(kp_load_hold_left(&kl, t0 + 3 + p.hold, &p) == 0, "nothing pending");
	/* A fall interrupted by a rise back to the published bucket is cancelled. */
	kp_load_update(&kl, t0 + 1000, 900, &p, &w);
	CHECK(!kp_load_update(&kl, t0 + 1001, 10, &p, &w), "fall pending");
	CHECK(!kp_load_update(&kl, t0 + 1002, 905, &p, &w) && !kl.kl_pending, "cancelled");
	p.flags |= KP_F_SYMHYST;
	CHECK(kp_load_update(&kl, t0 + 1003, 10, &p, &w), "symmetric: at once");
}

static u_int
bfs_diameter(struct kp_topo *t, bool *connected)
{
	u_int *d = malloc(t->kt_ncores * sizeof(u_int)), *qq = malloc(t->kt_ncores * sizeof(u_int));
	u_int s, i, h, tl, x, max = 0, k;

	*connected = true;
	for (s = 0; s < t->kt_ncores; s++) {
		for (i = 0; i < t->kt_ncores; i++)
			d[i] = ~0u;
		d[s] = 0;
		h = tl = 0;
		qq[tl++] = s;
		while (h < tl) {
			x = qq[h++];
			for (k = 0; k < t->kt_node[x].kn_nnb; k++) {
				i = t->kt_node[x].kn_nb[k];
				if (d[i] == ~0u) {
					d[i] = d[x] + 1;
					qq[tl++] = i;
				}
			}
		}
		for (i = 0; i < t->kt_ncores; i++) {
			if (d[i] == ~0u)
				*connected = false;
			else if (d[i] > max)
				max = d[i];
		}
	}
	free(d);
	free(qq);
	return (max);
}

static void
test_torus(void)
{
	static const u_int sizes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 31, 63, 64, 100,
	    127, 128, 200, 256 };
	struct kp_topo *t;
	u_int i, a, k, j, diam;
	bool conn, sym;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		t = mk_topo(sizes[i], 1, 1u << 30, 1);
		CHECK(kp_torus_build(t, KP_TORUS4, 1) == 0, "build %u", sizes[i]);
		sym = true;
		for (a = 0; a < t->kt_ncores; a++) {
			CHECK(t->kt_node[a].kn_nnb <= 4, "degree %u at %u", sizes[i], a);
			CHECK(sizes[i] < 5 || t->kt_node[a].kn_nnb >= 2,
			    "too few neighbours: n %u core %u has %u", sizes[i], a,
			    t->kt_node[a].kn_nnb);
			for (k = 0; k < t->kt_node[a].kn_nnb; k++) {
				u_int b = t->kt_node[a].kn_nb[k];
				bool back = false;

				CHECK(b != a && b < t->kt_ncores, "self or range");
				for (j = 0; j < t->kt_node[b].kn_nnb; j++)
					back |= t->kt_node[b].kn_nb[j] == a;
				sym &= back;
			}
		}
		CHECK(sym, "symmetric %u", sizes[i]);
		diam = bfs_diameter(t, &conn);
		CHECK(conn, "connected %u", sizes[i]);
		/* R/2 + C/2 hops, one more for a partial row. */
		CHECK(diam <= t->kt_rows / 2 + t->kt_cols / 2 + 1, "diameter %u: %u (%ux%u)",
		    sizes[i], diam, t->kt_rows, t->kt_cols);
		if (sizes[i] == 8 || sizes[i] == 128 || sizes[i] == 4 || sizes[i] == 127)
			printf("torus %3u cores: %u x %u, diameter %u\n", sizes[i],
			    t->kt_rows, t->kt_cols, diam);
		CHECK(kp_torus_build(t, KP_TORUS8, 1) == 0, "build8 %u", sizes[i]);
		for (a = 0; a < t->kt_ncores; a++)
			CHECK(t->kt_node[a].kn_nnb <= 8, "degree8");
		bfs_diameter(t, &conn);
		CHECK(conn, "connected8 %u", sizes[i]);
		CHECK(kp_torus_build(t, KP_EXPANDER, 7) == 0, "expander %u", sizes[i]);
		bfs_diameter(t, &conn);
		CHECK(conn || sizes[i] < 4, "connected expander %u", sizes[i]);
		free_topo(t);
	}
	t = mk_topo(8, 2, 8, 1);	/* lab: 2 x 4 */
	kp_torus_build(t, KP_TORUS4, 1);
	CHECK(t->kt_rows == 2 && t->kt_cols == 4, "lab shape");
	CHECK(kp_dist(t, 0, 0) == 0 && kp_dist(t, 0, 7) == 1, "lab dist");
	free_topo(t);
	t = mk_topo(64, 2, 8, 4);	/* T4: 2 packages x 4 L3 x 8 cores */
	CHECK(kp_dist(t, 0, 7) == 1 && kp_dist(t, 0, 8) == 2 && kp_dist(t, 0, 32) == 3,
	    "T4 dist");
	free_topo(t);
}

static void
set_status(struct kp_topo *t, int cpu, kp_time_t now, u_int load)
{
	t->kt_core[t->kt_cpu_core[cpu]]->kc_st.ks_w[t->kt_cpu_slot[cpu]] =
	    kp_st_make(now, load);
}

static void
test_merge(void)
{
	struct kp_params p;
	struct kp_topo *t;
	struct kp_mres r;
	uint64_t *tb, e;
	kp_time_t now = 0xffffff00u;	/* across the wrap */
	u_int c, j, n;

	kp_params_default(&p);
	t = mk_topo(16, 1, 1u << 30, 1);	/* 4 x 4 */
	kp_torus_build(t, KP_TORUS4, 1);
	for (c = 0; c < 16; c++)
		set_status(t, (int)c, now, 3000);
	/* Core 5 idle; core 0's neighbours are 1, 3, 4, 12. */
	set_status(t, 5, now, 0);
	kp_merge(t, 1, now, &p, &r);	/* 1 sees 5 directly */
	tb = t->kt_core[1]->kc_tb.kt_e;
	CHECK(kp_e_valid(tb[0]) && kp_e_core(tb[0]) == 5 && kp_e_hops(tb[0]) == 1 &&
	    kp_e_idle(tb[0]) == 1, "direct observation");
	CHECK(r.reads == 8, "8 lines: %u", r.reads);
	CHECK(r.changed, "changed");
	kp_merge(t, 0, now + 10, &p, &r);	/* 0 relays from 1 */
	tb = t->kt_core[0]->kc_tb.kt_e;
	for (j = n = 0; j < KP_TABLE_MAX; j++)
		if (kp_e_valid(tb[j]))
			n++, e = tb[j];
	CHECK(n == 1 && kp_e_core(e) == 5 && kp_e_hops(e) == 2 && kp_e_time(e) == now,
	    "relay keeps the stamp: n %u hops %u", n, n ? kp_e_hops(e) : 0);
	/* Unchanged: no write, the interval doubles. */
	kp_merge(t, 0, now + 20, &p, &r);
	CHECK(!r.changed && r.writes == 0, "quiet merge: %u writes", r.writes);
	CHECK(t->kt_core[0]->kc_ctl.kx_ival == 2 * p.imin, "doubled");
	/* Core 5 turns busy: 1 sees it directly and drops it. */
	set_status(t, 5, now + 30, 3000);
	kp_merge(t, 1, now + 30, &p, &r);
	tb = t->kt_core[1]->kc_tb.kt_e;
	for (j = n = 0; j < KP_TABLE_MAX; j++)
		n += kp_e_valid(tb[j]);
	CHECK(n == 0, "fresh direct observation wins: %u left", n);
	/* 0 still holds the old relay; it expires after texp. */
	kp_merge(t, 0, now + 40, &p, &r);
	CHECK(kp_e_valid(t->kt_core[0]->kc_tb.kt_e[0]), "relay not yet expired");
	kp_merge(t, 0, now + p.texp + 1, &p, &r);
	CHECK(!kp_e_valid(t->kt_core[0]->kc_tb.kt_e[0]), "expired");
	CHECK(t->kt_core[0]->kc_ctl.kx_ival == p.imin, "reset to imin on a change");
	/* Never an entry about the own core, and hops stop at the cap. */
	set_status(t, 0, now, 0);
	kp_merge(t, 1, now + p.texp + 2, &p, &r);
	kp_merge(t, 0, now + p.texp + 3, &p, &r);
	tb = t->kt_core[0]->kc_tb.kt_e;
	for (j = 0; j < KP_TABLE_MAX; j++)
		CHECK(!kp_e_valid(tb[j]) || kp_e_core(tb[j]) != 0, "own core");
	/* table_n caps the table; ranking prefers idle, then lower load, then near. */
	for (c = 0; c < 16; c++)
		set_status(t, (int)c, now, c == 0 ? 3000 : 100 + c * 10);
	set_status(t, 4, now, 0);
	memset(t->kt_core[0]->kc_tb.kt_e, 0, sizeof(t->kt_core[0]->kc_tb.kt_e));
	p.table_n = 2;
	kp_merge(t, 0, now + p.texp + 10, &p, &r);
	tb = t->kt_core[0]->kc_tb.kt_e;
	for (j = n = 0; j < KP_TABLE_MAX; j++)
		n += kp_e_valid(tb[j]);
	CHECK(n == 2, "table_n: %u", n);
	CHECK(kp_e_core(tb[0]) == 4 || kp_e_core(tb[1]) == 4, "idle first");
	free_topo(t);
}

/* Entries kp_place() would consider. */
static u_int
kp_table_valid_count(struct kp_topo *t, u_int core, kp_time_t now, const struct kp_params *p)
{
	u_int j, n = 0;
	uint64_t e;

	for (j = 0; j < p->table_n; j++) {
		e = t->kt_core[core]->kc_tb.kt_e[j];
		n += kp_e_valid(e) && !kp_older(now, kp_e_time(e), p->texp);
	}
	return (n);
}

static bool
ok_all(void *arg, int cpu)
{
	return (true);
}

static bool
ok_not(void *arg, int cpu)
{
	return (cpu != *(int *)arg);
}

static void
test_place(void)
{
	struct kp_params p;
	struct kp_topo *t;
	struct kp_mres r;
	struct kp_pick pk;
	uint32_t rng = 12345;
	kp_time_t now = 1000000;
	int cpu, no;
	u_int c;

	kp_params_default(&p);
	t = mk_topo(8, 2, 8, 1);	/* lab */
	kp_torus_build(t, KP_TORUS4, 1);
	for (c = 0; c < 16; c++)
		set_status(t, (int)c, now, 3000);
	/* Core 1 (CPUs 2, 3) fully idle; core 0's neighbours include 1. */
	set_status(t, 2, now, 0);
	set_status(t, 3, now, 0);
	kp_merge(t, 0, now, &p, &r);
	cpu = kp_place(t, 0, now + 5, 1000000, -1, &p, &rng, ok_all, NULL, &pk);
	CHECK((cpu == 2 || cpu == 3) && pk.step == KP_STEP_IDLE_NEAR && pk.checks == 1 &&
	    pk.reads == 1, "idle core: cpu %d step %u", cpu, pk.step);
	/*
	 * Step 0: a resubmitting worker below L_local stays, unless its
	 * sibling is busy and an idle core is near: then a large item moves
	 * and a small one stays.
	 */
	cpu = kp_place(t, 0, now + 5, 1000000, 50, &p, &rng, ok_all, NULL, &pk);
	CHECK((cpu == 2 || cpu == 3) && pk.step == KP_STEP_IDLE_NEAR,
	    "self beside a busy sibling: %d step %u", cpu, pk.step);
	cpu = kp_place(t, 0, now + 5, 3000, 20, &p, &rng, ok_all, NULL, &pk);
	CHECK(cpu == 0 && pk.step == KP_STEP_SELF, "small self: %d", cpu);
	set_status(t, 1, now + 5, 0);
	cpu = kp_place(t, 0, now + 5, 1000000, 20, &p, &rng, ok_all, NULL, &pk);
	CHECK(cpu == 0 && pk.step == KP_STEP_SELF, "self, sibling idle: %d", cpu);
	cpu = kp_place(t, 0, now + 5, 1000000, 500, &p, &rng, ok_all, NULL, &pk);
	CHECK(cpu != 0 || pk.step != KP_STEP_SELF, "self above L_local");
	p.flags |= KP_F_NOSELF;
	cpu = kp_place(t, 0, now + 5, 1000000, 50, &p, &rng, ok_all, NULL, &pk);
	CHECK(pk.step != KP_STEP_SELF, "noself");
	p.flags &= ~KP_F_NOSELF;
	set_status(t, 1, now + 5, 3000);
	/* The idle core got busy since the merge: the check fails and repairs. */
	set_status(t, 2, now + 6, 3000);
	set_status(t, 3, now + 6, 3000);
	cpu = kp_place(t, 0, now + 7, 1000000, -1, &p, &rng, ok_all, NULL, &pk);
	CHECK(pk.fails == 1 && pk.step == KP_STEP_NEIGHBOURS, "failed check: step %u fails %u",
	    pk.step, pk.fails);
	CHECK(!kp_e_valid(t->kt_core[0]->kc_tb.kt_e[0]), "repaired");
	/* Small items: the idle sibling before a remote idle core. */
	set_status(t, 1, now + 8, 0);
	set_status(t, 2, now + 8, 0);
	set_status(t, 3, now + 8, 0);
	kp_merge(t, 0, now + p.imax + 10, &p, &r);
	cpu = kp_place(t, 0, now + p.imax + 11, 3000, -1, &p, &rng, ok_all, NULL, &pk);
	CHECK(cpu == 1 && pk.step == KP_STEP_SIBLING, "small: sibling, got %d", cpu);
	cpu = kp_place(t, 0, now + p.imax + 11, 1000000, -1, &p, &rng, ok_all, NULL, &pk);
	CHECK(cpu == 2 || cpu == 3, "large: idle core, got %d", cpu);
	/* The high mark: a refused target sends the item on. */
	no = 1;
	cpu = kp_place(t, 0, now + p.imax + 11, 3000, -1, &p, &rng, ok_not, &no, &pk);
	CHECK(cpu != 1, "refused sibling");
	/* Expired table: nothing used. */
	cpu = kp_place(t, 0, now + p.imax + 11 + p.texp + 1, 1000000, -1, &p, &rng, ok_all,
	    NULL, &pk);
	CHECK(!pk.expired && pk.step != KP_STEP_IDLE_NEAR && pk.step != KP_STEP_TABLE,
	    "expired table: step %u", pk.step);
	/* Hand-off: a checked candidate at or below L. */
	kp_merge(t, 0, now + 2 * p.texp, &p, &r);
	cpu = kp_handoff_target(t, 0, now + 2 * p.texp + 1, &p, &rng, &pk);
	CHECK(cpu == 2 || cpu == 3, "handoff target %d", cpu);
	CHECK(kp_handoff_batch(100, 300000, 64, &p) == 4, "batch by H - L: %u",
	    kp_handoff_batch(100, 300000, 64, &p));
	CHECK(kp_handoff_batch(100, 3000, 64, &p) == 50, "batch half");
	CHECK(kp_handoff_batch(1000, 3000, 64, &p) == 64, "batch max");
	CHECK(kp_handoff_batch(1, 3000000, 64, &p) == 1, "batch one");
	CHECK(kp_idle_near(t, 0, now + 2 * p.texp + 1, &p), "idle near");
	/* Spill at H. */
	set_status(t, 5, now, p.h_us);
	CHECK(kp_spill(t, 5, &p) && !kp_spill(t, 2, &p), "spill");
	free_topo(t);
}

/* The 32-bit clock wraps every 4096 s: stamps older than 2048 s look new. */
static void
test_wrap(void)
{
	struct kp_params p;
	struct kp_topo *t;
	struct kp_mres r;
	struct kp_load kl;
	uint64_t w;
	kp_time_t t0 = 1000, later = t0 + 0x90000000u;	/* about 2400 s on */

	kp_params_default(&p);
	CHECK(kp_older(later, t0, p.texp), "wrapped stamp is old");
	CHECK(!kp_older(t0, t0 + 10, p.texp), "skew tolerated");
	CHECK(kp_older(t0, t0 + KP_SKEW + 1, p.texp), "far future is old");
	t = mk_topo(4, 1, 4, 1);
	kp_torus_build(t, KP_TORUS4, 1);
	CHECK(kp_merge_due(t, 0, t0), "never merged: due");
	set_status(t, 1, t0, 0);
	kp_merge(t, 0, t0, &p, &r);
	CHECK(kp_e_valid(t->kt_core[0]->kc_tb.kt_e[0]), "entry");
	CHECK(!kp_merge_due(t, 0, t0 + 1), "not due");
	CHECK(kp_merge_due(t, 0, later), "due after the wrap");
	/* the old entry must not be used after the wrap */
	CHECK(kp_table_valid_count(t, 0, later, &p) == 0, "expired across the wrap");
	/* a fall pending across the wrap is published */
	memset(&kl, 0, sizeof(kl));
	kp_load_update(&kl, t0, 900, &p, &w);
	kp_load_update(&kl, t0 + 1, 0, &p, &w);
	CHECK(kp_load_update(&kl, later, 0, &p, &w), "fall published after the wrap");
	free_topo(t);
}

int
main(void)
{
	test_encoding();
	test_buckets();
	test_hysteresis();
	test_torus();
	test_merge();
	test_place();
	test_wrap();
	printf("kp_test: %d checks, %d failed\n", checks, fails);
	return (fails != 0);
}
