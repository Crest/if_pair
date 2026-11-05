/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq_crypto.ko - a dummy BULK client that benchmarks disk-style crypto
 * through kwq (../PLAN.txt P6d).
 *
 * Requests of `bytes' each are encrypted with AES-XTS through crypto(9)
 * the way GELI does it: one session per CPU, one crypto operation per
 * `sector', synchronous on software and accelerated software drivers
 * (aesni, armv8crypto, ossl, cryptosoft), completion by callback.  `nq'
 * queues each keep `depth' requests in flight: a request that completes is
 * submitted again at once (a closed loop), so the queues stay busy for
 * `secs' seconds.  Modes:
 *
 *   kwq      one BULK queue per client queue (kern.kwq.bulk.queue.kcq<i>),
 *            each request one item; `cpu' -1 submits with KWQ_CPU_ANY, a
 *            CPU number puts every queue on that CPU (the multiple busy
 *            queues case), or with `cpustep' queue i on cpu + i x cpustep
 *            (two queues on SMT siblings or on two cores: the SMT yield,
 *            SPEC-p6g.md P6g.0); `stealable' creates the queues
 *            KWQ_F_STEALABLE.
 *   scatter  as kwq, but a request is cut into chunks of `chunk' bytes
 *            fanned out with kwq_scatter(); its group's done() completes it.
 *   thread   the baseline: GELI's own model, kernel threads at `prio'
 *            (PUSER by default) taking requests from a list per queue;
 *            `threads' 0 is one per CPU bound to it (kern.geom.eli.threads
 *            = 0), n is n threads bound to `cpu' if it is set.
 *
 *   sysctl kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=4 ...
 *   sysctl kern.kwq_crypto.run=1
 *   sysctl kern.kwq_crypto                     # until result_state is done
 *
 * Results: throughput in MB/s (10^6 bytes), per queue and in total, over
 * the window from `warm' seconds after the start to the stop; latency
 * from submission to completion (percentiles from a log histogram with
 * four buckets per octave, so +-19 %, the maximum exact); the share each
 * queue got against its weight and Jain's index over them; the CPUs that
 * ran requests; and with `verify' set every other pass decrypts and the
 * request is checked against its pattern.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/sched.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/priority.h>
#include <machine/atomic.h>
#include <machine/stdarg.h>

#include <opencrypto/cryptodev.h>

#include "kwq.h"

MALLOC_DEFINE(M_KWQ_CRYPTO, "kwq_crypto", "kwq crypto test client");

#define	KC_MAXQ		64
#define	KC_NBUCKET	256

/* Knobs. */
static char	kc_mode[16] = "kwq";
static u_int	kc_nq = 1;
static u_int	kc_bytes = 65536;
static u_int	kc_sector = 4096;
static u_int	kc_chunk = 16384;
static u_int	kc_depth = 16;
static u_int	kc_secs = 5;
static int	kc_cpu = -1;
static u_int	kc_cpustep = 0;
static u_int	kc_stealable = 0;
static u_int	kc_threads = 0;
static int	kc_prio = PUSER;
static u_int	kc_weight0 = 1;
static u_int	kc_verify = 0;
static u_int	kc_limit = 0;

/* Results. */
static char	kc_state[16] = "idle";
static char	kc_msg[256];
static char	kc_driver[64];
static char	kc_shares[1024];
static uint64_t	kc_r_mbps_x100, kc_r_items, kc_r_bytes, kc_r_errors, kc_r_enomem,
		kc_r_rejected, kc_r_p50_us, kc_r_p99_us, kc_r_p999_us, kc_r_max_us,
		kc_r_jain, kc_r_cpus, kc_r_window_ms;

struct kc_queue;

struct kc_chunk {
	struct kwq_item	c_item;
	struct kc_req	*c_req;
	u_int		c_off, c_len;
	volatile u_int	c_pending;	/* crypto operations outstanding */
};

struct kc_req {
	struct kwq_item	r_item;		/* kwq mode: the request is the item */
	STAILQ_ENTRY(kc_req) r_tlink;	/* thread mode */
	struct kc_queue	*r_q;
	uint8_t		*r_data;
	sbintime_t	r_start;
	volatile u_int	r_pending;	/* crypto operations outstanding (not scatter) */
	volatile int	r_error;
	bool		r_decrypt;	/* verify: this pass decrypts */
	struct kwq_group r_grp;		/* scatter */
	struct kc_chunk	*r_chunks;
	u_int		r_nchunks;
	uint64_t	r_seq;		/* sector number base: distinct IVs per request */
} __aligned(CACHE_LINE_SIZE);

struct kc_thr {
	struct kc_queue	*t_q;
	crypto_session_t t_sid;
	int		t_cpu;		/* -1: unbound */
	bool		t_done;
};

struct kc_queue {
	struct kwq	*q_kwq;
	int		q_idx;
	u_int		q_weight;
	crypto_session_t *q_sids;	/* kwq modes: [mp_maxid + 1] */
	struct kc_req	*q_reqs;
	volatile uint64_t q_bytes, q_items;	/* completions in the window */
	/* thread mode */
	struct mtx	q_mtx;
	STAILQ_HEAD(, kc_req) q_list;
	struct kc_thr	*q_thr;
	u_int		q_nthr;
	bool		q_exit;
};

static struct mtx kc_mtx;
static bool	kc_running;
static struct kc_queue *kc_q;
static u_int	kc_nqueues;
static int	kc_m;			/* 0 kwq, 1 scatter, 2 thread */
static volatile u_int kc_stop;		/* no more resubmission */
static volatile u_int kc_counting;	/* completions count toward the window */
static volatile u_int kc_inflight;
static volatile uint64_t kc_errors, kc_enomem, kc_rejected;
static uint64_t	(*kc_hist)[KC_NBUCKET];	/* [mp_maxid + 1] */
static uint64_t	*kc_cpu_hits;		/* [mp_maxid + 1] */
static volatile uint64_t kc_maxlat_ns;
static uint8_t	kc_key[64];		/* AES-XTS 256: two 256-bit keys */

SYSCTL_NODE(_kern, OID_AUTO, kwq_crypto, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "kwq crypto test client");
SYSCTL_STRING(_kern_kwq_crypto, OID_AUTO, mode, CTLFLAG_RW, kc_mode,
    sizeof(kc_mode), "kwq | scatter | thread");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, nq, CTLFLAG_RW, &kc_nq, 0,
    "client queues, 1..64");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, bytes, CTLFLAG_RW, &kc_bytes, 0,
    "bytes per request, a multiple of sector, at most 1 MiB");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, sector, CTLFLAG_RW, &kc_sector, 0,
    "bytes per crypto operation (GELI: the provider's sector size)");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, chunk, CTLFLAG_RW, &kc_chunk, 0,
    "scatter: bytes per chunk, a multiple of sector");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, depth, CTLFLAG_RW, &kc_depth, 0,
    "requests in flight per queue, 1..1024");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, secs, CTLFLAG_RW, &kc_secs, 0,
    "duration of a run, 1..3600");
SYSCTL_INT(_kern_kwq_crypto, OID_AUTO, cpu, CTLFLAG_RW, &kc_cpu, 0,
    "kwq modes: -1 = KWQ_CPU_ANY, else every queue on this CPU; thread mode: the CPU the threads are bound to (-1 = per threads)");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, cpustep, CTLFLAG_RW, &kc_cpustep, 0,
    "kwq mode with cpu set: queue i on CPU cpu + i * cpustep (0 = all on cpu; 1 and 2 put two queues on SMT siblings or on two cores)");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, stealable, CTLFLAG_RW, &kc_stealable, 0,
    "kwq modes: create the queues KWQ_F_STEALABLE");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, threads, CTLFLAG_RW, &kc_threads, 0,
    "thread mode: threads per queue; 0 = one per CPU, bound to it (GELI's default)");
SYSCTL_INT(_kern_kwq_crypto, OID_AUTO, prio, CTLFLAG_RW, &kc_prio, 0,
    "thread mode: the threads' priority (GELI: PUSER = 56)");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, weight0, CTLFLAG_RW, &kc_weight0, 0,
    "kwq modes: queue 0's DRR weight, 1..8 (the others have 1)");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, verify, CTLFLAG_RW, &kc_verify, 0,
    "every other pass decrypts and checks the request against its pattern");
SYSCTL_UINT(_kern_kwq_crypto, OID_AUTO, limit, CTLFLAG_RW, &kc_limit, 0,
    "kwq modes: per-CPU item limit of the queues (0 = class default)");
SYSCTL_STRING(_kern_kwq_crypto, OID_AUTO, result_state, CTLFLAG_RD, kc_state,
    sizeof(kc_state),
    "idle | running | done | fail (done and fail once torn down)");
SYSCTL_STRING(_kern_kwq_crypto, OID_AUTO, result_msg, CTLFLAG_RD, kc_msg,
    sizeof(kc_msg), "failure description");
SYSCTL_STRING(_kern_kwq_crypto, OID_AUTO, result_driver, CTLFLAG_RD, kc_driver,
    sizeof(kc_driver), "the crypto driver the sessions landed on");
SYSCTL_STRING(_kern_kwq_crypto, OID_AUTO, result_shares, CTLFLAG_RD, kc_shares,
    sizeof(kc_shares), "MB/s per queue over the window");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_mbps_x100, CTLFLAG_RD,
    &kc_r_mbps_x100, 0, "total throughput over the window, MB/s x 100");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_items, CTLFLAG_RD, &kc_r_items, 0,
    "requests completed in the window");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_bytes, CTLFLAG_RD, &kc_r_bytes, 0,
    "bytes completed in the window");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_window_ms, CTLFLAG_RD,
    &kc_r_window_ms, 0, "length of the window, ms");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_errors, CTLFLAG_RD, &kc_r_errors, 0,
    "crypto errors and, with verify, requests that did not decrypt to their pattern");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_enomem, CTLFLAG_RD, &kc_r_enomem, 0,
    "crypto_getreq(M_NOWAIT) failures");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_rejected, CTLFLAG_RD,
    &kc_r_rejected, 0, "submissions kwq refused (ENOBUFS)");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_lat_p50_us, CTLFLAG_RD,
    &kc_r_p50_us, 0, "submission to completion, median, us");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_lat_p99_us, CTLFLAG_RD,
    &kc_r_p99_us, 0, "the same, 99th percentile");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_lat_p999_us, CTLFLAG_RD,
    &kc_r_p999_us, 0, "the same, 99.9th percentile");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_lat_max_us, CTLFLAG_RD,
    &kc_r_max_us, 0, "the same, maximum (exact)");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_jain, CTLFLAG_RD, &kc_r_jain, 0,
    "Jain's fairness index over the queues' throughput per weight, x 1000");
SYSCTL_U64(_kern_kwq_crypto, OID_AUTO, result_cpus, CTLFLAG_RD, &kc_r_cpus, 0,
    "CPUs that ran crypto for the requests");

static void
kc_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(kc_msg, sizeof(kc_msg), fmt, ap);
	va_end(ap);
	strlcpy(kc_state, "fail", sizeof(kc_state));
	printf("kwq_crypto: FAIL: %s\n", kc_msg);
}

/* Log histogram: 0..15 us exact, then four buckets per octave. */
static u_int
kc_bucket(uint64_t us)
{
	u_int lg;

	if (us < 16)
		return ((u_int)us);
	lg = flsll(us) - 1;			/* >= 4 */
	return (MIN(16 + (lg - 4) * 4 + (u_int)((us >> (lg - 2)) & 3),
	    KC_NBUCKET - 1));
}

static uint64_t
kc_bucket_us(u_int b)		/* upper bound of bucket b */
{
	u_int lg, sub;

	if (b < 16)
		return (b);
	lg = (b - 16) / 4 + 4;
	sub = (b - 16) % 4;
	return (((4ULL + sub + 1) << (lg - 2)) - 1);
}

static void
kc_pattern(struct kc_req *r, u_int bytes)
{
	u_int i;

	for (i = 0; i < bytes; i += sizeof(uint64_t))
		*(uint64_t *)(r->r_data + i) = r->r_seq * 0x9e3779b97f4a7c15ULL + i;
}

static bool
kc_pattern_ok(struct kc_req *r, u_int bytes)
{
	u_int i;

	for (i = 0; i < bytes; i += sizeof(uint64_t))
		if (*(uint64_t *)(r->r_data + i) !=
		    r->r_seq * 0x9e3779b97f4a7c15ULL + i)
			return (false);
	return (true);
}

static void kc_submit(struct kc_req *r);

/* A request is done: account it, then submit it again or retire it. */
static void
kc_complete(struct kc_req *r)
{
	struct kc_queue *kq = r->r_q;
	uint64_t ns, old;
	sbintime_t now;

	now = sbinuptime();
	ns = sbttons(now - r->r_start);
	if (r->r_error != 0)
		atomic_add_64(&kc_errors, 1);
	if (kc_verify && r->r_decrypt && !kc_pattern_ok(r, kc_bytes))
		atomic_add_64(&kc_errors, 1);
	if (kc_verify)
		r->r_decrypt = !r->r_decrypt;
	r->r_error = 0;
	if (atomic_load_acq_int(&kc_counting)) {
		atomic_add_64(&kq->q_bytes, kc_bytes);
		atomic_add_64(&kq->q_items, 1);
		atomic_add_64(&kc_hist[curcpu][kc_bucket(ns / 1000)], 1);
		old = kc_maxlat_ns;
		while (ns > old && !atomic_fcmpset_64(&kc_maxlat_ns, &old, ns))
			;
	}
	if (atomic_load_acq_int(&kc_stop)) {
		if (atomic_fetchadd_int(&kc_inflight, -1) == 1)
			wakeup(__DEVOLATILE(u_int *, &kc_inflight));
		return;
	}
	r->r_start = now;
	kc_submit(r);
}

static int
kc_crypto_done(struct cryptop *crp)
{
	struct kc_req *r;
	struct kc_chunk *c;

	if (crp->crp_etype != 0)
		atomic_add_64(&kc_errors, 1);
	if (kc_m == 1) {
		c = crp->crp_opaque;
		r = c->c_req;
		crypto_freereq(crp);
		if (atomic_fetchadd_int(&c->c_pending, -1) == 1)
			kwq_group_leave(&r->r_grp, 1);
	} else {
		r = crp->crp_opaque;
		crypto_freereq(crp);
		if (atomic_fetchadd_int(&r->r_pending, -1) == 1)
			kc_complete(r);
	}
	return (0);
}

/*
 * The crypto operations for bytes [off, off + len) of r, one per sector
 * as GELI issues them, the callback set; `opaque' is what it gets back.
 * A failed allocation or dispatch counts that sector as done with an
 * error, so the request still completes.  Returns the operations that
 * were not issued, which the caller takes off the pending count.
 */
static u_int
kc_crypt(struct kc_req *r, crypto_session_t sid, u_int off, u_int len,
    void *opaque)
{
	struct cryptop *crp;
	u_int o, failed = 0;
	uint64_t sec;

	for (o = off; o < off + len; o += kc_sector) {
		crp = crypto_getreq(sid, M_NOWAIT);
		if (crp == NULL) {
			atomic_add_64(&kc_enomem, 1);
			r->r_error = ENOMEM;
			failed++;
			continue;
		}
		crypto_use_buf(crp, r->r_data + o, kc_sector);
		crp->crp_op = r->r_decrypt ? CRYPTO_OP_DECRYPT : CRYPTO_OP_ENCRYPT;
		crp->crp_flags = CRYPTO_F_CBIFSYNC | CRYPTO_F_IV_SEPARATE;
		crp->crp_payload_start = 0;
		crp->crp_payload_length = kc_sector;
		crp->crp_callback = kc_crypto_done;
		crp->crp_opaque = opaque;
		sec = r->r_seq * (kc_bytes / kc_sector) + o / kc_sector;
		memset(crp->crp_iv, 0, sizeof(crp->crp_iv));
		le64enc(crp->crp_iv, sec);
		if (crypto_dispatch(crp) != 0) {
			crypto_freereq(crp);
			r->r_error = EIO;
			failed++;
		}
	}
	return (failed);
}

/* Run one request (or one chunk of one) on this CPU's or thread's session. */
static void
kc_run_req(struct kc_req *r, crypto_session_t sid)
{
	u_int n = kc_bytes / kc_sector, failed;

	atomic_store_rel_int(&r->r_pending, n + 1);	/* +1: ours, until all are issued */
	failed = kc_crypt(r, sid, 0, kc_bytes, r);
	if (atomic_fetchadd_int(&r->r_pending, -(failed + 1)) == failed + 1)
		kc_complete(r);
}

static void
kc_run_chunk(struct kc_chunk *c, crypto_session_t sid)
{
	u_int n = c->c_len / kc_sector, failed;

	atomic_store_rel_int(&c->c_pending, n + 1);
	failed = kc_crypt(c->c_req, sid, c->c_off, c->c_len, c);
	if (atomic_fetchadd_int(&c->c_pending, -(failed + 1)) == failed + 1)
		kwq_group_leave(&c->c_req->r_grp, 1);
}

static void
kc_scatter_done(void *arg)
{
	kc_complete(arg);
}

/*
 * The BULK handler: requests (kwq mode) or chunks (scatter mode), each run
 * to completion on this CPU's session, the rest handed back when the budget
 * is spent.  A drain of a KWQ_F_DISCARD queue never happens here (the
 * queues are drained only after the last request retired).
 */
static void
kc_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
{
	struct kc_queue *kq = ctx;
	struct kwq_item *it, *next;
	crypto_session_t sid = kq->q_sids[curcpu];
	int done = 0;

	KASSERT(n > 0, ("kwq_crypto: discard"));
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		KWQ_ITEM_INIT(it);
		atomic_add_64(&kc_cpu_hits[curcpu], 1);
		if (kc_m == 1)
			kc_run_chunk(__containerof(it, struct kc_chunk, c_item), sid);
		else
			kc_run_req(__containerof(it, struct kc_req, r_item), sid);
		done++;
		if (next != NULL && kwq_budget_left(q) == 0) {
			kwq_requeue(q, next, NULL, n - done);
			return;
		}
	}
}

static void
kc_submit(struct kc_req *r)
{
	struct kc_queue *kq = r->r_q;
	u_int i;
	int error;

	switch (kc_m) {
	case 0:
		KWQ_ITEM_INIT(&r->r_item);
		error = kwq_enqueue(kq->q_kwq, kc_cpu >= 0 ?
		    kc_cpu + kq->q_idx * (int)kc_cpustep : KWQ_CPU_ANY, &r->r_item);
		break;
	case 1:
		for (i = 0; i < r->r_nchunks; i++)
			r->r_chunks[i].c_item.kwi_link.stqe_next =
			    i + 1 < r->r_nchunks ? &r->r_chunks[i + 1].c_item : NULL;
		error = kwq_scatter(kq->q_kwq, &r->r_grp, &r->r_chunks[0].c_item,
		    r->r_nchunks, kc_scatter_done, r);
		break;
	default:
		mtx_lock(&kq->q_mtx);
		STAILQ_INSERT_TAIL(&kq->q_list, r, r_tlink);
		mtx_unlock(&kq->q_mtx);
		wakeup_one(&kq->q_list);
		return;
	}
	if (error != 0) {
		/* Refused: retire the request; the run reports the count. */
		atomic_add_64(&kc_rejected, 1);
		if (atomic_fetchadd_int(&kc_inflight, -1) == 1)
			wakeup(__DEVOLATILE(u_int *, &kc_inflight));
	}
}

/* Thread mode: GELI's worker loop, one request at a time. */
static void
kc_thread(void *arg)
{
	struct kc_thr *t = arg;
	struct kc_queue *kq = t->t_q;
	struct kc_req *r;

	thread_lock(curthread);
	sched_prio(curthread, kc_prio);
	if (t->t_cpu >= 0)
		sched_bind(curthread, t->t_cpu);
	thread_unlock(curthread);
	mtx_lock(&kq->q_mtx);
	for (;;) {
		r = STAILQ_FIRST(&kq->q_list);
		if (r == NULL) {
			if (kq->q_exit)
				break;
			msleep(&kq->q_list, &kq->q_mtx, 0, "kccw", 0);
			continue;
		}
		STAILQ_REMOVE_HEAD(&kq->q_list, r_tlink);
		mtx_unlock(&kq->q_mtx);
		atomic_add_64(&kc_cpu_hits[curcpu], 1);
		kc_run_req(r, t->t_sid);
		mtx_lock(&kq->q_mtx);
	}
	t->t_done = true;
	wakeup(t);
	mtx_unlock(&kq->q_mtx);
	kthread_exit();
}

static int
kc_newsession(crypto_session_t *sid)
{
	struct crypto_session_params csp;

	memset(&csp, 0, sizeof(csp));
	csp.csp_mode = CSP_MODE_CIPHER;
	csp.csp_cipher_alg = CRYPTO_AES_XTS;
	csp.csp_ivlen = AES_XTS_IV_LEN;
	csp.csp_cipher_klen = sizeof(kc_key);
	csp.csp_cipher_key = kc_key;
	return (crypto_newsession(sid, &csp,
	    CRYPTOCAP_F_HARDWARE | CRYPTOCAP_F_SOFTWARE));
}

static void
kc_driver_name(crypto_session_t sid)
{
	device_t dev;

	dev = crypto_find_device_byhid(crypto_ses2hid(sid));
	snprintf(kc_driver, sizeof(kc_driver), "%s%s",
	    dev != NULL ? device_get_nameunit(dev) : "?",
	    (crypto_ses2caps(sid) & CRYPTOCAP_F_ACCEL_SOFTWARE) ? " (accelerated)" : "");
}

static void
kc_teardown(void)
{
	struct kc_queue *kq;
	u_int i, j;
	int cpu;

	if (kc_q == NULL)
		return;
	for (i = 0; i < kc_nqueues; i++) {
		kq = &kc_q[i];
		if (kq->q_kwq != NULL) {
			kwq_drain(kq->q_kwq);
			kwq_destroy(kq->q_kwq);
		}
		if (kq->q_thr != NULL) {
			mtx_lock(&kq->q_mtx);
			kq->q_exit = true;
			wakeup(&kq->q_list);
			for (j = 0; j < kq->q_nthr; j++)
				while (!kq->q_thr[j].t_done)
					msleep(&kq->q_thr[j], &kq->q_mtx, 0, "kcthx", 0);
			mtx_unlock(&kq->q_mtx);
			for (j = 0; j < kq->q_nthr; j++)
				if (kq->q_thr[j].t_sid != NULL)
					crypto_freesession(kq->q_thr[j].t_sid);
			free(kq->q_thr, M_KWQ_CRYPTO);
		}
		if (kq->q_sids != NULL) {
			CPU_FOREACH(cpu)
				if (kq->q_sids[cpu] != NULL)
					crypto_freesession(kq->q_sids[cpu]);
			free(kq->q_sids, M_KWQ_CRYPTO);
		}
		if (kq->q_reqs != NULL) {
			for (j = 0; j < kc_depth; j++) {
				free(kq->q_reqs[j].r_data, M_KWQ_CRYPTO);
				free(kq->q_reqs[j].r_chunks, M_KWQ_CRYPTO);
			}
			free(kq->q_reqs, M_KWQ_CRYPTO);
		}
		mtx_destroy(&kq->q_mtx);
	}
	free(kc_q, M_KWQ_CRYPTO);
	kc_q = NULL;
	free(kc_hist, M_KWQ_CRYPTO);
	kc_hist = NULL;
	free(kc_cpu_hits, M_KWQ_CRYPTO);
	kc_cpu_hits = NULL;
}

static int
kc_setup(void)
{
	struct kwq_params p;
	struct kc_queue *kq;
	struct kc_req *r;
	char name[KWQ_NAMELEN];
	u_int i, j, k, nthr;
	int cpu, error;

	kc_q = malloc(sizeof(*kc_q) * kc_nq, M_KWQ_CRYPTO, M_WAITOK | M_ZERO);
	kc_nqueues = kc_nq;
	kc_hist = malloc(sizeof(*kc_hist) * (mp_maxid + 1), M_KWQ_CRYPTO,
	    M_WAITOK | M_ZERO);
	kc_cpu_hits = malloc(sizeof(*kc_cpu_hits) * (mp_maxid + 1), M_KWQ_CRYPTO,
	    M_WAITOK | M_ZERO);
	arc4rand(kc_key, sizeof(kc_key), 0);
	for (i = 0; i < kc_nq; i++) {
		kq = &kc_q[i];
		kq->q_idx = i;
		kq->q_weight = (i == 0) ? kc_weight0 : 1;
		mtx_init(&kq->q_mtx, "kwq_crypto q", NULL, MTX_DEF);
		STAILQ_INIT(&kq->q_list);
		kq->q_reqs = malloc(sizeof(*kq->q_reqs) * kc_depth, M_KWQ_CRYPTO,
		    M_WAITOK | M_ZERO);
		for (j = 0; j < kc_depth; j++) {
			r = &kq->q_reqs[j];
			r->r_q = kq;
			r->r_seq = (uint64_t)i * kc_depth + j;
			r->r_data = malloc(kc_bytes, M_KWQ_CRYPTO, M_WAITOK);
			kc_pattern(r, kc_bytes);
			if (kc_m == 1) {
				r->r_nchunks = howmany(kc_bytes, kc_chunk);
				r->r_chunks = malloc(sizeof(*r->r_chunks) * r->r_nchunks,
				    M_KWQ_CRYPTO, M_WAITOK | M_ZERO);
				for (k = 0; k < r->r_nchunks; k++) {
					r->r_chunks[k].c_req = r;
					r->r_chunks[k].c_off = k * kc_chunk;
					r->r_chunks[k].c_len = MIN(kc_chunk,
					    kc_bytes - k * kc_chunk);
				}
			}
		}
		if (kc_m == 2) {
			nthr = kc_threads != 0 ? kc_threads : (u_int)mp_ncpus;
			kq->q_thr = malloc(sizeof(*kq->q_thr) * nthr, M_KWQ_CRYPTO,
			    M_WAITOK | M_ZERO);
			kq->q_nthr = nthr;
			cpu = -1;
			for (j = 0; j < nthr; j++) {
				struct kc_thr *t = &kq->q_thr[j];

				t->t_q = kq;
				if (kc_threads == 0) {
					do {
						cpu = (cpu + 1) % (mp_maxid + 1);
					} while (CPU_ABSENT(cpu));
					t->t_cpu = cpu;
				} else
					t->t_cpu = kc_cpu;
				error = kc_newsession(&t->t_sid);
				if (error != 0) {
					kc_fail("crypto_newsession: %d", error);
					t->t_sid = NULL;
					kq->q_nthr = j;		/* only those are running */
					return (error);
				}
				if (j == 0 && i == 0)
					kc_driver_name(t->t_sid);
				error = kthread_add(kc_thread, t, NULL, NULL, 0, 0,
				    "kwq_crypto/%u.%u", i, j);
				if (error != 0) {
					kc_fail("kthread_add: %d", error);
					crypto_freesession(t->t_sid);
					t->t_sid = NULL;
					kq->q_nthr = j;
					return (error);
				}
			}
			continue;
		}
		kq->q_sids = malloc(sizeof(*kq->q_sids) * (mp_maxid + 1),
		    M_KWQ_CRYPTO, M_WAITOK | M_ZERO);
		CPU_FOREACH(cpu) {
			error = kc_newsession(&kq->q_sids[cpu]);
			if (error != 0) {
				kc_fail("crypto_newsession: %d", error);
				return (error);
			}
		}
		if (i == 0)
			kc_driver_name(kq->q_sids[CPU_FIRST()]);
		memset(&p, 0, sizeof(p));
		p.limit = kc_limit;
		p.weight = kq->q_weight;
		p.domain = -1;
		snprintf(name, sizeof(name), "kcq%u", i);
		kq->q_kwq = kwq_create(name, KWQ_BULK,
		    kc_stealable ? KWQ_F_STEALABLE : 0, &p, kc_handler, kq);
		if (kq->q_kwq == NULL) {
			kc_fail("kwq_create(%s) failed", name);
			return (ENOMEM);
		}
	}
	return (0);
}

static void
kc_report(sbintime_t window)
{
	struct kc_queue *kq;
	uint64_t n, tot, sum, cnt, w;
	uint64_t x, sx, sxx;
	u_int i, b;
	int cpu, len;

	kc_r_window_ms = sbttoms(window);
	if (kc_r_window_ms == 0)
		kc_r_window_ms = 1;
	tot = cnt = 0;
	sx = sxx = 0;
	len = 0;
	kc_shares[0] = '\0';
	for (i = 0; i < kc_nqueues; i++) {
		kq = &kc_q[i];
		tot += kq->q_bytes;
		cnt += kq->q_items;
		/* MB/s x 100 = bytes / ms / 10 */
		x = kq->q_bytes / kc_r_window_ms / 10;
		len += snprintf(kc_shares + len, sizeof(kc_shares) - len,
		    "%sq%u:%ju.%02ju", i ? " " : "", i, (uintmax_t)(x / 100),
		    (uintmax_t)(x % 100));
		if (len >= (int)sizeof(kc_shares))
			len = sizeof(kc_shares) - 1;
		w = kq->q_weight != 0 ? kq->q_weight : 1;
		x = kq->q_bytes / w / 1024;	/* KiB per weight */
		sx += x;
		sxx += x * x;
	}
	kc_r_bytes = tot;
	kc_r_items = cnt;
	kc_r_mbps_x100 = tot / kc_r_window_ms / 10;
	/* sx^2 x 1000 in 64 bits: scale both sums alike, the ratio stays */
	while (sx > (1ULL << 26)) {
		sx >>= 1;
		sxx >>= 2;
	}
	kc_r_jain = sxx != 0 ? sx * sx * 1000 / (kc_nqueues * sxx) : 0;
	n = 0;
	CPU_FOREACH(cpu) {
		if (kc_cpu_hits[cpu] != 0)
			kc_r_cpus++;
		for (b = 0; b < KC_NBUCKET; b++)
			n += kc_hist[cpu][b];
	}
	sum = 0;
	kc_r_p50_us = kc_r_p99_us = kc_r_p999_us = 0;
	for (b = 0; b < KC_NBUCKET && n != 0; b++) {
		CPU_FOREACH(cpu)
			sum += kc_hist[cpu][b];
		if (kc_r_p50_us == 0 && sum * 2 >= n)
			kc_r_p50_us = kc_bucket_us(b);
		if (kc_r_p99_us == 0 && sum * 100 >= n * 99)
			kc_r_p99_us = kc_bucket_us(b);
		if (kc_r_p999_us == 0 && sum * 1000 >= n * 999)
			kc_r_p999_us = kc_bucket_us(b);
	}
	kc_r_max_us = kc_maxlat_ns / 1000;
	kc_r_errors = kc_errors;
	kc_r_enomem = kc_enomem;
	kc_r_rejected = kc_rejected;
}

/* Every queue's CPU, cpu + i x cpustep for i < nq, exists. */
static bool
kc_cpus_valid(void)
{
	u_int i;
	int c;

	if (kc_cpustep > mp_maxid)
		return (false);
	for (i = 0; i < kc_nq; i++) {
		c = kc_cpu + (int)(i * kc_cpustep);
		if (c > (int)mp_maxid || CPU_ABSENT(c))
			return (false);
		if (kc_cpustep == 0)
			break;
	}
	return (true);
}

static void
kc_run(void *arg __unused)
{
	struct kc_queue *kq;
	sbintime_t t0, t1, warm;
	u_int i, j;
	int error;

	kc_msg[0] = kc_driver[0] = kc_shares[0] = '\0';
	kc_r_mbps_x100 = kc_r_items = kc_r_bytes = kc_r_errors = kc_r_enomem = 0;
	kc_r_rejected = kc_r_p50_us = kc_r_p99_us = kc_r_p999_us = kc_r_max_us = 0;
	kc_r_jain = kc_r_cpus = kc_r_window_ms = 0;
	kc_errors = kc_enomem = kc_rejected = 0;
	kc_maxlat_ns = 0;
	kc_stop = kc_counting = 0;
	kc_inflight = 0;
	if (strcmp(kc_mode, "kwq") == 0)
		kc_m = 0;
	else if (strcmp(kc_mode, "scatter") == 0)
		kc_m = 1;
	else if (strcmp(kc_mode, "thread") == 0)
		kc_m = 2;
	else {
		kc_fail("unknown mode %s", kc_mode);
		goto out;
	}
	if (kc_nq < 1 || kc_nq > KC_MAXQ || kc_depth < 1 || kc_depth > 1024 ||
	    kc_sector < 16 || kc_sector % 16 != 0 || kc_bytes < kc_sector ||
	    kc_bytes > 1048576 || kc_bytes % kc_sector != 0 ||
	    kc_chunk < kc_sector || kc_chunk % kc_sector != 0 ||
	    kc_secs < 1 || kc_secs > 3600 || kc_weight0 < 1 || kc_weight0 > 8 ||
	    (kc_cpu >= 0 && !kc_cpus_valid()) ||
	    (uint64_t)kc_nq * kc_depth * kc_bytes > 512ULL * 1024 * 1024 ||
	    kc_prio < PRI_MIN_KERN || kc_prio > PRI_MAX_TIMESHARE) {
		kc_fail("bad knobs (nq 1..64, depth 1..1024, sector a multiple of 16, "
		    "bytes and chunk multiples of sector, bytes <= 1 MiB, "
		    "cpu + (nq - 1) x cpustep a present CPU, "
		    "nq x depth x bytes <= 512 MiB, prio 40..223)");
		goto out;
	}
	error = kc_setup();
	if (error != 0)
		goto out;

	warm = (sbintime_t)MIN(SBT_1S, (sbintime_t)kc_secs * SBT_1S / 5);
	kc_inflight = kc_nq * kc_depth;
	t0 = sbinuptime();
	for (i = 0; i < kc_nqueues; i++) {
		kq = &kc_q[i];
		for (j = 0; j < kc_depth; j++) {
			kq->q_reqs[j].r_start = t0;
			kc_submit(&kq->q_reqs[j]);
		}
	}
	pause_sbt("kcwarm", warm, 0, 0);
	atomic_store_rel_int(&kc_counting, 1);
	t0 = sbinuptime();
	pause_sbt("kcrun", (sbintime_t)kc_secs * SBT_1S - warm, 0, 0);
	atomic_store_rel_int(&kc_counting, 0);
	t1 = sbinuptime();
	atomic_store_rel_int(&kc_stop, 1);
	/* Every request retires at its next completion. */
	mtx_lock(&kc_mtx);
	while (atomic_load_acq_int(&kc_inflight) != 0)
		if (msleep(__DEVOLATILE(u_int *, &kc_inflight), &kc_mtx, 0,
		    "kcstop", 30 * hz) == EWOULDBLOCK) {
			kc_fail("%u requests still in flight after 30 s", kc_inflight);
			break;
		}
	mtx_unlock(&kc_mtx);
	kc_report(t1 - t0);
	printf("kwq_crypto: %s nq %u depth %u bytes %u cpu %d: %ju.%02ju MB/s "
	    "p50 %ju p99 %ju max %ju us jain %ju cpus %ju driver %s [%s]\n",
	    kc_mode, kc_nq, kc_depth, kc_bytes, kc_cpu,
	    (uintmax_t)(kc_r_mbps_x100 / 100), (uintmax_t)(kc_r_mbps_x100 % 100),
	    (uintmax_t)kc_r_p50_us, (uintmax_t)kc_r_p99_us, (uintmax_t)kc_r_max_us,
	    (uintmax_t)kc_r_jain, (uintmax_t)kc_r_cpus, kc_driver, kc_shares);
out:
	/* The inflight wait can time out; teardown's drain then waits for the rest. */
	kc_teardown();
	/*
	 * The verdict only now: a harness that sees "done" unloads the
	 * module or starts the next run, both refused (EBUSY) until here,
	 * and it can do so within milliseconds, before the teardown has
	 * drained the queues or stopped the threads.
	 */
	mtx_lock(&kc_mtx);
	if (kc_state[0] != 'f') {
		if (kc_r_errors != 0)
			kc_fail("%ju crypto or verify errors", (uintmax_t)kc_r_errors);
		else if (kc_r_items == 0)
			kc_fail("nothing completed");
		else
			strlcpy(kc_state, "done", sizeof(kc_state));
	}
	kc_running = false;
	wakeup(&kc_running);
	mtx_unlock(&kc_mtx);
	kthread_exit();
}

static int
kc_sysctl_run(SYSCTL_HANDLER_ARGS)
{
	int val = 0, error;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL || val == 0)
		return (error);
	mtx_lock(&kc_mtx);
	if (kc_running) {
		mtx_unlock(&kc_mtx);
		return (EBUSY);
	}
	kc_running = true;
	strlcpy(kc_state, "running", sizeof(kc_state));
	mtx_unlock(&kc_mtx);
	error = kthread_add(kc_run, NULL, NULL, NULL, 0, 0, "kwq_crypto");
	if (error != 0) {
		mtx_lock(&kc_mtx);
		kc_running = false;
		mtx_unlock(&kc_mtx);
		strlcpy(kc_state, "fail", sizeof(kc_state));
	}
	return (error);
}
SYSCTL_PROC(_kern_kwq_crypto, OID_AUTO, run,
    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0, kc_sysctl_run, "I",
    "write 1 to start a run with the knobs as set");

static int
kc_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		mtx_init(&kc_mtx, "kwq_crypto", NULL, MTX_DEF);
		return (0);
	case MOD_UNLOAD:
	case MOD_QUIESCE:
		mtx_lock(&kc_mtx);
		if (kc_running) {
			mtx_unlock(&kc_mtx);
			return (EBUSY);
		}
		mtx_unlock(&kc_mtx);
		if (type == MOD_UNLOAD)
			mtx_destroy(&kc_mtx);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t kc_mod = { "kwq_crypto", kc_modevent, NULL };
DECLARE_MODULE(kwq_crypto, kc_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(kwq_crypto, 1);
MODULE_DEPEND(kwq_crypto, kwq, 1, 1, 1);
MODULE_DEPEND(kwq_crypto, crypto, 1, 1, 1);
