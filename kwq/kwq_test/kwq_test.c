/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq_test.ko - synthetic kwq clients driven by sysctl (../PLAN.txt P3;
 * the P0 subset here covers the P0 exit criteria).
 *
 *   sysctl kern.kwq_test.scenario=lifecycle   # or fifo, notify, reject,
 *                                             # discard, sleep
 *   sysctl kern.kwq_test.items=1000 kern.kwq_test.reps=1000
 *   sysctl kern.kwq_test.run=1
 *   sysctl kern.kwq_test                      # poll result_state until
 *                                             # "done" or "fail"
 *
 * A scenario runs in its own kernel thread; results land in the
 * kern.kwq_test.result_* sysctls.  The "sleep" scenario deliberately
 * sleeps inside a handler and is expected to panic an INVARIANTS kernel;
 * it refuses to run unless kern.kwq_test.allow_panic=1.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/kthread.h>
#include <sys/unistd.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/time.h>
#include <sys/sched.h>
#include <machine/atomic.h>
#include <machine/stdarg.h>

#include "kwq.h"

MALLOC_DEFINE(M_KWQ_TEST, "kwq_test", "kwq test items");

#define	TITEM_MAGIC	0x6b77712d74657374ULL	/* "kwq-test" */

struct titem {
	struct kwq_item	ti_item;
	uint64_t	ti_magic;
	uint64_t	ti_seq;		/* per-CPU sequence for the FIFO check */
	int		ti_cpu;
};

/* Knobs. */
static char	kt_scenario[32] = "lifecycle";
static u_int	kt_items = 1000;
static u_int	kt_reps = 1000;
static u_int	kt_cost_us = 0;
static u_int	kt_limit = 0;
static int	kt_cpu = -1;		/* -1: spread round robin */
static int	kt_allow_panic = 0;

/* Results. */
static char	kt_state[16] = "idle";
static char	kt_msg[128] = "";
static uint64_t	kt_r_in, kt_r_out, kt_r_discarded, kt_r_rejected, kt_r_coalesced,
		kt_r_runs, kt_r_ns, kt_r_fifo_errors;

/* Per-run state shared with handlers. */
static struct mtx kt_mtx;
static bool	kt_running;
static uint64_t	kt_handled;		/* items run */
static uint64_t	kt_discarded;		/* items handed with n < 0 */
static uint64_t	kt_nf_runs;		/* notifier handler runs */
static uint64_t	kt_nf_seen;		/* last state value the handler saw */
static uint64_t	kt_nf_state;		/* the "hardware state" the producer bumps */
static uint64_t	kt_fifo_errors;
static uint64_t	*kt_last_seq;		/* [mp_maxid + 1] */

SYSCTL_NODE(_kern, OID_AUTO, kwq_test, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "kwq test harness");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, scenario, CTLFLAG_RW, kt_scenario,
    sizeof(kt_scenario), "lifecycle | fifo | notify | reject | discard | sleep");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, items, CTLFLAG_RW, &kt_items, 0,
    "items per repetition");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, reps, CTLFLAG_RW, &kt_reps, 0,
    "repetitions (create/.../destroy cycles)");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, cost_us, CTLFLAG_RW, &kt_cost_us, 0,
    "busy time per item in the handler");
SYSCTL_UINT(_kern_kwq_test, OID_AUTO, limit, CTLFLAG_RW, &kt_limit, 0,
    "queue limit (0 = class default)");
SYSCTL_INT(_kern_kwq_test, OID_AUTO, cpu, CTLFLAG_RW, &kt_cpu, 0,
    "target CPU (-1 = round robin over all)");
SYSCTL_INT(_kern_kwq_test, OID_AUTO, allow_panic, CTLFLAG_RW, &kt_allow_panic,
    0, "allow scenarios that are expected to panic");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, result_state, CTLFLAG_RD, kt_state,
    sizeof(kt_state), "idle | running | done | fail");
SYSCTL_STRING(_kern_kwq_test, OID_AUTO, result_msg, CTLFLAG_RD, kt_msg,
    sizeof(kt_msg), "failure description");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_in, CTLFLAG_RD, &kt_r_in, 0,
    "items/signals submitted");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_out, CTLFLAG_RD, &kt_r_out, 0,
    "items run by the handler");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_discarded, CTLFLAG_RD,
    &kt_r_discarded, 0, "items handed to the handler with n < 0");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_rejected, CTLFLAG_RD,
    &kt_r_rejected, 0, "enqueues refused");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_coalesced, CTLFLAG_RD,
    &kt_r_coalesced, 0, "kwq_notify() calls that returned false");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_runs, CTLFLAG_RD, &kt_r_runs, 0,
    "notifier handler runs");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_fifo_errors, CTLFLAG_RD,
    &kt_r_fifo_errors, 0, "out-of-order deliveries seen");
SYSCTL_U64(_kern_kwq_test, OID_AUTO, result_ns, CTLFLAG_RD, &kt_r_ns, 0,
    "wall time of the run");

static void
kt_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(kt_msg, sizeof(kt_msg), fmt, ap);
	va_end(ap);
	strlcpy(kt_state, "fail", sizeof(kt_state));
	printf("kwq_test: FAIL: %s\n", kt_msg);
}

/*
 * Handlers.
 */
static void
kt_item_handler(struct kwq *q __unused, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;
	struct titem *ti;
	int cpu;
	u_int cnt;

	cpu = curcpu;
	cnt = 0;
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(it);
		KWQ_ITEM_INIT(it);
		ti = __containerof(it, struct titem, ti_item);
		if (ti->ti_magic != TITEM_MAGIC)
			panic("kwq_test: bad item %p", ti);
		if (n > 0) {
			if (ti->ti_cpu != cpu ||
			    ti->ti_seq <= kt_last_seq[cpu])
				atomic_add_64(&kt_fifo_errors, 1);
			kt_last_seq[cpu] = ti->ti_seq;
			if (kt_cost_us != 0)
				DELAY(kt_cost_us);
		}
		cnt++;
	}
	if (cnt != (u_int)(n < 0 ? -n : n))
		panic("kwq_test: handler count %u != n %d", cnt, n);
	if (n > 0)
		atomic_add_64(&kt_handled, cnt);
	else
		atomic_add_64(&kt_discarded, cnt);
}

static void
kt_sleep_handler(struct kwq *q __unused, struct kwq_item *head, int n,
    void *ctx __unused)
{
	struct kwq_item *it, *next;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(it);
		KWQ_ITEM_INIT(it);
	}
	if (n > 0)
		pause("kwqtst", 1);	/* must panic under INVARIANTS */
	atomic_add_64(&kt_handled, n < 0 ? -n : n);
}

static void
kt_notify_handler(struct kwq *q __unused, struct kwq_item *head __unused,
    int n, void *ctx __unused)
{
	if (n < 0)
		return;
	atomic_add_64(&kt_nf_runs, 1);
	/* Level semantics: read the state, not "one event". */
	atomic_store_64(&kt_nf_seen, atomic_load_64(&kt_nf_state));
}

/*
 * Scenarios.
 */
static struct kwq *
kt_create(const char *name, uint32_t flags, kwq_handler_t *fn)
{
	struct kwq_params p;

	memset(&p, 0, sizeof(p));
	p.limit = kt_limit;
	p.domain = -1;
	return (kwq_create(name, KWQ_NET, flags | KWQ_F_INACTIVE, &p, fn, NULL));
}

static int
kt_next_cpu(int *cur)
{
	int cpu;

	if (kt_cpu >= 0)
		return (kt_cpu);
	do {
		*cur = (*cur + 1) % (mp_maxid + 1);
		cpu = *cur;
	} while (CPU_ABSENT(cpu));
	return (cpu);
}

/* create/activate/enqueue/drain/destroy, kt_reps times. */
static void
kt_run_items(uint32_t flags, kwq_handler_t *fn)
{
	struct kwq *q;
	struct titem *items, *ti;
	uint64_t in, rejected, *seq;
	u_int rep, i;
	int cpu, cur, error;

	items = malloc(sizeof(*items) * kt_items, M_KWQ_TEST, M_WAITOK | M_ZERO);
	seq = malloc(sizeof(*seq) * (mp_maxid + 1), M_KWQ_TEST, M_WAITOK | M_ZERO);
	in = rejected = 0;
	cur = 0;
	for (rep = 0; rep < kt_reps; rep++) {
		q = kt_create("test", flags, fn);
		if (q == NULL) {
			kt_fail("kwq_create failed at rep %u", rep);
			break;
		}
		memset(kt_last_seq, 0, sizeof(*kt_last_seq) * (mp_maxid + 1));
		memset(seq, 0, sizeof(*seq) * (mp_maxid + 1));
		/* Enqueue before activation must be refused. */
		ti = &items[0];
		ti->ti_magic = TITEM_MAGIC;
		KWQ_ITEM_INIT(&ti->ti_item);
		if (kwq_enqueue(q, 0, &ti->ti_item) != ENXIO) {
			kt_fail("enqueue before activate not refused");
			kwq_drain(q);
			kwq_destroy(q);
			break;
		}
		kwq_activate(q);
		for (i = 0; i < kt_items; i++) {
			ti = &items[i];
			cpu = kt_next_cpu(&cur);
			ti->ti_magic = TITEM_MAGIC;
			ti->ti_cpu = cpu;
			ti->ti_seq = ++seq[cpu];
			KWQ_ITEM_INIT(&ti->ti_item);
			error = kwq_enqueue(q, cpu, &ti->ti_item);
			in++;
			if (error != 0) {
				rejected++;
				seq[cpu]--;
				if (error != ENOBUFS) {
					kt_fail("enqueue error %d", error);
					break;
				}
			}
		}
		kwq_drain(q);
		kwq_destroy(q);
		if (kt_state[0] == 'f')
			break;
		if (kt_handled + kt_discarded + rejected != in) {
			kt_fail("rep %u: in %ju != run %ju + discarded %ju + "
			    "rejected %ju", rep, (uintmax_t)in,
			    (uintmax_t)kt_handled, (uintmax_t)kt_discarded,
			    (uintmax_t)rejected);
			break;
		}
	}
	kt_r_in = in;
	kt_r_out = kt_handled;
	kt_r_discarded = kt_discarded;
	kt_r_rejected = rejected;
	kt_r_fifo_errors = kt_fifo_errors;
	if (kt_fifo_errors != 0)
		kt_fail("%ju out-of-order deliveries", (uintmax_t)kt_fifo_errors);
	free(seq, M_KWQ_TEST);
	free(items, M_KWQ_TEST);
}

/*
 * Edge to level: bump the state, notify; the handler must end up having
 * seen the final state, runs <= signals, nothing lost, nothing rejected.
 */
static void
kt_run_notify(void)
{
	struct kwq *q;
	struct kwq_notifier nf;
	uint64_t signals, queued, coalesced;
	u_int i;
	int cpu;

	q = kt_create("test", 0, kt_notify_handler);
	if (q == NULL) {
		kt_fail("kwq_create failed");
		return;
	}
	kwq_activate(q);
	cpu = kt_cpu >= 0 ? kt_cpu : (curcpu + 1) % (mp_maxid + 1);
	if (CPU_ABSENT(cpu))
		cpu = curcpu;
	kwq_notifier_init(&nf, cpu);
	signals = queued = coalesced = 0;
	for (i = 0; i < kt_items; i++) {
		atomic_add_64(&kt_nf_state, 1);
		signals++;
		if (kwq_notify(q, &nf))
			queued++;
		else
			coalesced++;
	}
	kwq_drain(q);
	kwq_destroy(q);
	kt_r_in = signals;
	kt_r_out = queued;
	kt_r_coalesced = coalesced;
	kt_r_runs = kt_nf_runs;
	if (kt_nf_seen != kt_nf_state)
		kt_fail("final state %ju not observed (saw %ju)",
		    (uintmax_t)kt_nf_state, (uintmax_t)kt_nf_seen);
	else if (kt_nf_runs > signals || kt_nf_runs < 1)
		kt_fail("runs %ju vs signals %ju", (uintmax_t)kt_nf_runs,
		    (uintmax_t)signals);
	else if (kt_nf_runs != queued)
		kt_fail("runs %ju != queued %ju", (uintmax_t)kt_nf_runs,
		    (uintmax_t)queued);
}

static void
kt_thread(void *arg __unused)
{
	struct timespec t0, t1;

	nanouptime(&t0);
	kt_handled = kt_discarded = kt_nf_runs = kt_nf_seen = kt_nf_state = 0;
	kt_fifo_errors = 0;
	kt_r_in = kt_r_out = kt_r_discarded = kt_r_rejected = kt_r_coalesced = 0;
	kt_r_runs = kt_r_fifo_errors = 0;
	kt_msg[0] = '\0';

	if (strcmp(kt_scenario, "lifecycle") == 0 ||
	    strcmp(kt_scenario, "fifo") == 0 ||
	    strcmp(kt_scenario, "reject") == 0)
		kt_run_items(0, kt_item_handler);
	else if (strcmp(kt_scenario, "discard") == 0)
		kt_run_items(KWQ_F_DISCARD, kt_item_handler);
	else if (strcmp(kt_scenario, "notify") == 0)
		kt_run_notify();
	else if (strcmp(kt_scenario, "sleep") == 0) {
		if (!kt_allow_panic)
			kt_fail("sleep scenario needs allow_panic=1");
		else
			kt_run_items(0, kt_sleep_handler);
	} else
		kt_fail("unknown scenario %s", kt_scenario);

	nanouptime(&t1);
	kt_r_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
	    (t1.tv_nsec - t0.tv_nsec);
	if (kt_state[0] != 'f')
		strlcpy(kt_state, "done", sizeof(kt_state));
	printf("kwq_test: %s %s: in %ju out %ju discarded %ju rejected %ju "
	    "coalesced %ju runs %ju fifo_errors %ju in %ju us\n", kt_scenario,
	    kt_state, (uintmax_t)kt_r_in, (uintmax_t)kt_r_out,
	    (uintmax_t)kt_r_discarded, (uintmax_t)kt_r_rejected,
	    (uintmax_t)kt_r_coalesced, (uintmax_t)kt_r_runs,
	    (uintmax_t)kt_r_fifo_errors, (uintmax_t)kt_r_ns / 1000);
	mtx_lock(&kt_mtx);
	kt_running = false;
	mtx_unlock(&kt_mtx);
	kthread_exit();
}

static int
kt_sysctl_run(SYSCTL_HANDLER_ARGS)
{
	struct thread *td;
	int error, val;

	val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val == 0)
		return (0);
	mtx_lock(&kt_mtx);
	if (kt_running) {
		mtx_unlock(&kt_mtx);
		return (EBUSY);
	}
	kt_running = true;
	strlcpy(kt_state, "running", sizeof(kt_state));
	mtx_unlock(&kt_mtx);
	error = kthread_add(kt_thread, NULL, NULL, &td, 0, 0, "kwq_test");
	if (error != 0) {
		mtx_lock(&kt_mtx);
		kt_running = false;
		strlcpy(kt_state, "idle", sizeof(kt_state));
		mtx_unlock(&kt_mtx);
	}
	return (error);
}
SYSCTL_PROC(_kern_kwq_test, OID_AUTO, run,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, kt_sysctl_run, "I",
    "write 1 to start the selected scenario");

static int
kt_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		mtx_init(&kt_mtx, "kwq_test", NULL, MTX_DEF);
		kt_last_seq = malloc(sizeof(*kt_last_seq) * (mp_maxid + 1),
		    M_KWQ_TEST, M_WAITOK | M_ZERO);
		return (0);
	case MOD_UNLOAD:
		mtx_lock(&kt_mtx);
		if (kt_running) {
			mtx_unlock(&kt_mtx);
			return (EBUSY);
		}
		mtx_unlock(&kt_mtx);
		free(kt_last_seq, M_KWQ_TEST);
		mtx_destroy(&kt_mtx);
		return (0);
	case MOD_QUIESCE:
		return (kt_running ? EBUSY : 0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t kt_mod = { "kwq_test", kt_modevent, NULL };
DECLARE_MODULE(kwq_test, kt_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(kwq_test, 1);
MODULE_DEPEND(kwq_test, kwq, 1, 1, 1);
