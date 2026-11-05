/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The gossip scheme, kern.kwq.bulk.place=1 (../SPEC-p6g.md S2, K1-K7;
 * simulated as policy 5 in ../sim/place/placesim.c).  kwq_place.c holds
 * the scheme's pure functions, shared with the simulator; this file is
 * its kernel side:
 *
 * - the torus of cores from smp_topo(), the per-core blocks (status,
 *   table, control) and kern.kwq.bulk.gossip.torus to dump them;
 * - the load figure and the status word, at the BULK worker's decision
 *   points (pass start and end, sleep, wake), a sleeping worker's fall
 *   published after the hold by a callout;
 * - the merge: the worker at a pass end, a producer before it places
 *   (SPEC-p6g.md K3's merge at a wake after Imax asleep was not built);
 * - placement of KWQ_CPU_ANY (kp_place()) and the spill of a fixed CPU's
 *   items (kp_spill()) for stealable queues;
 * - the hand-off at a pass end in place of stealing (decision D2): no
 *   idle mask, no idle-path steal and no steal wake under place=1.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/counter.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/rmlock.h>
#include <sys/sbuf.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <machine/atomic.h>

#include "kwq_internal.h"
#include "kwq_sched.h"
#include "kwq_sdt.h"

static struct kp_topo kwq_kt;
static struct kp_core *kwq_kblk;
static struct kp_params kwq_kp;
static u_int kwq_handoff_n = 4;		/* hand-offs at most per pass end */
DPCPU_DEFINE_STATIC(uint32_t, kwq_kp_rng);

SYSCTL_DECL(_kern_kwq_bulk);
static struct sysctl_ctx_list kwq_gossip_ctx;
static counter_u64_t kwq_gp_step[KP_STEP_FALLBACK + 1], kwq_gp_spill,
    kwq_gp_handoffs, kwq_gp_handed, kwq_gp_merges, kwq_gp_expired;

static inline kp_time_t
kwq_kpnow(void)
{
	return ((kp_time_t)(sbinuptime() >> 12));
}

/*
 * The cores in topology order: a depth-first walk of the tree, the CPUs
 * of an SMT leaf one core, every other CPU a core of its own; the L3 and
 * the package of each, as small ids for kp_dist().
 */
#ifdef SMP
static void
kwq_gossip_walk(struct cpu_group *cg, int l3, int pkg, int depth, int *nl3,
    int *npkg, cpuset_t *seen)
{
	struct kp_node *n;
	int c, i;

	if (depth == 1)
		pkg = (*npkg)++;
	if (cg->cg_level == CG_SHARE_L3)
		l3 = (*nl3)++;
	if (cg->cg_children > 0) {
		for (i = 0; i < cg->cg_children; i++)
			kwq_gossip_walk(&cg->cg_child[i], l3, pkg, depth + 1, nl3,
			    npkg, seen);
		return;
	}
	if (l3 < 0)
		l3 = (*nl3)++;
	n = NULL;
	CPU_FOREACH(c) {
		if (!CPU_ISSET(c, &cg->cg_mask) || CPU_ISSET(c, seen))
			continue;
		if (n == NULL || (cg->cg_flags & CG_FLAG_THREAD) == 0 ||
		    n->kn_nthr == KP_MAXSMT) {
			n = &kwq_kt.kt_node[kwq_kt.kt_ncores++];
			n->kn_l3 = (uint16_t)l3;
			n->kn_pkg = (uint16_t)(pkg < 0 ? 0 : pkg);
		}
		CPU_SET(c, seen);
		kwq_kt.kt_cpu_core[c] = (int)kwq_kt.kt_ncores - 1;
		kwq_kt.kt_cpu_slot[c] = n->kn_nthr;
		n->kn_cpu[n->kn_nthr++] = c;
	}
}
#endif

static int	kwq_sysctl_torus(SYSCTL_HANDLER_ARGS);
static void	kwq_gossip_sysctl(void);

void
kwq_gossip_init(void)
{
	cpuset_t seen;
	struct kp_node *n;
	int c, i, nl3 = 0, npkg = 0, ids = (int)mp_maxid + 1;

	memset(&kwq_kt, 0, sizeof(kwq_kt));
	kwq_kt.kt_maxcpu = (u_int)ids;
	kwq_kt.kt_node = malloc(sizeof(*kwq_kt.kt_node) * ids, M_KWQ, M_WAITOK | M_ZERO);
	kwq_kt.kt_core = malloc(sizeof(*kwq_kt.kt_core) * ids, M_KWQ, M_WAITOK | M_ZERO);
	kwq_kt.kt_cpu_core = malloc(sizeof(int) * ids, M_KWQ, M_WAITOK);
	kwq_kt.kt_cpu_slot = malloc(ids, M_KWQ, M_WAITOK | M_ZERO);
	for (c = 0; c < ids; c++)
		kwq_kt.kt_cpu_core[c] = -1;
	CPU_ZERO(&seen);
#ifdef SMP
	kwq_gossip_walk(smp_topo(), -1, -1, 0, &nl3, &npkg, &seen);
#endif
	CPU_FOREACH(c) {
		if (CPU_ISSET(c, &seen))
			continue;
		n = &kwq_kt.kt_node[kwq_kt.kt_ncores++];
		n->kn_l3 = (uint16_t)nl3++;
		kwq_kt.kt_cpu_core[c] = (int)kwq_kt.kt_ncores - 1;
		kwq_kt.kt_cpu_slot[c] = 0;
		n->kn_cpu[n->kn_nthr++] = c;
	}
	kwq_kblk = malloc_aligned(sizeof(*kwq_kblk) * kwq_kt.kt_ncores, KP_LINE,
	    M_KWQ, M_WAITOK | M_ZERO);
	for (i = 0; i < (int)kwq_kt.kt_ncores; i++)
		kwq_kt.kt_core[i] = &kwq_kblk[i];
	(void)kp_torus_build(&kwq_kt, KP_TORUS4, 1);
	kp_params_default(&kwq_kp);
	CPU_FOREACH(c)
		DPCPU_ID_SET(c, kwq_kp_rng, 0x9e3779b9u * (uint32_t)(c + 1));
	for (i = 0; i <= KP_STEP_FALLBACK; i++)
		kwq_gp_step[i] = counter_u64_alloc(M_WAITOK);
	kwq_gp_spill = counter_u64_alloc(M_WAITOK);
	kwq_gp_handoffs = counter_u64_alloc(M_WAITOK);
	kwq_gp_handed = counter_u64_alloc(M_WAITOK);
	kwq_gp_merges = counter_u64_alloc(M_WAITOK);
	kwq_gp_expired = counter_u64_alloc(M_WAITOK);
	kwq_gossip_sysctl();
}

void
kwq_gossip_fini(void)
{
	int i;

	if (kwq_kblk == NULL)
		return;
	sysctl_ctx_free(&kwq_gossip_ctx);
	for (i = 0; i <= KP_STEP_FALLBACK; i++)
		counter_u64_free(kwq_gp_step[i]);
	counter_u64_free(kwq_gp_spill);
	counter_u64_free(kwq_gp_handoffs);
	counter_u64_free(kwq_gp_handed);
	counter_u64_free(kwq_gp_merges);
	counter_u64_free(kwq_gp_expired);
	free(kwq_kblk, M_KWQ);
	free(kwq_kt.kt_cpu_slot, M_KWQ);
	free(kwq_kt.kt_cpu_core, M_KWQ);
	free(kwq_kt.kt_core, M_KWQ);
	free(kwq_kt.kt_node, M_KWQ);
	kwq_kblk = NULL;
}

struct kwq_gbl {
	uint64_t	b, s, qn;	/* the figure's backlog, the stealable one, Q */
	struct kwq_cpu	*best;		/* the longest stealable list */
	u_int		bd;
};

static inline void
kwq_gossip_one(struct kwq_gbl *g, struct kwq_cpu *kc, struct kwq_cpu *cur,
    u_int ahead)
{
	uint64_t w;
	u_int d = kc->kc_depth + (kc == cur ? ahead : 0);

	w = (uint64_t)d * kc->kc_cost_ns;
	g->b += w < g->qn ? w : g->qn;
	if ((kc->kc_q->kwq_flags & KWQ_F_STEALABLE) == 0)
		return;
	g->s += (uint64_t)kc->kc_depth * kc->kc_cost_ns;
	if (kc->kc_depth > g->bd) {
		g->bd = kc->kc_depth;
		g->best = kc;
	}
}

/*
 * The worker's stealable and other backlog for the load figure (S2.3):
 * each list's work, at most a quantum of it (DRR serves the others in
 * between), the pass starting on `cur' with its `ahead' items included.
 * kw_mtx held.
 */
static uint64_t
kwq_gossip_backlog(struct kwq_worker *kw, struct kwq_cpu *cur, u_int ahead,
    struct kwq_cpu **bestp, u_int *bdp, uint64_t *stealp)
{
	struct kwq_gbl g = { .qn = kw->kw_knobs->quantum_ns };
	struct kwq_cpu *kc;
	u_int k = 0;

	TAILQ_FOREACH(kc, &kw->kw_new, kc_active) {
		if (k++ >= 32)
			break;
		kwq_gossip_one(&g, kc, cur, ahead);
	}
	TAILQ_FOREACH(kc, &kw->kw_active, kc_active) {
		if (k++ >= 32)
			break;
		kwq_gossip_one(&g, kc, cur, ahead);
	}
	if (bestp != NULL) {
		*bestp = g.best;
		*bdp = g.bd;
		*stealp = g.s;
	}
	return (g.b);
}

/* A decision point (S2.3): the figure, published when its bucket changes. */
void
kwq_gossip_point(struct kwq_worker *kw, struct kwq_cpu *cur, u_int ahead)
{
	const struct kwq_sched_knobs *k = kw->kw_knobs;
	uint64_t b, word;
	u_int us, share;
	int core;

	mtx_assert(&kw->kw_mtx, MA_OWNED);
	if (kw->kw_class != KWQ_BULK || kwq_bulk_place != KWQ_PLACE_GOSSIP)
		return;
	core = kwq_kt.kt_cpu_core[kw->kw_cpu];
	if (core < 0)
		return;
	b = kw->kw_sleeping ? 0 : kwq_gossip_backlog(kw, cur, ahead, NULL, NULL, NULL);
	kw->kw_kp_start_ns = b;
	share = kw->kw_cap_over ? k->cap_permille * 1024 / 1000 : 1024;
	us = kp_load_us(b, share, 0, (u_int)(k->quantum_ns / 1000));
	if (kp_load_update(&kw->kw_kl, kwq_kpnow(), us, &kwq_kp, &word))
		KP_STORE64(&kwq_kblk[core].kc_st.ks_w[kwq_kt.kt_cpu_slot[kw->kw_cpu]], word);
}

/*
 * A sleeping worker with a fall pending publishes it when the hold has
 * passed: a direct callout, re-armed if it fired early.
 */
static void
kwq_gossip_pub_fire(void *arg)
{
	struct kwq_worker *kw = arg;

	mtx_lock_spin(&kw->kw_mtx);
	kw->kw_pub_armed = false;
	if (kw->kw_sleeping && !kw->kw_exit) {
		kwq_gossip_point(kw, NULL, 0);
		kwq_gossip_sleep(kw);
	}
	mtx_unlock_spin(&kw->kw_mtx);
}

/* Going to sleep (kw_mtx held): the figure, and the hold's callout. */
void
kwq_gossip_sleep(struct kwq_worker *kw)
{
	kp_time_t left;

	mtx_assert(&kw->kw_mtx, MA_OWNED);
	if (kwq_bulk_place != KWQ_PLACE_GOSSIP || kw->kw_exit)
		return;
	if (!kw->kw_pub_armed)
		kwq_gossip_point(kw, NULL, 0);
	left = kp_load_hold_left(&kw->kw_kl, kwq_kpnow(), &kwq_kp);
	if (left == 0 || kw->kw_pub_armed)
		return;
	kw->kw_pub_armed = true;
	callout_reset_sbt_on(&kw->kw_pub_co, ((sbintime_t)left << 12) + SBT_1US,
	    SBT_1US * 8, kwq_gossip_pub_fire, kw, kw->kw_cpu, C_DIRECT_EXEC);
}

/* The merge (S2.4) of cpu's core when due, under its try-flag. */
void
kwq_gossip_merge(int cpu)
{
	struct kp_mres r;
	kp_time_t now = kwq_kpnow();
	int core = kwq_kt.kt_cpu_core[cpu];

	if (core < 0 || !kp_merge_due(&kwq_kt, (u_int)core, now))
		return;
	if (!KP_TRYFLAG(&kwq_kblk[core].kc_ctl))
		return;
	kp_merge(&kwq_kt, (u_int)core, now, &kwq_kp, &r);
	KP_CLRFLAG(&kwq_kblk[core].kc_ctl);
	counter_u64_add(kwq_gp_merges, 1);
}

/* kp_place()'s check of a candidate: the queue's list there below H. */
static bool
kwq_gossip_ok(void *arg, int cpu)
{
	struct kwq *q = arg;
	struct kwq_cpu *kc = q->kwq_pcpu[cpu];

	return (kc != NULL && kc->kc_depth < q->kwq_limit &&
	    (uint64_t)kc->kc_depth * kc->kc_cost_ns < (uint64_t)kwq_kp.h_us * 1000);
}

/*
 * KWQ_CPU_ANY under place=1 (S2.5): a due merge of the producer's core,
 * then kp_place() from its table.  A BULK handler resubmitting work is
 * step 0's producer: its own load without what its pass has done, the
 * figure of the pass start less the CPU time the pass has used.
 */
int
kwq_pick_gossip(struct kwq *q, int spill)
{
	struct kwq_worker *kw;
	struct kp_pick pk;
	struct kwq_cpu *kc;
	uint64_t done;
	uint32_t *rng;
	int me, self_us = -1, c;

	critical_enter();
	me = curcpu;
	rng = DPCPU_PTR(kwq_kp_rng);
	critical_exit();
	kwq_gossip_merge(me);
	kw = kwq_workers[KWQ_BULK][me];
	if (kw != NULL && kw->kw_td == curthread && kw->kw_cur != NULL) {
		done = kwq_cputime_ns(kw) - kw->kw_pass_start;
		self_us = kw->kw_kp_start_ns > done ?
		    (int)((kw->kw_kp_start_ns - done) / 1000) : 0;
	}
	kc = q->kwq_pcpu[me];
	c = kp_place(&kwq_kt, me, kwq_kpnow(), kc->kc_cost_ns, self_us, &kwq_kp,
	    rng, kwq_gossip_ok, q, &pk);
	counter_u64_add(kwq_gp_step[pk.step <= KP_STEP_FALLBACK ? pk.step :
	    KP_STEP_FALLBACK], 1);
	if (__predict_false(pk.expired))
		counter_u64_add(kwq_gp_expired, 1);
	if (c < 0 || c > (int)mp_maxid || CPU_ABSENT(c))
		c = me;
	SDT_PROBE5(kwq, , , place, q, me, c, KWQ_PICK_GOSSIP + pk.step, spill);
	return (c);
}

/* Spill (S2.6): a fixed CPU's item placed as KWQ_CPU_ANY at or above H. */
int
kwq_gossip_spill(struct kwq *q, int cpu)
{
	struct kwq_cpu *kc = q->kwq_pcpu[cpu];

	if (!kwq_steal_on || (!kp_spill(&kwq_kt, cpu, &kwq_kp) &&
	    (uint64_t)kc->kc_depth * kc->kc_cost_ns < (uint64_t)kwq_kp.h_us * 1000))
		return (cpu);
	counter_u64_add(kwq_gp_spill, 1);
	return (kwq_pick_gossip(q, 1));
}

/*
 * cpu's status word says idle: written, below idle_us.  As kp_core_view()
 * reads it, without an age: the owner writes its word at every change, so
 * a worker asleep for longer than T_exp is still idle (the first version
 * aged it and kwq_scatter() found no idle CPU after 32 ms of quiet).
 */
bool
kwq_gossip_cpu_idle(int cpu)
{
	uint64_t w;
	int core = kwq_kt.kt_cpu_core[cpu];

	if (core < 0)
		return (false);
	w = KP_LOAD64(&kwq_kblk[core].kc_st.ks_w[kwq_kt.kt_cpu_slot[cpu]]);
	return (kp_st_valid(w) && kp_st_load(w) < kwq_kp.idle_us);
}

/* The pass start's helper test (S2.7's pass cap): an idle core near. */
bool
kwq_gossip_idle_near(int cpu)
{
	return (kp_idle_near(&kwq_kt, cpu, kwq_kpnow(), &kwq_kp));
}

/*
 * The hand-off (S2.7), at a pass end, no lock held: with a stealable
 * backlog of H or more, the older part of the longest stealable list to a
 * table candidate at or below L, checked, and its doorbell; at most
 * kwq_handoff_n per pass end.  The read side of kwq_steal_rm is the drain
 * fence, as for a steal.
 */
void
kwq_gossip_handoff(struct kwq_worker *kw)
{
	struct rm_priotracker tr;
	struct kp_pick pk;
	struct kwq_cpu *kc, *tkc;
	struct kwq *q;
	struct kwq_item *head, *tail;
	uint64_t s;
	u_int i, k, bd, take, left;
	int t;

	for (i = 0; i < kwq_handoff_n && kwq_steal_on && kwq_nstealable != 0; i++) {
		mtx_lock_spin(&kw->kw_mtx);
		(void)kwq_gossip_backlog(kw, NULL, 0, &kc, &bd, &s);
		mtx_unlock_spin(&kw->kw_mtx);
		if (kc == NULL || bd < 2 || s < (uint64_t)kwq_kp.h_us * 1000)
			return;
		t = kp_handoff_target(&kwq_kt, kw->kw_cpu, kwq_kpnow(), &kwq_kp,
		    DPCPU_PTR(kwq_kp_rng), &pk);
		if (t < 0 || t == kw->kw_cpu || t > (int)mp_maxid || CPU_ABSENT(t))
			return;
		q = kc->kc_q;
		rm_rlock(&kwq_steal_rm, &tr);
		kwq_kc_lock(kc);
		if (!q->kwq_active || kc->kc_depth < 2) {
			kwq_kc_unlock(kc);
			rm_runlock(&kwq_steal_rm, &tr);
			return;
		}
		take = kp_handoff_batch(kc->kc_depth, kc->kc_cost_ns, kwq_steal_max, &kwq_kp);
		if (take >= kc->kc_depth)
			take = kc->kc_depth / 2 > 0 ? kc->kc_depth / 2 : 1;
		head = STAILQ_FIRST(&kc->kc_list);
		tail = head;
		for (k = 1; k < take; k++)
			tail = tail->kwi_link.stqe_next;
		KASSERT(KWQ_TAGBIT(tail->kwi_link.stqe_next) == 0,
		    ("kwq_gossip_handoff: %s: a notifier on a stealable list",
		    q->kwq_name));
		kc->kc_list.stqh_first = tail->kwi_link.stqe_next;
		if (kc->kc_list.stqh_first == NULL)
			kc->kc_list.stqh_last = &kc->kc_list.stqh_first;
		tail->kwi_link.stqe_next = NULL;
		kc->kc_depth -= take;
		left = kc->kc_depth;
		kc->kc_steals_out++;
		kc->kc_stolen_out += take;
		kwq_list_check(kc, "handoff giver");
		kwq_kc_unlock(kc);

		tkc = q->kwq_pcpu[t];
		kwq_kc_lock(tkc);
		kwq_list_concat(tkc, head, tail);
		tkc->kc_depth += take;
		tkc->kc_steals_in++;
		tkc->kc_stolen_in += take;
		if (tkc->kc_cost_ns == 0)
			tkc->kc_cost_ns = kc->kc_cost_ns;
		kwq_list_check(tkc, "handoff taker");
		(void)kwq_kc_place(tkc, take);	/* its doorbell */
		kwq_kc_unlock(tkc);
		rm_runlock(&kwq_steal_rm, &tr);
		SDT_PROBE6(kwq, , , steal, q, kw->kw_cpu, t, (int)take,
		    KWQ_STEAL_HANDOFF, (int)left);
		kw->kw_handoffs++;
		counter_u64_add(kwq_gp_handoffs, 1);
		counter_u64_add(kwq_gp_handed, take);
	}
}

/* kern.kwq.bulk.gossip.torus: each core's CPUs, L3, package and neighbours. */
static int
kwq_sysctl_torus(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	struct kp_node *n;
	u_int i, j;
	int error;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	sbuf_printf(&sb, "%u cores, %u x %u", kwq_kt.kt_ncores, kwq_kt.kt_rows,
	    kwq_kt.kt_cols);
	for (i = 0; i < kwq_kt.kt_ncores; i++) {
		n = &kwq_kt.kt_node[i];
		sbuf_printf(&sb, "\n%u: cpus", i);
		for (j = 0; j < n->kn_nthr; j++)
			sbuf_printf(&sb, " %d", n->kn_cpu[j]);
		sbuf_printf(&sb, " l3 %u pkg %u nb", n->kn_l3, n->kn_pkg);
		for (j = 0; j < n->kn_nnb; j++)
			sbuf_printf(&sb, " %u", n->kn_nb[j]);
	}
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static void
kwq_gossip_sysctl(void)
{
	struct sysctl_ctx_list *ctx = &kwq_gossip_ctx;
	struct sysctl_oid *oid;
	struct sysctl_oid_list *g;
	static const char *steps[] = { "self", "idle_near", "sibling", "local",
	    "table", "neighbours", "fallback" };
	char name[32];
	int i;

	sysctl_ctx_init(ctx);
	oid = SYSCTL_ADD_NODE(ctx, SYSCTL_STATIC_CHILDREN(_kern_kwq_bulk), OID_AUTO,
	    "gossip", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
	    "the gossip scheme (place=1, SPEC-p6g.md S2): torus, constants, counters");
	g = SYSCTL_CHILDREN(oid);
	SYSCTL_ADD_PROC(ctx, g, OID_AUTO, "torus",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, kwq_sysctl_torus,
	    "A", "the torus of cores: each core's CPUs, L3, package and neighbours");
	SYSCTL_ADD_UINT(ctx, g, OID_AUTO, "h_us", CTLFLAG_RW, &kwq_kp.h_us, 0,
	    "H: hand-off and spill at or above this backlog, us");
	SYSCTL_ADD_UINT(ctx, g, OID_AUTO, "l_us", CTLFLAG_RW, &kwq_kp.l_us, 0,
	    "L: hand-off to a thread at or below this load, us");
	SYSCTL_ADD_UINT(ctx, g, OID_AUTO, "llocal_us", CTLFLAG_RW, &kwq_kp.llocal_us, 0,
	    "L_local: the producer's own thread is good below this load, us");
	SYSCTL_ADD_UINT(ctx, g, OID_AUTO, "handoff_n", CTLFLAG_RW, &kwq_handoff_n, 0,
	    "hand-offs at most per pass end");
	for (i = 0; i <= KP_STEP_FALLBACK; i++) {
		snprintf(name, sizeof(name), "place_%s", steps[i]);
		SYSCTL_ADD_COUNTER_U64(ctx, g, OID_AUTO, strdup(name, M_KWQ),
		    CTLFLAG_RD, &kwq_gp_step[i], "KWQ_CPU_ANY placements by this step (S2.5)");
	}
	SYSCTL_ADD_COUNTER_U64(ctx, g, OID_AUTO, "spills", CTLFLAG_RD, &kwq_gp_spill,
	    "items for a fixed CPU placed elsewhere, that CPU at or above H (S2.6)");
	SYSCTL_ADD_COUNTER_U64(ctx, g, OID_AUTO, "handoffs", CTLFLAG_RD,
	    &kwq_gp_handoffs, "batches handed off at a pass end (S2.7)");
	SYSCTL_ADD_COUNTER_U64(ctx, g, OID_AUTO, "handed", CTLFLAG_RD, &kwq_gp_handed,
	    "items in those batches");
	SYSCTL_ADD_COUNTER_U64(ctx, g, OID_AUTO, "merges", CTLFLAG_RD, &kwq_gp_merges,
	    "merges run (S2.4)");
	SYSCTL_ADD_COUNTER_U64(ctx, g, OID_AUTO, "expired_used", CTLFLAG_RD,
	    &kwq_gp_expired, "placements that used an expired table entry (W7: 0)");
}
