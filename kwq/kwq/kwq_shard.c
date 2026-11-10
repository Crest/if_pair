/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * P8 BULK placement, kern.kwq.bulk.place=2 (../SPEC-p6g.md S3.3 and
 * K8-K12; simulated as policy 8 in ../sim/place/placesim.c).
 *
 * Today's code (place=0, SCHED.md S16) keeps one idle mask that every
 * BULK worker writes at each sleep and wake, and an idle path that scans
 * every stealable list in reach before it sleeps.  P8 keeps today's
 * placement and replaces what costs:
 *
 * - The idle mask is cut into one line per group of kwq_shard_group
 *   CPUs (16) in topology order, written by that group's CPUs only.  A
 *   producer reads its own group's line and up to kwq_shard_probe more,
 *   nearest first, and when none has an idle worker one far group's line
 *   from a per-CPU cursor, so an island whose overflow exceeds its
 *   neighbourhood still reaches the idle rest of the machine.
 * - A CPU whose stealable backlog reaches kwq_donor_us sets its bit in
 *   one donor line, at the transitions only, and names its longest
 *   stealable list in a hint line of its own; an idle worker reads the
 *   donor line instead of scanning lists, and steals from the nearest
 *   donor in reach.  A lone item behind running work counts that work
 *   and may be taken by an idle thief while its worker is in a pass, or
 *   from a starved donor: narrower than today's kwq_steal_lone_ok(),
 *   which also takes it from a worker asleep with its list placed or
 *   waiting in the idle class.
 * - directed (default 2): a wake to steal names its victim and list in
 *   the woken worker's mailbox, and an idle thief tries the victim of its
 *   last batch again before it sleeps (the chain), both only for lists
 *   whose items cost at least directed_us (default 10 us).
 * - A steal takes no more than leaves the taker below a donor's backlog,
 *   so a batch is not stolen on again before new arrivals lift its holder;
 *   a worker whose backlog falls below kwq_pull_us pulls from a donor at
 *   the end of a pass.
 * - A starvation watchdog, a per-CPU callout armed while the CPU holds
 *   stealable work, flags a worker that reached no decision point since
 *   it was armed a starved donor, whose whole list may be stolen, lone
 *   items too: a CPU whose BULK worker gets no time cannot flag itself.
 *
 * Who writes the words added: a group's idle line its CPUs, the donor line
 * a CPU at its donor transitions, a CPU's hint that CPU, and a worker's
 * mailbox whoever wakes it, under its kw_mtx.  The hint names a queue by its slot in
 * kwq_steal_tab and its id, never by a pointer, and is checked under the
 * read side of kwq_steal_rm, which a drain's unlisting write-locks.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/counter.h>
#include <sys/cpuset.h>
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
#include <vm/vm.h>

#include "kwq_internal.h"
#include "kwq_sched.h"
#include "kwq_sdt.h"

volatile u_int kwq_bulk_place = KWQ_PLACE_TODAY;
struct kwq *kwq_steal_tab[KWQ_STEAL_SLOTS];

static u_int kwq_shard_group = 16;	/* CPUs per idle line; loader tunable */
static u_int kwq_shard_probe = 2;	/* near groups read besides the own one */
static u_int kwq_donor_us = 500;	/* a donor's backlog */
static u_int kwq_pull_us = 500;		/* a worker below it pulls at pass end */
static u_int kwq_watch_us = 1000;	/* the starvation watchdog's period; 0: off */
static u_int kwq_directed = 2;		/* the wake names the victim (the mailbox); 2: the chain */
static u_int kwq_directed_us = 10;	/* directed only for items at least this costly; 0: all */

#define	KWQ_NEAR_MAX	5		/* the own group and at most 4 more */
#define	KWQ_GRP_MAX	64		/* bits of a group's line */
#define	KWQ_RING_WALK	16		/* lists a decision point looks at */

struct kwq_grp {
	volatile uint64_t	kg_idle;	/* a bit per member whose worker sleeps */
} __aligned(KWQ_LINE);

struct kwq_hint {
	volatile uint64_t	kh_word;	/* KH_* below; 0: none */
} __aligned(KWQ_LINE);

static struct {
	struct _cpuset		ds_set;		/* CPUs that are donors */
} __aligned(KWQ_LINE) kwq_donors;

/* hint word: slot + 1 (16 bits), an item's cost in us (16), the queue's id (32) */
#define	KH_MAKE(slot, us, id)	(((uint64_t)((slot) + 1) << 48) | \
				    ((uint64_t)(us) << 32) | (uint64_t)(id))
#define	KH_SLOT(h)		((int)((h) >> 48) - 1)
#define	KH_US(h)		((u_int)((h) >> 32) & 0xffff)
#define	KH_ID(h)		((uint32_t)(h))

static struct kwq_grp *kwq_grps;
static struct kwq_hint *kwq_hints;	/* [mp_maxid + 1] */
static int kwq_ngrps;
static int *kwq_cpu_grp;		/* [mp_maxid + 1]: its group */
static int *kwq_cpu_gbit;		/* [mp_maxid + 1]: its bit there */
static int *kwq_grp_cpu;		/* [ngrps * KWQ_GRP_MAX]: a bit's CPU */
static u_short (*kwq_grp_near)[KWQ_NEAR_MAX];	/* the own group, then the nearest */
static u_char *kwq_grp_nnear;
static uint32_t kwq_steal_ids;		/* under kwq_steal_rm's write side */
DPCPU_DEFINE_STATIC(int, kwq_far_g);	/* the far probe's group + 1; 0: unset */

SYSCTL_DECL(_kern_kwq_bulk);
static struct sysctl_ctx_list kwq_shard_ctx;
static void kwq_shard_sysctl(void);

static uint64_t kwq_hint_word(struct kwq_cpu *kc);

/* placement outcomes, by where the item went */
static counter_u64_t kwq_pk_near, kwq_pk_far, kwq_pk_self, kwq_pk_nondonor,
    kwq_pk_probe, kwq_pk_spill;

static inline uint64_t
kwq_grp_bit(int cpu)
{
	return (1ULL << kwq_cpu_gbit[cpu]);
}

static inline bool
kwq_is_donor(int cpu)
{
	return (CPU_ISSET(cpu, &kwq_donors.ds_set));
}

/*
 * The groups: consecutive runs of kwq_shard_group CPUs in a depth-first
 * walk of the topology tree (smp_topo()), so a group is a cache domain or
 * a run of neighbouring ones, then the CPUs the tree missed.
 */
#ifdef SMP
static void
kwq_shard_walk(struct cpu_group *cg, int *order, int *n, cpuset_t *seen)
{
	int c, i;

	if (cg->cg_children == 0) {
		CPU_FOREACH(c) {
			if (CPU_ISSET(c, &cg->cg_mask) && !CPU_ISSET(c, seen)) {
				CPU_SET(c, seen);
				order[(*n)++] = c;
			}
		}
		return;
	}
	for (i = 0; i < cg->cg_children; i++)
		kwq_shard_walk(&cg->cg_child[i], order, n, seen);
}
#endif

void
kwq_shard_init(void)
{
	cpuset_t seen;
	int *order, n = 0, cpu, g, i, j, k, nord, ids = (int)mp_maxid + 1;
	u_int gs;

	TUNABLE_INT_FETCH("kern.kwq.bulk.group", &kwq_shard_group);
	if (kwq_shard_group < 1)
		kwq_shard_group = 1;
	if (kwq_shard_group > KWQ_GRP_MAX)
		kwq_shard_group = KWQ_GRP_MAX;
	gs = kwq_shard_group;
	order = malloc(sizeof(int) * ids, M_KWQ, M_WAITOK | M_ZERO);
	CPU_ZERO(&seen);
#ifdef SMP
	kwq_shard_walk(smp_topo(), order, &n, &seen);
#endif
	CPU_FOREACH(cpu) {
		if (!CPU_ISSET(cpu, &seen)) {
			CPU_SET(cpu, &seen);
			order[n++] = cpu;
		}
	}
	kwq_ngrps = (n + (int)gs - 1) / (int)gs;
	kwq_grps = malloc(sizeof(*kwq_grps) * kwq_ngrps, M_KWQ, M_WAITOK | M_ZERO);
	kwq_hints = malloc(sizeof(*kwq_hints) * ids, M_KWQ, M_WAITOK | M_ZERO);
	kwq_cpu_grp = malloc(sizeof(int) * ids, M_KWQ, M_WAITOK | M_ZERO);
	kwq_cpu_gbit = malloc(sizeof(int) * ids, M_KWQ, M_WAITOK | M_ZERO);
	kwq_grp_cpu = malloc(sizeof(int) * kwq_ngrps * KWQ_GRP_MAX, M_KWQ,
	    M_WAITOK | M_ZERO);
	kwq_grp_near = malloc(sizeof(*kwq_grp_near) * ids, M_KWQ, M_WAITOK | M_ZERO);
	kwq_grp_nnear = malloc(ids, M_KWQ, M_WAITOK | M_ZERO);
	for (i = 0; i < n; i++) {
		g = i / (int)gs;
		kwq_cpu_grp[order[i]] = g;
		kwq_cpu_gbit[order[i]] = i % (int)gs;
		kwq_grp_cpu[g * KWQ_GRP_MAX + i % (int)gs] = order[i];
	}
	/* each CPU's near groups: its own, then those of its nearest CPUs */
	CPU_FOREACH(cpu) {
		k = 0;
		kwq_grp_near[cpu][k++] = (u_short)kwq_cpu_grp[cpu];
		nord = kwq_topo_ring[cpu][KWQ_TOPO_RINGS - 1];
		for (i = 0; i < nord && k < KWQ_NEAR_MAX; i++) {
			g = kwq_cpu_grp[kwq_topo_order[cpu][i]];
			for (j = 0; j < k && kwq_grp_near[cpu][j] != g; j++)
				;
			if (j == k)
				kwq_grp_near[cpu][k++] = (u_short)g;
		}
		kwq_grp_nnear[cpu] = (u_char)k;
	}
	free(order, M_KWQ);
	CPU_ZERO(&kwq_donors.ds_set);
	kwq_pk_near = counter_u64_alloc(M_WAITOK);
	kwq_pk_far = counter_u64_alloc(M_WAITOK);
	kwq_pk_self = counter_u64_alloc(M_WAITOK);
	kwq_pk_nondonor = counter_u64_alloc(M_WAITOK);
	kwq_pk_probe = counter_u64_alloc(M_WAITOK);
	kwq_pk_spill = counter_u64_alloc(M_WAITOK);
	/*
	 * The default (SPEC-p6g.md D12): P8 on a machine of one NUMA domain,
	 * today's code on more, where the simulator's two-socket model saw P8
	 * collapse near saturation and no machine was measured.  smp_topo()
	 * has no package level (x86 builds its groups from caches and NUMA
	 * nodes), so the domain count stands in for the package count; a
	 * two-socket machine with its memory interleaved shows one domain.
	 * The loader tunable, applied when the knob registers, wins.
	 */
	kwq_bulk_place = vm_ndomains > 1 ? KWQ_PLACE_TODAY : KWQ_PLACE_SHARD;
	kwq_shard_sysctl();
}

void
kwq_shard_fini(void)
{
	if (kwq_grps == NULL)
		return;
	sysctl_ctx_free(&kwq_shard_ctx);	/* waits for a reader in a handler */
	counter_u64_free(kwq_pk_near);
	counter_u64_free(kwq_pk_far);
	counter_u64_free(kwq_pk_self);
	counter_u64_free(kwq_pk_nondonor);
	counter_u64_free(kwq_pk_probe);
	counter_u64_free(kwq_pk_spill);
	free(kwq_grp_nnear, M_KWQ);
	free(kwq_grp_near, M_KWQ);
	free(kwq_grp_cpu, M_KWQ);
	free(kwq_cpu_gbit, M_KWQ);
	free(kwq_cpu_grp, M_KWQ);
	free(kwq_hints, M_KWQ);
	free(kwq_grps, M_KWQ);
	kwq_grps = NULL;
}

/* The slot table: create and drain (kwq.c), kwq_steal_rm write-locked. */
int
kwq_shard_slot_add(struct kwq *q)
{
	int i;

	q->kwq_steal_slot = -1;
	q->kwq_steal_id = ++kwq_steal_ids;
	for (i = 0; i < KWQ_STEAL_SLOTS; i++) {
		if (kwq_steal_tab[i] == NULL) {
			kwq_steal_tab[i] = q;
			q->kwq_steal_slot = i;
			return (0);
		}
	}
	return (ENOSPC);	/* stealable, but never named by a hint */
}

void
kwq_shard_slot_del(struct kwq *q)
{
	if (q->kwq_steal_slot >= 0 && kwq_steal_tab[q->kwq_steal_slot] == q)
		kwq_steal_tab[q->kwq_steal_slot] = NULL;
	q->kwq_steal_slot = -1;
}

/* The idle bit: in the line the mode at the sleep reads, cleared there. */
void
kwq_idle_set(struct kwq_worker *kw)
{
	if (kwq_bulk_place == KWQ_PLACE_GOSSIP) {
		kw->kw_idle_at = KWQ_IDLE_NONE;	/* the status word says it */
		kwq_gossip_sleep(kw);
	} else if (kwq_bulk_place == KWQ_PLACE_SHARD) {
		atomic_set_64(&kwq_grps[kwq_cpu_grp[kw->kw_cpu]].kg_idle,
		    kwq_grp_bit(kw->kw_cpu));
		kw->kw_idle_at = KWQ_IDLE_GROUP;
	} else {
		CPU_SET_ATOMIC(kw->kw_cpu, &kwq_bulk_idle);
		kw->kw_idle_at = KWQ_IDLE_MASK;
	}
}

void
kwq_idle_clr(struct kwq_worker *kw)
{
	if (kw->kw_idle_at == KWQ_IDLE_GROUP)
		atomic_clear_64(&kwq_grps[kwq_cpu_grp[kw->kw_cpu]].kg_idle,
		    kwq_grp_bit(kw->kw_cpu));
	else if (kw->kw_idle_at == KWQ_IDLE_MASK)
		CPU_CLR_ATOMIC(kw->kw_cpu, &kwq_bulk_idle);
	kw->kw_idle_at = KWQ_IDLE_NONE;
}

/*
 * Does cpu's BULK worker sleep, as the placement mode sees it (a hint,
 * kwq_scatter()'s test): its group's line under place=2, its status word
 * under place=1 (no idle mask is written there), the idle mask under 0.
 */
bool
kwq_bulk_idle_hint(int cpu)
{
	switch (kwq_bulk_place) {
	case KWQ_PLACE_SHARD:
		return ((atomic_load_64(&kwq_grps[kwq_cpu_grp[cpu]].kg_idle) &
		    kwq_grp_bit(cpu)) != 0);
	case KWQ_PLACE_GOSSIP:
		return (kwq_gossip_cpu_idle(cpu));
	default:
		return (CPU_ISSET(cpu, &kwq_bulk_idle));
	}
}

/*
 * The pass start's helper test (kwq_steal_idle_near()) under place=2: a
 * BULK worker in reach sleeps, read from the group lines that cover the
 * reach as kwq_shard_wake() reads them.  Within the own group only, a
 * donor with no sleeper there took full batches while CPUs of the next
 * group slept (the guest, groups of one CPU).
 */
bool
kwq_shard_idle_near(int cpu)
{
	uint64_t w = 0;
	u_int i, reach;
	int c, g, lg = -1;

	reach = kwq_topo_reach(cpu);
	for (i = 0; i < reach; i++) {
		c = kwq_topo_order[cpu][i];
		g = kwq_cpu_grp[c];
		if (g != lg) {
			w = atomic_load_64(&kwq_grps[g].kg_idle);
			lg = g;
		}
		if ((w & kwq_grp_bit(c)) != 0)
			return (true);
	}
	return (false);
}

/*
 * The steal wake under place=2: the nearest sleeping BULK worker in reach,
 * as today's wake, reading the group lines that cover the reach (one or
 * two with groups of 16).  The simulator's P8 woke within the own group
 * only; on the guest with groups of one CPU that left a donor with no
 * sleeper of its own group nobody to wake while three workers slept.
 */
bool
kwq_shard_wake(int cpu, uint64_t mw, int why)
{
	struct kwq_worker *kw;
	uint64_t w = 0;
	u_int i, reach;
	int c, g, lg = -1, filled = 0;

	reach = kwq_topo_reach(cpu);
	for (i = 0; i < reach; i++) {
		c = kwq_topo_order[cpu][i];
		g = kwq_cpu_grp[c];
		if (g != lg) {
			w = atomic_load_64(&kwq_grps[g].kg_idle);
			lg = g;
		}
		if ((w & kwq_grp_bit(c)) == 0)
			continue;
		kw = kwq_workers[KWQ_BULK][c];
		mtx_lock_spin(&kw->kw_mtx);
		if (kw->kw_sleeping && !kw->kw_exit) {
			kw->kw_sleeping = false;
			kw->kw_steal_wakes++;
			/*
			 * directed: the mailbox names the victim and its list,
			 * in the line this wake writes anyway, so the thief
			 * steals from its waker without a donor's backlog.
			 */
			if (kwq_directed) {
				if (mw == 0)
					mw = atomic_load_64(&kwq_hints[cpu].kh_word);
				if (mw != 0 && KH_US(mw) < kwq_directed_us)
					mw = 0;		/* cheap items: the donor rule */
				if (mw != 0) {
					kw->kw_mb_cpu = cpu;
					kw->kw_mb_word = mw;
					filled = 1;
				}
			}
			wakeup_one(kw->kw_chan);
			mtx_unlock_spin(&kw->kw_mtx);
			SDT_PROBE4(kwq, , , steal__wake, cpu, c, why, filled);
			return (true);
		}
		mtx_unlock_spin(&kw->kw_mtx);
	}
	SDT_PROBE4(kwq, , , steal__wake, cpu, -1, why, 0);
	return (false);
}

/* A list as a hint's word: its slot, its item's cost in us, its queue's id; 0 if none. */
static uint64_t
kwq_hint_word(struct kwq_cpu *kc)
{
	struct kwq *q = kc->kc_q;
	uint64_t us = kc->kc_cost_ns / 1000;

	if (q->kwq_steal_slot < 0)
		return (0);
	if (us > 0xffff)
		us = 0xffff;
	return (KH_MAKE(q->kwq_steal_slot, us, q->kwq_steal_id));
}

/*
 * The mailbox word for a steal wake from kc's list: its hint word under
 * place=2 with shard.directed, else 0.  The list is held (its lock, or a
 * pass on it), so kc's queue is alive.
 */
uint64_t
kwq_steal_word(struct kwq_cpu *kc)
{
	if (kwq_bulk_place != KWQ_PLACE_SHARD || kwq_directed == 0)
		return (0);
	return (kwq_hint_word(kc));
}

static void
kwq_hint_set(int cpu, struct kwq_cpu *kc)
{
	uint64_t w = kwq_hint_word(kc);

	if (w != 0)
		atomic_store_64(&kwq_hints[cpu].kh_word, w);
}

/*
 * The longest stealable list among the worker's prompt list, new list
 * and ring (KWQ_RING_WALK of the first two together at most, twice that
 * of the three), and the stealable backlog they hold, ns.
 * kw_mtx held (both lists are under it); the depths are read unlocked.
 */
static struct kwq_cpu *
kwq_ring_backlog(struct kwq_worker *kw, struct kwq_cpu *cur, uint64_t *bp, u_int *bdp)
{
	struct kwq_cpu *kc, *best = NULL;
	uint64_t b = 0;
	u_int bd = 0, d, k = 0;
	bool seen = false;

	TAILQ_FOREACH(kc, &kw->kw_prompt, kc_active) {
		if (k++ == KWQ_RING_WALK)
			break;
		if (kc == cur)
			seen = true;
		if ((kc->kc_q->kwq_flags & KWQ_F_STEALABLE) == 0)
			continue;
		d = kc->kc_depth;
		b += (uint64_t)d * kc->kc_cost_ns;
		if (d > bd) {
			bd = d;
			best = kc;
		}
	}
	TAILQ_FOREACH(kc, &kw->kw_new, kc_active) {
		if (k++ >= KWQ_RING_WALK)
			break;
		if (kc == cur)
			seen = true;
		if ((kc->kc_q->kwq_flags & KWQ_F_STEALABLE) == 0)
			continue;
		d = kc->kc_depth;
		b += (uint64_t)d * kc->kc_cost_ns;
		if (d > bd) {
			bd = d;
			best = kc;
		}
	}
	TAILQ_FOREACH(kc, &kw->kw_active, kc_active) {
		if (k++ >= 2 * KWQ_RING_WALK)
			break;
		if (kc == cur)
			seen = true;
		if ((kc->kc_q->kwq_flags & KWQ_F_STEALABLE) == 0)
			continue;
		d = kc->kc_depth;
		b += (uint64_t)d * kc->kc_cost_ns;
		if (d > bd) {
			bd = d;
			best = kc;
		}
	}
	if (cur != NULL && !seen && (cur->kc_q->kwq_flags & KWQ_F_STEALABLE)) {
		d = cur->kc_depth;
		b += (uint64_t)d * cur->kc_cost_ns;
		if (d > bd) {
			bd = d;
			best = cur;
		}
	}
	*bp = b;
	*bdp = bd;
	return (best);
}

/*
 * A decision point (pass start, pass end, sleep): is this CPU a donor?
 * A stealable backlog of kwq_donor_us in two items or more, or a lone item
 * that, with the `ahead' items of the pass starting on `cur' in front of
 * it, waits that long.  The donor line is written at the transitions
 * only, the hint at every decision point while the CPU is a donor.
 * kw_mtx held; true if
 * the CPU just became a donor, and the caller wakes a thief once it has
 * dropped its locks (kwq_shard_wake() takes another worker's kw_mtx).
 */
bool
kwq_donor_point(struct kwq_worker *kw, struct kwq_cpu *cur, u_int ahead)
{
	struct kwq_cpu *best;
	uint64_t b, lim = (uint64_t)kwq_donor_us * 1000;
	u_int bd;
	bool d, was;

	mtx_assert(&kw->kw_mtx, MA_OWNED);
	kw->kw_dec_n++;
	was = kw->kw_starved;
	kw->kw_starved = false;
	if (kwq_bulk_place != KWQ_PLACE_SHARD) {
		if (__predict_false(kw->kw_donor)) {
			CPU_CLR_ATOMIC(kw->kw_cpu, &kwq_donors.ds_set);
			kw->kw_donor = false;
			SDT_PROBE4(kwq, , , donor, kw->kw_cpu, 0, 0, (uint64_t)0);
		}
		return (false);
	}
	best = kwq_ring_backlog(kw, cur, &b, &bd);
	kw->kw_backlog_ns = b;
	if (bd == 1 && ahead > 0 && cur != NULL)
		d = b + (uint64_t)ahead * cur->kc_cost_ns >= lim;
	else
		d = bd >= 2 && b >= lim;
	if (d)
		kwq_hint_set(kw->kw_cpu, best);
	if (d == kw->kw_donor) {
		if (__predict_false(was))	/* the starved flag went */
			SDT_PROBE4(kwq, , , donor, kw->kw_cpu, (int)d, 0, b);
		return (false);
	}
	if (d)
		CPU_SET_ATOMIC(kw->kw_cpu, &kwq_donors.ds_set);
	else
		CPU_CLR_ATOMIC(kw->kw_cpu, &kwq_donors.ds_set);
	kw->kw_donor = d;
	SDT_PROBE4(kwq, , , donor, kw->kw_cpu, (int)d, 0, b);
	if (d) {
		kw->kw_donor_n++;
		kwq_watch_arm(kw);
	}
	return (d);
}

/*
 * The cost of an item on kc, ns: an average over passes, 1/8 per pass.  A
 * pass whose CPU-time delta ks_pass_end() counts as a ticker glitch
 * (negative or over KS_GLITCH_NS, ../SCHED.md S13; a07's cpu_ticks) says
 * nothing of the cost and is left out: a negative one drove the average
 * below zero, which as a uint64_t overflowed the backlog sums.
 */
void
kwq_pass_cost(struct kwq_cpu *kc, uint64_t ns, u_int n)
{
	int64_t per, old;

	if (n == 0 || (int64_t)ns < 0 || ns > KS_GLITCH_NS)
		return;
	per = (int64_t)(ns / n);
	old = (int64_t)kc->kc_cost_ns;
	kc->kc_cost_ns = old == 0 ? (uint64_t)per : (uint64_t)(old + (per - old) / 8);
}

/*
 * One victim's steal, kwq_steal_rm read-locked: from vcpu's list that the
 * word h names (a hint, or a mailbox), the older half at most steal_max,
 * the bound, a lone item only from a starved victim or, for an idle thief,
 * behind the victim's running work.  True if a batch moved.
 */
static bool
kwq_steal_from(struct kwq_worker *kw, int vcpu, uint64_t h, int64_t room_ns,
    int how)
{
	struct kwq *q;
	struct kwq_cpu *vkc, *kc;
	struct kwq_worker *vkw;
	struct kwq_item *head, *tail;
	uint64_t cost, room;
	u_int n, take, k, lim, left;
	int slot, why;
	bool lone;

	slot = KH_SLOT(h);
	why = KWQ_SMISS_STALE;
	if (vcpu == kw->kw_cpu || slot < 0 || slot >= KWQ_STEAL_SLOTS)
		goto miss;
	q = kwq_steal_tab[slot];
	if (q == NULL || q->kwq_steal_id != KH_ID(h))
		goto miss;
	vkc = q->kwq_pcpu[vcpu];
	vkw = kwq_workers[KWQ_BULK][vcpu];
	lone = vkw->kw_starved || (room_ns < 0 && vkw->kw_cur != NULL);
	n = vkc->kc_depth;		/* unlocked: a hint */
	if (n == 0 || (n == 1 && !lone)) {
		why = n == 0 ? KWQ_SMISS_EMPTY : KWQ_SMISS_LONE;
		goto miss;
	}
	kwq_kc_lock(vkc);
	n = vkc->kc_depth;
	if (n == 0 || (n == 1 && !lone) || !q->kwq_active) {
		why = n == 0 ? KWQ_SMISS_EMPTY : n == 1 && !lone ?
		    KWQ_SMISS_LONE : KWQ_SMISS_INACTIVE;
		kwq_kc_unlock(vkc);
		goto miss;
	}
	take = vkw->kw_starved ? n : (n + 1) / 2;
	if (take > kwq_steal_max)
		take = kwq_steal_max;
	cost = (uint64_t)KH_US(h) * 1000;
	if (cost > 0) {
		room = room_ns >= 0 ? (uint64_t)room_ns : (uint64_t)kwq_donor_us * 1000;
		lim = room / cost > 2 ? (u_int)(room / cost) - 1 : 1;
		if (take > lim)
			take = lim;
	}
	head = STAILQ_FIRST(&vkc->kc_list);
	tail = head;
	for (k = 1; k < take; k++)
		tail = tail->kwi_link.stqe_next;
	KASSERT(KWQ_TAGBIT(tail->kwi_link.stqe_next) == 0,
	    ("kwq_steal_from: %s: a notifier on a stealable list", q->kwq_name));
	vkc->kc_list.stqh_first = tail->kwi_link.stqe_next;
	if (vkc->kc_list.stqh_first == NULL)
		vkc->kc_list.stqh_last = &vkc->kc_list.stqh_first;
	tail->kwi_link.stqe_next = NULL;
	vkc->kc_depth -= take;
	left = vkc->kc_depth;
	vkc->kc_steals_out++;
	vkc->kc_stolen_out += take;
	kwq_list_check(vkc, "donor steal victim");
	kwq_kc_unlock(vkc);

	kc = q->kwq_pcpu[kw->kw_cpu];
	kwq_kc_lock(kc);
	kwq_list_concat(kc, head, tail);
	kc->kc_depth += take;
	kc->kc_steals_in++;
	kc->kc_stolen_in += take;
	if (kc->kc_cost_ns == 0)
		kc->kc_cost_ns = cost;
	kwq_list_check(kc, "donor steal stealer");
	(void)kwq_kc_place(kc, take);
	kwq_kc_unlock(kc);
	SDT_PROBE6(kwq, , , steal, q, vcpu, kw->kw_cpu, (int)take, how, (int)left);
	kw->kw_steals++;
	kw->kw_stolen += take;
	return (true);
miss:
	SDT_PROBE4(kwq, , , steal__miss, kw->kw_cpu, how, vcpu, why);
	return (false);
}

/*
 * The donor steal (place=2): first the victim the mailbox names (mcpu,
 * mword; directed), then the nearest donor in reach by its hint.  No lock
 * held; true if a batch moved.
 *
 * directed=2 adds the chain: an idle thief with an empty mailbox tries the
 * victim and list of its last batch again, so the thieves of a shallow
 * list run from item to item as today's scan lets them, not one wake per
 * item (a lone item behind running work never makes its CPU a donor).  The
 * first miss forgets the victim.  The word is checked against the slot
 * table as a hint is, so a victim's queue destroyed meanwhile is a miss.
 * shard.directed_us limits the mailbox and the chain to lists whose items
 * cost at least that much (the word's cost): moving a cheap lone item
 * costs the thief about what running it saves, so those lists keep the
 * donor rule.
 */
bool
kwq_steal_donor(struct kwq_worker *kw, int64_t room_ns, int mcpu, uint64_t mword)
{
	struct rm_priotracker tr;
	uint64_t h = 0;
	u_int i, reach;
	int vcpu = -1, how;
	bool chain = false, moved = false, tried = false;

	reach = kwq_topo_reach(kw->kw_cpu);
	if (mcpu < 0 && room_ns < 0 && kwq_directed >= 2 &&
	    kw->kw_chain_cpu >= 0) {
		mcpu = kw->kw_chain_cpu;
		mword = kw->kw_chain_word;
		chain = true;
	}
	if (mcpu >= 0 && mword != 0) {
		kw->kw_steal_tries++;
		rm_rlock(&kwq_steal_rm, &tr);
		moved = kwq_steal_from(kw, mcpu, mword, room_ns,
		    chain ? KWQ_STEAL_CHAIN : KWQ_STEAL_MAILBOX);
		rm_runlock(&kwq_steal_rm, &tr);
		if (moved) {
			if (chain)
				kw->kw_chained++;
			else
				kw->kw_directed++;
			if (kwq_directed >= 2 && KH_US(mword) >= kwq_directed_us) {
				kw->kw_chain_cpu = mcpu;
				kw->kw_chain_word = mword;
			} else
				kw->kw_chain_cpu = -1;
			return (true);
		}
		if (chain)
			kw->kw_chain_cpu = -1;
	}
	how = room_ns >= 0 ? KWQ_STEAL_PULL : KWQ_STEAL_DONOR;
	if (reach == 0 || CPU_EMPTY(&kwq_donors.ds_set)) {
		SDT_PROBE4(kwq, , , steal__miss, kw->kw_cpu, how, -1, KWQ_SMISS_NONE);
		return (false);
	}
	kw->kw_steal_tries++;
	rm_rlock(&kwq_steal_rm, &tr);
	for (i = 0; i < reach && !moved; i++) {
		vcpu = kwq_topo_order[kw->kw_cpu][i];
		if (kwq_is_donor(vcpu)) {
			tried = true;
			h = atomic_load_64(&kwq_hints[vcpu].kh_word);
			moved = kwq_steal_from(kw, vcpu, h, room_ns, how);
		}
	}
	rm_runlock(&kwq_steal_rm, &tr);
	if (!tried)
		SDT_PROBE4(kwq, , , steal__miss, kw->kw_cpu, how, -1, KWQ_SMISS_NONE);
	if (moved && kwq_directed >= 2 && KH_US(h) >= kwq_directed_us) {
		kw->kw_chain_cpu = vcpu;
		kw->kw_chain_word = h;
	}
	return (moved);
}

/* The early pull: a worker whose backlog fell below kwq_pull_us. */
void
kwq_shard_pull(struct kwq_worker *kw)
{
	uint64_t l = (uint64_t)kwq_pull_us * 1000;

	if (kw->kw_backlog_ns >= l || kwq_nstealable == 0 || !kwq_steal_on)
		return;
	if (kwq_steal_donor(kw, (int64_t)(l - kw->kw_backlog_ns), -1, 0))
		kw->kw_pulls++;
}

/*
 * KWQ_CPU_ANY under place=2: the nearest CPU in topology order, within the
 * own group and kwq_shard_probe more, whose worker sleeps with this
 * queue's list empty; then one far group's line from the per-CPU cursor,
 * which stays on a group while it has such a CPU and moves on when it has
 * none; then the own CPU unless it is a donor, the nearest CPU in reach
 * that is not, and today's shallowest of the own list and four probes.
 */
/* The nearest idle CPU of the near groups with q's list empty there, or -1. */
static int
kwq_pick_near(struct kwq *q, int me, int *nnp)
{
	uint64_t w[KWQ_NEAR_MAX];
	u_int i, nord;
	int c, g, k, nn;

	nn = 1 + (int)kwq_shard_probe;
	if (nn > kwq_grp_nnear[me])
		nn = kwq_grp_nnear[me];
	*nnp = nn;
	for (k = 0; k < nn; k++)
		w[k] = atomic_load_64(&kwq_grps[kwq_grp_near[me][k]].kg_idle);
	nord = kwq_topo_ring[me][KWQ_TOPO_RINGS - 1];
	for (i = 0; i < nord; i++) {
		c = kwq_topo_order[me][i];
		g = kwq_cpu_grp[c];
		for (k = 0; k < nn && kwq_grp_near[me][k] != g; k++)
			;
		if (k == nn)
			break;		/* beyond the near groups */
		if ((w[k] & kwq_grp_bit(c)) != 0 && q->kwq_pcpu[c]->kc_depth == 0)
			return (c);
	}
	return (-1);
}

int
kwq_pick_shard(struct kwq *q, int spill)
{
	uint64_t fw;
	u_int i, reach, depth, bestdepth, gs = kwq_shard_group;
	int me, c, k, nn, best, fg, tries, b, j;

	me = curcpu;
	reach = kwq_topo_reach(me);
	if ((c = kwq_pick_near(q, me, &nn)) >= 0) {
		counter_u64_add(kwq_pk_near, 1);
		SDT_PROBE5(kwq, , , place, q, me, c, KWQ_PICK_NEAR, spill);
		return (c);
	}
	if (kwq_ngrps > nn) {
		fg = DPCPU_GET(kwq_far_g) - 1;
		if (fg < 0 || fg >= kwq_ngrps)
			fg = (kwq_cpu_grp[me] + 1 + me) % kwq_ngrps;
		for (tries = 0; tries < kwq_ngrps; tries++, fg = (fg + 1) % kwq_ngrps) {
			for (k = 0; k < nn && kwq_grp_near[me][k] != fg; k++)
				;
			if (k == nn)
				break;
		}
		if (tries < kwq_ngrps) {
			fw = atomic_load_64(&kwq_grps[fg].kg_idle);
			for (j = 0; fw != 0 && j < (int)gs; j++) {
				b = ((int)me + j) % (int)gs;
				if ((fw & (1ULL << b)) == 0)
					continue;
				c = kwq_grp_cpu[fg * KWQ_GRP_MAX + b];
				if (q->kwq_pcpu[c] != NULL && q->kwq_pcpu[c]->kc_depth == 0) {
					DPCPU_SET(kwq_far_g, fg + 1);
					counter_u64_add(kwq_pk_far, 1);
					SDT_PROBE5(kwq, , , place, q, me, c, KWQ_PICK_FAR, spill);
					return (c);
				}
			}
			DPCPU_SET(kwq_far_g, (fg + 1) % kwq_ngrps + 1);
		}
	}
	if (!kwq_is_donor(me)) {
		counter_u64_add(kwq_pk_self, 1);
		SDT_PROBE5(kwq, , , place, q, me, me, KWQ_PICK_SELF, spill);
		return (me);
	}
	for (i = 0; i < reach && i < 16; i++) {
		c = kwq_topo_order[me][i];
		if (!kwq_is_donor(c)) {
			counter_u64_add(kwq_pk_nondonor, 1);
			SDT_PROBE5(kwq, , , place, q, me, c, KWQ_PICK_NONDONOR, spill);
			return (c);
		}
	}
	best = me;
	bestdepth = q->kwq_pcpu[me]->kc_depth;
	for (i = 0; i < reach && i < 4 && bestdepth != 0; i++) {
		c = kwq_topo_order[me][i];
		depth = q->kwq_pcpu[c]->kc_depth;
		if (depth < bestdepth) {
			best = c;
			bestdepth = depth;
		}
	}
	counter_u64_add(kwq_pk_probe, 1);
	SDT_PROBE5(kwq, , , place, q, me, best, KWQ_PICK_PROBE, spill);
	return (best);
}

/*
 * Spill (place=2): an item for a fixed CPU of a stealable queue goes where
 * KWQ_CPU_ANY would when that CPU is a donor or its list holds a donor's
 * backlog.
 */
int
kwq_shard_spill(struct kwq *q, int cpu)
{
	struct kwq_cpu *kc = q->kwq_pcpu[cpu];

	if (!kwq_steal_on)
		return (cpu);
	if (kwq_is_donor(cpu) ||
	    (uint64_t)kc->kc_depth * kc->kc_cost_ns >= (uint64_t)kwq_donor_us * 1000) {
		counter_u64_add(kwq_pk_spill, 1);
		return (kwq_pick_shard(q, 1));
	}
	return (cpu);
}

/*
 * The starvation watchdog (K12): a direct callout on the worker's CPU, armed
 * while the CPU holds stealable work.  It runs in the clock interrupt,
 * so a thread that keeps the BULK worker off the CPU does not stop it;
 * a worker that reached no decision point since the watchdog was armed is
 * flagged a starved donor and a thief is woken, every period until the
 * worker runs again.
 */
static void
kwq_watch_fire(void *arg)
{
	struct kwq_worker *kw = arg;
	struct kwq_cpu *best;
	uint64_t b;
	u_int bd;
	bool wake = false;

	mtx_lock_spin(&kw->kw_mtx);
	kw->kw_watch_armed = false;
	if (kw->kw_exit || kwq_bulk_place != KWQ_PLACE_SHARD) {
		mtx_unlock_spin(&kw->kw_mtx);
		return;
	}
	best = kwq_ring_backlog(kw, kw->kw_cur, &b, &bd);
	if (best == NULL || bd == 0) {
		mtx_unlock_spin(&kw->kw_mtx);
		return;
	}
	if (kw->kw_dec_n == kw->kw_watch_n) {
		kwq_hint_set(kw->kw_cpu, best);
		if (!kw->kw_donor) {
			CPU_SET_ATOMIC(kw->kw_cpu, &kwq_donors.ds_set);
			kw->kw_donor = true;
			kw->kw_donor_n++;
		}
		if (!kw->kw_starved) {
			kw->kw_starved_n++;
			SDT_PROBE4(kwq, , , donor, kw->kw_cpu, 1, 1, b);
		}
		kw->kw_starved = true;
		wake = true;
	}
	kwq_watch_arm(kw);
	mtx_unlock_spin(&kw->kw_mtx);
	if (wake)
		(void)kwq_shard_wake(kw->kw_cpu, 0, KWQ_SWAKE_WATCHDOG);	/* the hint just set */
}

void
kwq_watch_arm(struct kwq_worker *kw)
{
	sbintime_t t;

	mtx_assert(&kw->kw_mtx, MA_OWNED);
	if (kw->kw_watch_armed || kw->kw_exit || kwq_watch_us == 0 ||
	    kwq_bulk_place != KWQ_PLACE_SHARD || kw->kw_class != KWQ_BULK)
		return;
	kw->kw_watch_armed = true;
	kw->kw_watch_n = kw->kw_dec_n;
	t = (sbintime_t)kwq_watch_us * SBT_1US;
	callout_reset_sbt_on(&kw->kw_watch_co, t, t >> 3, kwq_watch_fire, kw,
	    kw->kw_cpu, C_DIRECT_EXEC);
}

/*
 * kern.kwq.bulk.place: 0 today's code, 1 the gossip scheme (kwq_gossip.c),
 * 2 P8.  A flip wakes every sleeping BULK worker, so each puts its idle
 * bit where the new mode reads it (none under 1) and publishes its load,
 * and clears the donor line.
 */
static int
kwq_sysctl_place(SYSCTL_HANDLER_ARGS)
{
	struct kwq_worker *kw;
	u_int v = kwq_bulk_place;
	int error, cpu;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v != KWQ_PLACE_TODAY && v != KWQ_PLACE_GOSSIP && v != KWQ_PLACE_SHARD)
		return (EINVAL);
	if (v == kwq_bulk_place)
		return (0);
	kwq_bulk_place = v;
	if (kwq_workers[KWQ_BULK] == NULL)
		return (0);
	CPU_FOREACH(cpu) {
		kw = kwq_workers[KWQ_BULK][cpu];
		if (kw == NULL)
			continue;
		mtx_lock_spin(&kw->kw_mtx);
		if (kw->kw_donor || kw->kw_starved)
			SDT_PROBE4(kwq, , , donor, cpu, 0, 0, (uint64_t)0);
		if (kw->kw_donor) {
			CPU_CLR_ATOMIC(cpu, &kwq_donors.ds_set);
			kw->kw_donor = false;
		}
		kw->kw_starved = false;
		if (kw->kw_sleeping && !kw->kw_exit) {
			kw->kw_sleeping = false;
			wakeup_one(kw->kw_chan);
		}
		mtx_unlock_spin(&kw->kw_mtx);
	}
	return (0);
}

/* kern.kwq.bulk.shard.idle: each group's idle line, and the donor CPUs. */
static int
kwq_sysctl_shard_idle(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	int error, g, c;

	sbuf_new_for_sysctl(&sb, NULL, 128, req);
	for (g = 0; g < kwq_ngrps; g++)
		sbuf_printf(&sb, "%s%jx", g == 0 ? "" : " ",
		    (uintmax_t)atomic_load_64(&kwq_grps[g].kg_idle));
	sbuf_cat(&sb, " | donors");
	CPU_FOREACH(c)
		if (kwq_is_donor(c))
			sbuf_printf(&sb, " %d", c);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
kwq_sysctl_shard_knob(SYSCTL_HANDLER_ARGS)
{
	u_int *var = arg1, hi = (u_int)arg2;
	u_int val = *var;
	int error;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val > hi)
		return (EINVAL);
	*var = val;
	return (0);
}

/* kern.kwq.bulk.place, kern.kwq.bulk.shard.* (OBSERVABILITY.md). */
static void
kwq_shard_sysctl(void)
{
	struct sysctl_ctx_list *ctx = &kwq_shard_ctx;
	struct sysctl_oid_list *bulk = SYSCTL_STATIC_CHILDREN(_kern_kwq_bulk);
	struct sysctl_oid *oid;
	struct sysctl_oid_list *sh;

	sysctl_ctx_init(ctx);
	SYSCTL_ADD_PROC(ctx, bulk, OID_AUTO, "place",
	    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, NULL, 0, kwq_sysctl_place,
	    "IU", "BULK placement (SPEC-p6g.md S2.8): 0 kwq_pick_any() and the steal scan (SCHED.md S16); 1 the gossip scheme, tables on a torus of cores and the hand-off; 2 P8, the idle mask in group lines and the donor line; flippable under load; default 2 on one NUMA domain, else 0 (D12)");
	oid = SYSCTL_ADD_NODE(ctx, bulk, OID_AUTO, "shard",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "P8 (place=2): knobs and placement counters");
	sh = SYSCTL_CHILDREN(oid);
	SYSCTL_ADD_UINT(ctx, sh, OID_AUTO, "group", CTLFLAG_RD, &kwq_shard_group, 0,
	    "CPUs per idle line, in topology order (loader tunable kern.kwq.bulk.group, 1-64)");
	SYSCTL_ADD_INT(ctx, sh, OID_AUTO, "ngroups", CTLFLAG_RD, &kwq_ngrps, 0,
	    "idle lines");
	SYSCTL_ADD_PROC(ctx, sh, OID_AUTO, "idle",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, kwq_sysctl_shard_idle,
	    "A", "each group's idle line in hex (a bit per member whose BULK worker sleeps), then the donor CPUs");
	SYSCTL_ADD_PROC(ctx, sh, OID_AUTO, "probe",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, &kwq_shard_probe,
	    KWQ_NEAR_MAX - 1, kwq_sysctl_shard_knob, "IU",
	    "groups a producer reads besides its own before the far probe (0-4)");
	SYSCTL_ADD_PROC(ctx, sh, OID_AUTO, "donor_us",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, &kwq_donor_us, 1000000,
	    kwq_sysctl_shard_knob, "IU",
	    "stealable backlog that makes a CPU a donor, us");
	SYSCTL_ADD_PROC(ctx, sh, OID_AUTO, "pull_us",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, &kwq_pull_us, 1000000,
	    kwq_sysctl_shard_knob, "IU",
	    "a worker whose backlog is below this at a pass end pulls from a donor, us; 0: never");
	SYSCTL_ADD_PROC(ctx, sh, OID_AUTO, "directed",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, &kwq_directed, 2,
	    kwq_sysctl_shard_knob, "IU",
	    "1: a wake to steal names the victim and its list in the woken worker's mailbox, which it steals from first, donor or not; 2 (default): and an idle thief tries the victim of its last batch again (the chain); 0: neither");
	SYSCTL_ADD_PROC(ctx, sh, OID_AUTO, "directed_us",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, &kwq_directed_us, 0xffff,
	    kwq_sysctl_shard_knob, "IU",
	    "directed: the mailbox and the chain only for lists whose items cost at least this many us (their hint's cost; default 10); 0: every list");
	SYSCTL_ADD_PROC(ctx, sh, OID_AUTO, "watch_us",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, &kwq_watch_us, 1000000,
	    kwq_sysctl_shard_knob, "IU",
	    "the starvation watchdog's period, us; 0: off");
	SYSCTL_ADD_COUNTER_U64(ctx, sh, OID_AUTO, "pick_near", CTLFLAG_RD,
	    &kwq_pk_near, "KWQ_CPU_ANY items placed on an idle CPU of the near groups");
	SYSCTL_ADD_COUNTER_U64(ctx, sh, OID_AUTO, "pick_far", CTLFLAG_RD,
	    &kwq_pk_far, "KWQ_CPU_ANY items placed on an idle CPU of a far group (the far probe)");
	SYSCTL_ADD_COUNTER_U64(ctx, sh, OID_AUTO, "pick_self", CTLFLAG_RD,
	    &kwq_pk_self, "on the producer's CPU (no idle CPU found, not a donor)");
	SYSCTL_ADD_COUNTER_U64(ctx, sh, OID_AUTO, "pick_nondonor", CTLFLAG_RD,
	    &kwq_pk_nondonor, "on the nearest CPU that is not a donor");
	SYSCTL_ADD_COUNTER_U64(ctx, sh, OID_AUTO, "pick_probe", CTLFLAG_RD,
	    &kwq_pk_probe, "on the shallowest of the own list and four probes");
	SYSCTL_ADD_COUNTER_U64(ctx, sh, OID_AUTO, "spills", CTLFLAG_RD,
	    &kwq_pk_spill, "items for a fixed CPU placed elsewhere, that CPU being a donor or its list holding a donor's backlog");
}
