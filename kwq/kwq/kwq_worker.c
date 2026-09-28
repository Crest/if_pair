/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq(9) - workers and the pass.
 *
 * One worker thread per (class, CPU), created at module load, bound to
 * its CPU by itself once smp_started (the GELI pattern, g_eli.c), at the
 * class's fixed priority.  A worker sleeps on kw_active until a
 * producer's doorbell puts a (queue, CPU) on that list, then runs one
 * pass per entry: swap the list out under the list lock, run the handler
 * with no service lock held, re-check emptiness under the lock before
 * going IDLE (if_pair's protocol, ../NOTES.md).
 *
 * P0: FIFO over the active list, whole batch per pass, no quantum, no
 * yield.  P1 replaces this loop with the scheduler of ../SCHED.md.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/unistd.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/domainset.h>
#include <sys/epoch.h>

#include <net/vnet.h>

#include "kwq_internal.h"

struct kwq_worker **kwq_workers[KWQ_NCLASS];

static void kwq_worker_main(void *arg);

/*
 * Ring the doorbell for kc: append it to its CPU's class worker's active
 * list and wake the worker if it sleeps.  Called with kc locked; kc_mtx
 * (sleep or spin) -> kw_mtx (spin) is the lock order.
 */
void
kwq_doorbell(struct kwq_cpu *kc)
{
	struct kwq_worker *kw;

	kw = kwq_workers[kc->kc_q->kwq_class][kc->kc_cpu];
	mtx_lock_spin(&kw->kw_mtx);
	TAILQ_INSERT_TAIL(&kw->kw_active, kc, kc_active);
	if (kw->kw_sleeping) {
		kw->kw_sleeping = false;
		wakeup_one(&kw->kw_active);
	}
	mtx_unlock_spin(&kw->kw_mtx);
}

/*
 * One pass over one (queue, CPU).
 *
 * Notifiers are released one at a time: under the lock, read the next
 * pointer, drop the pending state, unlock, call.  From that moment a
 * re-notify may legally relink the notifier into the live list, which is
 * why its link must not be part of a list the handler is walking.
 * Cancelled notifiers are unlinked and skipped.  Items travel as one list.
 */
static void
kwq_pass(struct kwq_worker *kw, struct kwq_cpu *kc)
{
	struct kwq *q;
	struct kwq_item *items, *nfs, *it, *next;
	struct kwq_notifier *nf;
	struct epoch_tracker et;
	uint64_t t0;
	u_int n, nnf;
	int sign;
	bool net;

	q = kc->kc_q;
	net = (q->kwq_class == KWQ_NET);

	kwq_kc_lock(kc);
	kc->kc_state = KWQ_CPU_RUNNING;
	items = STAILQ_FIRST(&kc->kc_list);
	n = kc->kc_depth;
	STAILQ_INIT(&kc->kc_list);
	kc->kc_depth = 0;
	nfs = STAILQ_FIRST(&kc->kc_notify);
	nnf = kc->kc_nnotify;
	STAILQ_INIT(&kc->kc_notify);
	kc->kc_nnotify = 0;
	/* Discard only ever happens on a drained queue. */
	sign = (!q->kwq_active && (q->kwq_flags & KWQ_F_DISCARD)) ? -1 : 1;
	kwq_kc_unlock(kc);

	if (n > kc->kc_maxdepth)
		kc->kc_maxdepth = n;

	kw->kw_cur = kc;
	t0 = cpu_ticks();
	THREAD_NO_SLEEPING();
	if (net)
		NET_EPOCH_ENTER(et);
#ifdef VIMAGE
	if (q->kwq_flags & KWQ_F_VNET)
		CURVNET_SET_QUIET(q->kwq_vnet);
#endif

	for (it = nfs; it != NULL; it = next) {
		nf = __containerof(it, struct kwq_notifier, kn_item);
		kwq_kc_lock(kc);
		next = STAILQ_NEXT(it, kwi_link);
		KWQ_ITEM_INIT(it);
		if (nf->kn_state == KWQ_NF_PENDING)
			nf->kn_state = KWQ_NF_IDLE;
		else
			it = NULL;	/* cancelled: skip */
		kwq_kc_unlock(kc);
		if (it != NULL)
			q->kwq_fn(q, it, (int)sign, q->kwq_ctx);
	}
	if (items != NULL)
		q->kwq_fn(q, items, (int)sign * (int)n, q->kwq_ctx);

#ifdef VIMAGE
	if (q->kwq_flags & KWQ_F_VNET)
		CURVNET_RESTORE();
#endif
	if (net)
		NET_EPOCH_EXIT(et);
	THREAD_SLEEPING_OK();
	kc->kc_cycles += cpu_ticks() - t0;
	kc->kc_passes++;
	kc->kc_items += n + nnf;
	kw->kw_passes++;
	kw->kw_cur = NULL;

	/* Emptiness re-check under the lock before IDLE. */
	kwq_kc_lock(kc);
	if (!STAILQ_EMPTY(&kc->kc_list) || !STAILQ_EMPTY(&kc->kc_notify)) {
		kc->kc_state = KWQ_CPU_WAKING;
		kwq_doorbell(kc);
	} else {
		kc->kc_state = KWQ_CPU_IDLE;
	}
	if (kc->kc_waiters != 0)
		wakeup(kc);
	kwq_kc_unlock(kc);
}

static void
kwq_worker_main(void *arg)
{
	struct kwq_worker *kw = arg;
	struct kwq_cpu *kc;

#ifdef SMP
	/* Before sched_bind() to a CPU, wait for all CPUs to go on-line. */
	while (!smp_started)
		tsleep(kw, 0, "kwqsmp", hz / 4);
#endif
	thread_lock(curthread);
	sched_prio(curthread, kw->kw_pri);
	sched_bind(curthread, kw->kw_cpu);
	thread_unlock(curthread);

	mtx_lock_spin(&kw->kw_mtx);
	for (;;) {
		while (TAILQ_EMPTY(&kw->kw_active)) {
			if (kw->kw_exit)
				goto out;
			kw->kw_sleeping = true;
			msleep_spin(&kw->kw_active, &kw->kw_mtx, "kwqidle", 0);
			kw->kw_wakeups++;
		}
		kc = TAILQ_FIRST(&kw->kw_active);
		TAILQ_REMOVE(&kw->kw_active, kc, kc_active);
		mtx_unlock_spin(&kw->kw_mtx);

		kwq_pass(kw, kc);

		mtx_lock_spin(&kw->kw_mtx);
	}
out:
	/*
	 * Announce the exit under the lock, then leave through kernel text.
	 * The instructions between the unlock and kthread_exit() are the
	 * same window every module with its own threads has (g_eli.c's
	 * workers, taskqueue_terminate()); the unloader also has to get
	 * through kldunload(2), so the window has never been observed.
	 */
	kw->kw_td = NULL;
	wakeup(&kw->kw_td);
	mtx_unlock_spin(&kw->kw_mtx);
	kthread_exit();
}

#ifdef INVARIANTS
/*
 * Sleep channels hash into 256 chains by address (subr_sleepqueue.c:
 * SC_HASH, SC_SHIFT 8).  Two workers of one class on the same chain share
 * a chain lock on every doorbell.  Report collisions at load.
 */
static u_int
kwq_sc_hash(const void *wc)
{
	return ((((uintptr_t)wc >> 8) ^ (uintptr_t)wc) & 255);
}

static void
kwq_workers_check_chains(void)
{
	struct kwq_worker *a, *b;
	int cls, i, j, coll;

	coll = 0;
	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		CPU_FOREACH(i) {
			a = kwq_workers[cls][i];
			CPU_FOREACH(j) {
				if (j <= i)
					continue;
				b = kwq_workers[cls][j];
				if (kwq_sc_hash(&a->kw_active) ==
				    kwq_sc_hash(&b->kw_active))
					coll++;
			}
		}
	}
	if (coll != 0)
		printf("kwq: %d sleepqueue chain collisions between workers\n",
		    coll);
}
#endif

int
kwq_workers_start(void)
{
	struct kwq_worker *kw;
	struct thread *td;
	int cls, cpu, error;

	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		kwq_workers[cls] = malloc(sizeof(kw) * (mp_maxid + 1), M_KWQ,
		    M_WAITOK | M_ZERO);
	}
	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		CPU_FOREACH(cpu) {
			kw = malloc_domainset_aligned(sizeof(*kw), KWQ_LINE, M_KWQ,
			    DOMAINSET_PREF(pcpu_find(cpu)->pc_domain),
			    M_WAITOK | M_ZERO);
			mtx_init(&kw->kw_mtx, "kwq worker", NULL, MTX_SPIN);
			TAILQ_INIT(&kw->kw_active);
			kw->kw_class = cls;
			kw->kw_cpu = cpu;
			kw->kw_pri = kwq_class_priority(cls);
			kwq_workers[cls][cpu] = kw;
			error = kthread_add(kwq_worker_main, kw, NULL, &kw->kw_td,
			    RFSTOPPED, 0, "kwq_%s/%d", kwq_class_names[cls], cpu);
			if (error != 0) {
				printf("kwq: kthread_add(kwq_%s/%d): %d\n",
				    kwq_class_names[cls], cpu, error);
				kwq_workers_stop();
				return (error);
			}
			/*
			 * sched_add() releases the thread lock (as in
			 * _taskqueue_start_threads() and kthread_add()).
			 */
			td = kw->kw_td;
			thread_lock(td);
			sched_prio(td, kw->kw_pri);
			sched_add(td, SRQ_BORING);
		}
	}
#ifdef INVARIANTS
	kwq_workers_check_chains();
#endif
	return (0);
}

/*
 * Stop and join every worker; only called with no queues in existence,
 * so no active list can be non-empty.
 */
void
kwq_workers_stop(void)
{
	struct kwq_worker *kw;
	int cls, cpu;

	for (cls = 0; cls < KWQ_BULK + 1; cls++) {
		if (kwq_workers[cls] == NULL)
			continue;
		CPU_FOREACH(cpu) {
			kw = kwq_workers[cls][cpu];
			if (kw == NULL)
				continue;
			mtx_lock_spin(&kw->kw_mtx);
			kw->kw_exit = true;
			if (kw->kw_sleeping) {
				kw->kw_sleeping = false;
				wakeup_one(&kw->kw_active);
			}
			while (kw->kw_td != NULL)
				msleep_spin(&kw->kw_td, &kw->kw_mtx, "kwqexit",
				    0);
			mtx_unlock_spin(&kw->kw_mtx);
			mtx_destroy(&kw->kw_mtx);
			free(kw, M_KWQ);
			kwq_workers[cls][cpu] = NULL;
		}
		free(kwq_workers[cls], M_KWQ);
		kwq_workers[cls] = NULL;
	}
}
