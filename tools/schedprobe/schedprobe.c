/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * schedprobe: the cost of the scheduler operations a kwq worker would use
 * to share its CPU, measured on this machine.  Standalone, nothing here
 * depends on kwq.  A kernel thread bound to kern.schedprobe.cpu at PI_NET
 * (a timeshare-class thread, as kthread_add() makes them and as kwq's
 * workers are) runs each operation many times and reports min, median,
 * 99th percentile and maximum in nanoseconds:
 *
 *   getprio     reading curthread->td_priority
 *   setprio     thread_lock, sched_prio() to PRI_MIN_IDLE and back to PI_NET,
 *               thread_unlock: the pair a worker would pay per pass to run
 *               passes at class priority and wait at idle priority
 *   runnable    sched_runnable()
 *   callout     callout_reset_sbt_on(C_ABSOLUTE | C_DIRECT_EXEC, +1 ms) and
 *               callout_stop(): arming and disarming a period deadline
 *   yieldself   kern_yield(PRI_UNCHANGED) with nothing else runnable
 *   yieldpair   kern_yield() with a partner thread on the same CPU that
 *               yields straight back: one hand-over round trip, two switches
 *   mtxspin     mtx_lock() on a mutex the partner CPU holds for ~1 us while
 *               running: the adaptive spin a waiter pays today
 *   mtxblock    mtx_lock() on a mutex whose holder, on the partner CPU, has
 *               yielded at PRI_MIN_IDLE behind a spinner: the waiter blocks,
 *               lends priority, the holder is requeued and preempts the
 *               spinner (IPI), unlocks; the cross-CPU cost of a worker
 *               preempted while holding a lock
 *   deadline    the probe arms a callout 200 us out, yields at PRI_MIN_IDLE
 *               behind a spinner on its own CPU, and the callout raises it to
 *               PI_NET: time from the callout running to the probe resuming
 *               (the preemption), and from arming to resuming (the deadline's
 *               accuracy)
 *
 * sysctl kern.schedprobe.run=1 runs everything (results on the console),
 * kern.schedprobe.cpu picks the probe's CPU (default 1), kern.schedprobe.partner
 * the holder's CPU for the mutex tests (default cpu + 1), kern.schedprobe.n
 * the sample count for the cheap operations (default 100000; the switching
 * ones use n / 10, the mutex and deadline ones n / 100).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
#include <machine/atomic.h>
#include <machine/cpu.h>

static int sp_cpu = 1;
static int sp_partner = -1;
static int sp_n = 100000;
static volatile int sp_busy;
static uint64_t sp_hz;		/* get_cyclecount() per second, measured */

static MALLOC_DEFINE(M_SCHEDPROBE, "schedprobe", "schedprobe samples");

SYSCTL_NODE(_kern, OID_AUTO, schedprobe, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "scheduler operation costs");
SYSCTL_INT(_kern_schedprobe, OID_AUTO, cpu, CTLFLAG_RW, &sp_cpu, 0, "probe CPU");
SYSCTL_INT(_kern_schedprobe, OID_AUTO, partner, CTLFLAG_RW, &sp_partner, 0,
    "holder CPU for the mutex tests; -1 = cpu + 1");
SYSCTL_INT(_kern_schedprobe, OID_AUTO, n, CTLFLAG_RW, &sp_n, 0, "samples per cheap operation");

static int
cmp64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y ? -1 : x > y);
}

static uint64_t
ns(uint64_t cycles)
{
	return (cycles * 1000000000ULL / sp_hz);
}

static void
report(const char *name, uint64_t *s, int n)
{
	qsort(s, n, sizeof(*s), cmp64);
	printf("schedprobe: %-10s n %7d  min %6ju  median %6ju  p99 %7ju  max %8ju ns\n",
	    name, n, (uintmax_t)ns(s[0]), (uintmax_t)ns(s[n / 2]),
	    (uintmax_t)ns(s[n * 99 / 100]), (uintmax_t)ns(s[n - 1]));
}

static void
calibrate(void)
{
	sbintime_t s0;
	uint64_t c0, c1;

	s0 = sbinuptime();
	c0 = get_cyclecount();
	while (sbinuptime() - s0 < SBT_1MS * 200)
		;
	c1 = get_cyclecount();
	sp_hz = (c1 - c0) * 5;
}

/* Partner threads. */
static struct mtx sp_mtx;
static volatile int sp_stop;		/* partners exit when set */
static volatile int sp_mode;		/* 0 idle, 1 yieldpair, 2 mtx hold running, 3 mtx hold yielding */
static volatile int sp_spinner_run;	/* spinner spins while set */
static struct thread *sp_td;		/* the probe thread */

static void
sp_spinner(void *arg)
{
	int cpu = (int)(intptr_t)arg;

	thread_lock(curthread);
	sched_prio(curthread, PUSER);
	sched_bind(curthread, cpu);
	thread_unlock(curthread);
	while (!sp_stop) {
		if (sp_spinner_run)
			cpu_spinwait();
		else
			pause_sbt("spsp", SBT_1MS, 0, 0);
	}
	kthread_exit();
}

static void
sp_partner_fn(void *arg)
{
	int cpu = (int)(intptr_t)arg;

	thread_lock(curthread);
	sched_prio(curthread, PI_NET);
	sched_bind(curthread, cpu);
	thread_unlock(curthread);
	while (!sp_stop) {
		switch (sp_mode) {
		case 1:	/* yield pair: hand straight back */
			kern_yield(PRI_UNCHANGED);
			break;
		case 2:	/* hold ~1 us, running */
			mtx_lock(&sp_mtx);
			DELAY(1);
			mtx_unlock(&sp_mtx);
			DELAY(1);
			break;
		case 3:	/* hold, and leave the CPU at idle priority behind the spinner */
			mtx_lock(&sp_mtx);
			kern_yield(PRI_MIN_IDLE);
			thread_lock(curthread);
			sched_prio(curthread, PI_NET);
			thread_unlock(curthread);
			mtx_unlock(&sp_mtx);
			DELAY(2);
			break;
		default:
			pause_sbt("sppa", SBT_1MS, 0, 0);
			break;
		}
	}
	kthread_exit();
}

/* The deadline test's callout: stamp and raise the probe thread. */
static struct callout sp_co;
static volatile uint64_t sp_fire;

static void
sp_deadline(void *arg __unused)
{
	sp_fire = get_cyclecount();
	thread_lock(sp_td);
	sched_prio(sp_td, PI_NET);
	thread_unlock(sp_td);
}

static void
sp_run(void *arg __unused)
{
	struct thread *td = curthread, *ptd = NULL, *std = NULL, *std2 = NULL;
	uint64_t *s, t0, t1, c, c2;
	int i, n, n10, n100, partner;
	volatile int pri;

	n = sp_n; n10 = n / 10; n100 = n / 100;
	partner = sp_partner >= 0 ? sp_partner : sp_cpu + 1;
	if (partner > (int)mp_maxid || CPU_ABSENT(partner))
		partner = sp_cpu;
	s = malloc(sizeof(*s) * n, M_SCHEDPROBE, M_WAITOK);
	sp_td = td;
	thread_lock(td);
	sched_prio(td, PI_NET);
	sched_bind(td, sp_cpu);
	thread_unlock(td);
	calibrate();
	printf("schedprobe: cpu %d partner %d, %ju cycles/s, hz %d, PI_NET %d PRI_MIN_IDLE %d\n",
	    sp_cpu, partner, (uintmax_t)sp_hz, hz, PI_NET, PRI_MIN_IDLE);

	/* getprio */
	for (i = 0; i < n; i++) {
		t0 = get_cyclecount();
		pri = td->td_priority;
		t1 = get_cyclecount();
		s[i] = t1 - t0;
	}
	report("getprio", s, n);
	(void)pri;

	/* setprio: down to idle and back */
	for (i = 0; i < n; i++) {
		t0 = get_cyclecount();
		thread_lock(td);
		sched_prio(td, PRI_MIN_IDLE);
		thread_unlock(td);
		thread_lock(td);
		sched_prio(td, PI_NET);
		thread_unlock(td);
		t1 = get_cyclecount();
		s[i] = t1 - t0;
	}
	report("setprio", s, n);

	/* runnable */
	for (i = 0; i < n; i++) {
		t0 = get_cyclecount();
		pri = sched_runnable();
		t1 = get_cyclecount();
		s[i] = t1 - t0;
	}
	report("runnable", s, n);

	/* callout arm + disarm */
	callout_init(&sp_co, 1);
	for (i = 0; i < n10; i++) {
		t0 = get_cyclecount();
		callout_reset_sbt_on(&sp_co, sbinuptime() + SBT_1MS, 0,
		    sp_deadline, NULL, sp_cpu, C_ABSOLUTE | C_DIRECT_EXEC);
		callout_stop(&sp_co);
		t1 = get_cyclecount();
		s[i] = t1 - t0;
	}
	report("callout", s, n10);

	/* yieldself */
	for (i = 0; i < n10; i++) {
		t0 = get_cyclecount();
		kern_yield(PRI_UNCHANGED);
		t1 = get_cyclecount();
		s[i] = t1 - t0;
	}
	report("yieldself", s, n10);

	/* partner on the same CPU for yieldpair */
	sp_mode = 0; sp_stop = 0;
	if (kthread_add(sp_partner_fn, (void *)(intptr_t)sp_cpu, NULL, &ptd, 0, 0,
	    "schedprobe_pair") == 0) {
		sp_mode = 1;
		pause_sbt("spw", SBT_1MS * 10, 0, 0);
		for (i = 0; i < n10; i++) {
			t0 = get_cyclecount();
			kern_yield(PRI_UNCHANGED);
			t1 = get_cyclecount();
			s[i] = t1 - t0;
		}
		report("yieldpair", s, n10);
		sp_mode = 0;
		sp_stop = 1;
		pause_sbt("spw", SBT_1MS * 20, 0, 0);
		sp_stop = 0;
		ptd = NULL;
	} else
		printf("schedprobe: no partner thread for yieldpair\n");

	/* mutex tests: holder on the partner CPU, spinner there too */
	mtx_init(&sp_mtx, "schedprobe", NULL, MTX_DEF);
	if (partner != sp_cpu &&
	    kthread_add(sp_partner_fn, (void *)(intptr_t)partner, NULL, &ptd, 0, 0,
	    "schedprobe_hold") == 0 &&
	    kthread_add(sp_spinner, (void *)(intptr_t)partner, NULL, &std, 0, 0,
	    "schedprobe_spin") == 0) {
		sp_mode = 2;
		pause_sbt("spw", SBT_1MS * 10, 0, 0);
		for (i = 0; i < n100; i++) {
			t0 = get_cyclecount();
			mtx_lock(&sp_mtx);
			t1 = get_cyclecount();
			mtx_unlock(&sp_mtx);
			s[i] = t1 - t0;
			DELAY(3);
		}
		report("mtxspin", s, n100);
		sp_mode = 3; sp_spinner_run = 1;
		pause_sbt("spw", SBT_1MS * 10, 0, 0);
		for (i = 0; i < n100; i++) {
			t0 = get_cyclecount();
			mtx_lock(&sp_mtx);
			t1 = get_cyclecount();
			mtx_unlock(&sp_mtx);
			s[i] = t1 - t0;
			DELAY(5);
		}
		report("mtxblock", s, n100);
		sp_mode = 0; sp_spinner_run = 0;
	} else
		printf("schedprobe: mutex tests skipped (no partner CPU or thread)\n");

	/* deadline: spinner on our own CPU, we yield at idle, the callout raises us */
	sp_stop = 0;
	if (kthread_add(sp_spinner, (void *)(intptr_t)sp_cpu, NULL, &std2, 0, 0,
	    "schedprobe_spin2") == 0) {
		uint64_t *s2 = malloc(sizeof(*s2) * n100, M_SCHEDPROBE, M_WAITOK);

		pause_sbt("spw", SBT_1MS * 10, 0, 0);
		sp_spinner_run = 1;
		for (i = 0; i < n100; i++) {
			c = get_cyclecount();
			callout_reset_sbt_on(&sp_co, sbinuptime() + 200 * SBT_1US, 0,
			    sp_deadline, NULL, sp_cpu, C_ABSOLUTE | C_DIRECT_EXEC);
			kern_yield(PRI_MIN_IDLE);
			c2 = get_cyclecount();
			thread_lock(td);
			sched_prio(td, PI_NET);
			thread_unlock(td);
			s[i] = c2 - sp_fire;	/* callout to resume */
			s2[i] = c2 - c;		/* arm to resume (200 us asked) */
		}
		sp_spinner_run = 0;
		report("dl-resume", s, n100);
		report("dl-total", s2, n100);
		free(s2, M_SCHEDPROBE);
	} else
		printf("schedprobe: deadline test skipped (no spinner thread)\n");

	sp_stop = 1;
	pause_sbt("spw", SBT_1MS * 50, 0, 0);
	callout_drain(&sp_co);
	mtx_destroy(&sp_mtx);
	free(s, M_SCHEDPROBE);
	printf("schedprobe: done\n");
	sp_busy = 0;
	kthread_exit();
}

static int
sp_sysctl_run(SYSCTL_HANDLER_ARGS)
{
	int v = 0, error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v == 0)
		return (0);
	if (sp_cpu < 0 || sp_cpu > (int)mp_maxid || CPU_ABSENT(sp_cpu))
		return (EINVAL);
	if (atomic_cmpset_int((volatile u_int *)&sp_busy, 0, 1) == 0)
		return (EBUSY);
	return (kthread_add(sp_run, NULL, NULL, NULL, 0, 0, "schedprobe"));
}
SYSCTL_PROC(_kern_schedprobe, OID_AUTO, run, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
    NULL, 0, sp_sysctl_run, "I", "write 1 to run");

static int
sp_modevent(module_t mod __unused, int what, void *arg __unused)
{
	switch (what) {
	case MOD_LOAD:
		return (0);
	case MOD_UNLOAD:
		return (sp_busy ? EBUSY : 0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t sp_mod = { "schedprobe", sp_modevent, NULL };
DECLARE_MODULE(schedprobe, sp_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(schedprobe, 1);
