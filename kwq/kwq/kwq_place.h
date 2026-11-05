/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq placement by load gossip on a torus of cores (../SPEC-p6g.md S2):
 * pure functions over per-core blocks, shared by the kernel and the
 * simulator (../sim/place/placesim.c), as kwq_sched.c is shared with
 * ../sim/sim.c.
 *
 * This file and kwq_place.c make no kernel calls: no locks, no clocks, no
 * threads.  Both worlds provide <kwq_place_env.h>, which defines
 *
 *   KP_LOAD64(p), KP_STORE64(p, v)  single-copy atomic 64-bit accesses to
 *                                   the blocks (the simulator counts the
 *                                   cache-line transfers they cause)
 *   KP_TRYFLAG(c), KP_CLRFLAG(c)    take and drop a core's try-flag
 *                                   (struct kp_ctl), true if taken
 *   KP_ASSERT(e, msg)               a consistency check
 *
 * and calls the functions below with the time from its own clock.
 *
 * Nodes are cores (an SMT group, or a CPU on a machine without SMT).
 * Each core has three blocks of KP_LINE bytes (S2.2):
 *
 *   status   one word per thread, written by that thread when its load
 *            bucket changes (rises at once, falls after the hold)
 *   table    up to KP_TABLE_MAX entries for nearly idle cores this core
 *            knows, written by the core's threads under the try-flag
 *   control  the try-flag and the merge interval, the core's own
 *
 * Time is in units of 2^-20 s (about 0.95 us; sbinuptime() >> 12 in the
 * kernel), 32 bits, compared by signed difference: it wraps after 4096 s
 * and entries expire long before.  Loads are in us.
 */

#ifndef _KWQ_PLACE_H_
#define	_KWQ_PLACE_H_

#include <kwq_place_env.h>

#ifndef KP_ASSERT
#define	KP_ASSERT(e, msg)	do { } while (0)
#endif

#define	KP_LINE		128	/* KWQ_LINE: the larger of the line and the prefetch pair */
#define	KP_MAXSMT	8	/* threads per core in a status block */
#define	KP_TABLE_MAX	8	/* entries per table block */
#define	KP_NB_MAX	8	/* neighbours per core */
#define	KP_HOPS_MAX	15

typedef uint32_t kp_time_t;

static inline int32_t
kp_tdiff(kp_time_t a, kp_time_t b)
{
	return ((int32_t)(a - b));
}

/*
 * A stamp from the future is either clock skew between CPUs (within
 * KP_SKEW) or one more than 2^31 units (2048 s) old that wrapped: older
 * than any bound, so it counts as expired (S2.2).
 */
#define	KP_SKEW		64	/* about 61 us */

static inline bool
kp_older(kp_time_t now, kp_time_t t, kp_time_t bound)
{
	int32_t d = kp_tdiff(now, t);

	return (d > (int32_t)bound || d < -KP_SKEW);
}

#define	KP_US2T(us)	((kp_time_t)(((uint64_t)(us) << 20) / 1000000))
#define	KP_T2US(t)	((uint32_t)(((uint64_t)(t) * 1000000) >> 20))

/*
 * Status word: bits 0-31 the time of the write, 32-43 the load (us,
 * saturating at 4095), 44 valid, 45-63 spare.  A thread whose word was
 * never written reads as invalid and counts as unknown, not idle.
 */
#define	KP_LOAD_MAX	4095u
#define	KP_ST_VALID	(1ULL << 44)

static inline uint64_t
kp_st_make(kp_time_t t, u_int load)
{
	if (load > KP_LOAD_MAX)
		load = KP_LOAD_MAX;
	return ((uint64_t)t | ((uint64_t)load << 32) | KP_ST_VALID);
}
static inline u_int	kp_st_load(uint64_t w) { return ((u_int)(w >> 32) & 0xfff); }
static inline kp_time_t	kp_st_time(uint64_t w) { return ((kp_time_t)w); }
static inline bool	kp_st_valid(uint64_t w) { return ((w & KP_ST_VALID) != 0); }

/*
 * Table entry: bits 0-31 the time of the observation (copied, never
 * renewed, when relayed), 32-45 the core, 46-55 its least loaded
 * thread's load (us, saturating at 1023), 56-57 its idle threads
 * (saturating at 3), 58-61 the hops the entry travelled, 62 valid.
 */
#define	KP_E_LOAD_MAX	1023u
#define	KP_E_VALID	(1ULL << 62)

static inline uint64_t
kp_e_make(kp_time_t t, u_int core, u_int load, u_int idle, u_int hops)
{
	if (load > KP_E_LOAD_MAX)
		load = KP_E_LOAD_MAX;
	if (idle > 3)
		idle = 3;
	if (hops > KP_HOPS_MAX)
		hops = KP_HOPS_MAX;
	return ((uint64_t)t | ((uint64_t)(core & 0x3fff) << 32) |
	    ((uint64_t)load << 46) | ((uint64_t)idle << 56) |
	    ((uint64_t)hops << 58) | KP_E_VALID);
}
static inline kp_time_t	kp_e_time(uint64_t e) { return ((kp_time_t)e); }
static inline u_int	kp_e_core(uint64_t e) { return ((u_int)(e >> 32) & 0x3fff); }
static inline u_int	kp_e_load(uint64_t e) { return ((u_int)(e >> 46) & 0x3ff); }
static inline u_int	kp_e_idle(uint64_t e) { return ((u_int)(e >> 56) & 0x3); }
static inline u_int	kp_e_hops(uint64_t e) { return ((u_int)(e >> 58) & 0xf); }
static inline bool	kp_e_valid(uint64_t e) { return ((e & KP_E_VALID) != 0); }

/* The three blocks of a core, one line each. */
struct kp_status {
	uint64_t	ks_w[KP_MAXSMT];
	uint8_t		ks_pad[KP_LINE - KP_MAXSMT * sizeof(uint64_t)];
};

struct kp_tableblk {
	uint64_t	kt_e[KP_TABLE_MAX];
	uint8_t		kt_pad[KP_LINE - KP_TABLE_MAX * sizeof(uint64_t)];
};

struct kp_ctl {
	u_int		kx_flag;	/* try-flag: one merge or repair at a time */
	kp_time_t	kx_last;	/* time of the last merge */
	kp_time_t	kx_ival;	/* current interval, imin..imax */
	u_int		kx_quiet;	/* merges in a row that changed nothing */
	uint64_t	kx_merges, kx_changed, kx_writes, kx_repairs;
	uint8_t		kx_pad[KP_LINE - 4 * sizeof(u_int) - 4 * sizeof(uint64_t)];
};

struct kp_core {
	struct kp_status	kc_st;
	struct kp_tableblk	kc_tb;
	struct kp_ctl		kc_ctl;
};

_Static_assert(sizeof(struct kp_status) == KP_LINE, "kp_status is one line");
_Static_assert(sizeof(struct kp_tableblk) == KP_LINE, "kp_tableblk is one line");
_Static_assert(sizeof(struct kp_ctl) == KP_LINE, "kp_ctl is one line");
_Static_assert(sizeof(struct kp_core) == 3 * KP_LINE, "kp_core is three lines");

/* Static description of a core, read-only after kp_torus_build(). */
struct kp_node {
	int		kn_cpu[KP_MAXSMT];	/* its CPUs, kn_nthr of them */
	uint16_t	kn_nb[KP_NB_MAX];	/* neighbour cores, kn_nnb of them */
	uint8_t		kn_nthr, kn_nnb;
	uint16_t	kn_l3, kn_pkg;		/* group ids for kp_dist() */
	uint16_t	kn_row, kn_col;
};

struct kp_topo {
	u_int		kt_ncores, kt_rows, kt_cols;
	u_int		kt_maxcpu;		/* highest CPU id + 1 */
	struct kp_node	*kt_node;		/* [kt_ncores], topology order */
	struct kp_core	**kt_core;		/* [kt_ncores] */
	int		*kt_cpu_core;		/* [kt_maxcpu]: core, -1 absent */
	uint8_t		*kt_cpu_slot;		/* [kt_maxcpu]: thread within it */
};

/* Torus shapes for kp_torus_build(); the last two are ablations. */
#define	KP_TORUS4	0	/* R x C, four neighbours */
#define	KP_TORUS8	1	/* the same with the diagonals */
#define	KP_EXPANDER	2	/* four random neighbours per core, symmetric */

/* Parameters (S2.9), in time units and us. */
struct kp_params {
	kp_time_t	imin, imax;	/* merge interval bounds */
	kp_time_t	texp;		/* entries older than this are dropped */
	kp_time_t	hold;		/* a lower load is published after this */
	u_int		h_us;		/* hand-off and spill at or above */
	u_int		l_us;		/* hand-off to a thread at or below */
	u_int		ladv_us;	/* a core at or below is advertised */
	u_int		llocal_us;	/* the producer's own thread is good below */
	u_int		idle_us;	/* a thread below counts as idle */
	u_int		csmall_ns;	/* items cheaper prefer the near choices */
	u_int		dnear;		/* kp_dist() of a near candidate */
	u_int		table_n;	/* table entries kept, 1..KP_TABLE_MAX */
	u_int		checks;		/* candidates checked per decision */
	u_int		flags;
};

#define	KP_F_NOSMT	0x001	/* rank idle threads as "some" (ablation) */
#define	KP_F_NOCHECK	0x002	/* trust the table without a check */
#define	KP_F_NORAND	0x004	/* first of equal ranks, not a random one */
#define	KP_F_SYMHYST	0x008	/* falls published at once, like rises */
#define	KP_F_FIXIVAL	0x010	/* merge every imin, no Trickle doubling */
#define	KP_F_NOSPLIT	0x020	/* no size split: small items as large */
#define	KP_F_NOREPAIR	0x040	/* a failed check leaves the table alone */
#define	KP_F_NOSELF	0x080	/* no step 0: a resubmitting worker searches too */

void	kp_params_default(struct kp_params *p);

/* The load figure and the status word (S2.3), worker-private state. */
struct kp_load {
	u_int		kl_pub;		/* the load last published, us */
	u_int		kl_cur;		/* the load last computed */
	kp_time_t	kl_since;	/* when a lower bucket was first seen */
	bool		kl_pending;	/* a lower bucket waits out the hold */
	bool		kl_valid;	/* published at least once */
};

u_int	kp_bucket(u_int us, const struct kp_params *p);
u_int	kp_load_us(uint64_t backlog_ns, u_int share1024, u_int busy1024,
	    u_int quantum_us);
bool	kp_load_update(struct kp_load *kl, kp_time_t now, u_int load_us,
	    const struct kp_params *p, uint64_t *word);
kp_time_t kp_load_hold_left(const struct kp_load *kl, kp_time_t now,
	    const struct kp_params *p);

/* The torus (S2.1). */
int	kp_torus_build(struct kp_topo *t, u_int shape, uint32_t seed);
u_int	kp_dist(const struct kp_topo *t, u_int a, u_int b);

/* The merge (S2.4). */
struct kp_mres {
	u_int	reads;		/* lines read */
	u_int	writes;		/* words written */
	u_int	cand;		/* candidates before the filter */
	bool	changed;
};

bool	kp_merge_due(struct kp_topo *t, u_int core, kp_time_t now);
bool	kp_merge(struct kp_topo *t, u_int core, kp_time_t now,
	    const struct kp_params *p, struct kp_mres *r);

/* Placement (S2.5), spill (S2.6), hand-off (S2.7). */
typedef bool kp_ok_t(void *arg, int cpu);

/* What a decision did, for the counters and the probes. */
#define	KP_STEP_SELF		0	/* the resubmitting worker's own CPU */
#define	KP_STEP_IDLE_NEAR	1
#define	KP_STEP_SIBLING		2
#define	KP_STEP_LOCAL		3
#define	KP_STEP_TABLE		4
#define	KP_STEP_NEIGHBOURS	5
#define	KP_STEP_FALLBACK	6	/* nothing acceptable: the caller's CPU */

struct kp_pick {
	int		cpu;
	u_int		step;
	u_int		reads;		/* remote lines read */
	u_int		checks, fails;
	kp_time_t	age;		/* of the table entry used, or 0 */
	bool		expired;	/* an expired entry was used: a bug */
};

int	kp_place(struct kp_topo *t, int me, kp_time_t now, uint64_t cost_ns,
	    int self_us, const struct kp_params *p, uint32_t *rng, kp_ok_t *ok,
	    void *arg, struct kp_pick *pk);
bool	kp_spill(struct kp_topo *t, int cpu, const struct kp_params *p);
int	kp_handoff_target(struct kp_topo *t, int me, kp_time_t now,
	    const struct kp_params *p, uint32_t *rng, struct kp_pick *pk);
u_int	kp_handoff_batch(u_int n, uint64_t cost_ns, u_int max,
	    const struct kp_params *p);
bool	kp_idle_near(struct kp_topo *t, int me, kp_time_t now,
	    const struct kp_params *p);

/* xorshift32, per CPU */
static inline uint32_t
kp_rand(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x != 0 ? x : 0x9e3779b9u;
	return (*s);
}

#endif /* !_KWQ_PLACE_H_ */
