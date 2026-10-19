/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * clockprobe: what clocks does this machine have and how do they behave?
 *
 * For each counter the architecture exposes (arm64: CNTVCT_EL0 bare and
 * behind an isb, CNTPCT_EL0, CNTVCTSS_EL0 where FEAT_ECV exists; amd64:
 * rdtsc, rdtsc behind lfence, rdtscp) and for the kernel's views of them
 * (get_cyclecount(), cpu_ticks(), the timecounter's 32-bit raw read,
 * sbinuptime()), measured on one CPU with interrupts off:
 *
 *   cost        ns per read, warm CPU (after a spin) and fresh from idle
 *   resolution  the smallest non-zero step between consecutive reads
 *   monotonic   backward steps between consecutive reads, how many and how
 *               large, and how many would be taken for a 32-bit wrap by
 *               the kernel's tc_cpu_ticks() extension (any decrease)
 *   jitter      the distribution of steps between consecutive reads
 *   frequency   ticks per second against sbinuptime(), and the nominal rate
 *   cross-CPU   two CPUs passing a reading back and forth: a reader that
 *               sees the other CPU's value and then reads less than it has
 *               observed the counters out of order by that much
 *
 *   with interrupts on   the same scan for intr_secs seconds (default 20) on
 *               the first clock and on cpu_ticks(), counting forward jumps
 *               of a second or more: the kernel's 32-bit extension can
 *               misfire when an interrupt reads the ticker inside it
 *   extension lag  cpu_ticks() minus the counter on every CPU, in periods of
 *               2^32 ticks relative to cpu 0: wraps lost while idle
 *
 * then the monotonic scan on every CPU.  Results go to the console as
 * "clockprobe:" lines.  sysctl kern.clockprobe.run=1 starts it in a kernel
 * thread; kern.clockprobe.reads sets the reads per monotonic scan (default
 * 2000000), kern.clockprobe.cpu the CPU for the single-CPU tests (default
 * 1), kern.clockprobe.partner the second CPU of the cross-CPU test
 * (default: the last CPU).
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
#include <sys/timetc.h>
#include <sys/malloc.h>
#include <machine/atomic.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#if defined(__aarch64__)
#include <machine/armreg.h>
#endif

static u_int cp_reads = 2000000;
static int cp_cpu = 1;
static int cp_partner = -1;
static int cp_intr_secs = 20;
static volatile int cp_running;

SYSCTL_NODE(_kern, OID_AUTO, clockprobe, CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "clock probe");
SYSCTL_UINT(_kern_clockprobe, OID_AUTO, reads, CTLFLAG_RW, &cp_reads, 0, "reads per monotonic scan");
SYSCTL_INT(_kern_clockprobe, OID_AUTO, cpu, CTLFLAG_RW, &cp_cpu, 0, "CPU for the single-CPU tests");
SYSCTL_INT(_kern_clockprobe, OID_AUTO, partner, CTLFLAG_RW, &cp_partner, 0, "second CPU of the cross-CPU test (-1: the last)");
SYSCTL_INT(_kern_clockprobe, OID_AUTO, intr_secs, CTLFLAG_RW, &cp_intr_secs, 0, "seconds of the scan with interrupts enabled");

/* ---------------------------------------------------------------- clocks */

struct cp_clock {
	const char	*name;
	uint64_t	(*read)(void);
	uint64_t	hz;		/* nominal rate, 0 if unknown; sbinuptime: 2^32 */
	bool		is32;		/* the timecounter's raw 32-bit view */
};

static uint64_t cp_read_cyclecount(void) { return (get_cyclecount()); }
static uint64_t cp_read_cpu_ticks(void) { return (cpu_ticks()); }
static uint64_t cp_read_sbinuptime(void) { return ((uint64_t)sbinuptime()); }
static uint64_t
cp_read_tc_raw(void)
{
	struct timecounter *tc = timecounter;

	return (tc->tc_get_timecount(tc) & tc->tc_counter_mask);
}

#if defined(__aarch64__)
static uint64_t cp_read_cntvct(void) { return (READ_SPECIALREG(cntvct_el0)); }
static uint64_t cp_read_cntvct_isb(void) { isb(); return (READ_SPECIALREG(cntvct_el0)); }
static uint64_t cp_read_cntpct(void) { isb(); return (READ_SPECIALREG(cntpct_el0)); }
static uint64_t cp_read_cntvctss(void) { return (READ_SPECIALREG(CNTVCTSS_EL0_REG)); }
static bool
cp_have_ecv(void)
{
	uint64_t v;

	return (get_kernel_reg(ID_AA64MMFR0_EL1, &v) &&
	    ID_AA64MMFR0_ECV_VAL(v) >= ID_AA64MMFR0_ECV_IMPL);
}
#elif defined(__amd64__)
static uint64_t cp_read_rdtsc(void) { return (rdtsc()); }
static uint64_t cp_read_rdtsc_lfence(void) { return (rdtsc_ordered_lfence()); }
static uint64_t cp_read_rdtscp(void) { return (rdtscp()); }
#endif

#define	CP_MAXCLOCKS	10
static struct cp_clock cp_clocks[CP_MAXCLOCKS];
static int cp_nclocks;

static void
cp_add(const char *name, uint64_t (*read)(void), uint64_t hz, bool is32)
{
	if (cp_nclocks < CP_MAXCLOCKS) {
		cp_clocks[cp_nclocks].name = name;
		cp_clocks[cp_nclocks].read = read;
		cp_clocks[cp_nclocks].hz = hz;
		cp_clocks[cp_nclocks].is32 = is32;
		cp_nclocks++;
	}
}

static void
cp_setup_clocks(void)
{
	struct timecounter *tc = timecounter;

	cp_nclocks = 0;
#if defined(__aarch64__)
	cp_add("cntvct_el0 (bare mrs)", cp_read_cntvct, READ_SPECIALREG(cntfrq_el0), false);
	cp_add("cntvct_el0 (isb; mrs)", cp_read_cntvct_isb, READ_SPECIALREG(cntfrq_el0), false);
	cp_add("cntpct_el0 (isb; mrs)", cp_read_cntpct, READ_SPECIALREG(cntfrq_el0), false);
	if (cp_have_ecv())
		cp_add("cntvctss_el0 (ECV)", cp_read_cntvctss, READ_SPECIALREG(cntfrq_el0), false);
	else
		printf("clockprobe: no FEAT_ECV: CNTVCTSS_EL0 not available\n");
#elif defined(__amd64__)
	cp_add("rdtsc", cp_read_rdtsc, cpu_tickrate(), false);
	cp_add("lfence; rdtsc", cp_read_rdtsc_lfence, cpu_tickrate(), false);
	cp_add("rdtscp", cp_read_rdtscp, cpu_tickrate(), false);
#endif
	cp_add("get_cyclecount()", cp_read_cyclecount, 0, false);
	cp_add("cpu_ticks()", cp_read_cpu_ticks, cpu_tickrate(), false);
	cp_add("timecounter raw 32-bit", cp_read_tc_raw, tc->tc_frequency, true);
	cp_add("sbinuptime()", cp_read_sbinuptime, 1ULL << 32, false);
}

/* ---------------------------------------------------------------- helpers */

static uint64_t cp_ref_hz;	/* get_cyclecount() rate, measured */

static void
cp_bind(int cpu)
{
	thread_lock(curthread);
	sched_bind(curthread, cpu);
	thread_unlock(curthread);
}

static void
cp_unbind(void)
{
	thread_lock(curthread);
	sched_unbind(curthread);
	thread_unlock(curthread);
}

static void
cp_spin(int ms)
{
	sbintime_t s0 = sbinuptime();

	while (sbinuptime() - s0 < SBT_1MS * ms)
		cpu_spinwait();
}

static uint64_t
cp_cyc2ns(uint64_t cyc)
{
	return (cyc * 1000000000ULL / cp_ref_hz);
}

/* ns per read: best of 20 batches of 200 reads, interrupts off. */
static uint64_t
cp_cost(struct cp_clock *c)
{
	uint64_t best = ~0ULL, c0, c1;
	volatile uint64_t sink = 0;
	register_t s;
	int b, i;

	for (b = 0; b < 20; b++) {
		s = intr_disable();
		c0 = get_cyclecount();
		for (i = 0; i < 200; i++)
			sink += c->read();
		c1 = get_cyclecount();
		intr_restore(s);
		if (c1 - c0 < best)
			best = c1 - c0;
	}
	return (cp_cyc2ns(best) / 200);
}

/* ns per read of the first 100 reads after the CPU slept 50 ms. */
static uint64_t
cp_cost_cold(struct cp_clock *c)
{
	uint64_t c0, c1;
	volatile uint64_t sink = 0;
	int i;

	pause("cpcold", hz / 20);
	c0 = get_cyclecount();
	for (i = 0; i < 100; i++)
		sink += c->read();
	c1 = get_cyclecount();
	return (cp_cyc2ns(c1 - c0) / 100);
}

/*
 * Consecutive reads with interrupts off, in chunks of 100000 so the CPU
 * keeps taking interrupts between chunks.  Steps are counted in log2
 * buckets; a decrease is a backward step (for the 32-bit view a decrease
 * larger than half the range is a genuine wrap).
 */
struct cp_scan {
	uint64_t	reads, zero, back, back_total, back_max, wraps, max_step, min_step;
	uint64_t	hist[64];	/* steps by log2 */
	uint64_t	span;		/* last - first, forward */
	uint64_t	first, last;
};

static void
cp_scan(struct cp_clock *c, u_int n, struct cp_scan *r)
{
	uint64_t prev, cur, d, chunk, half = c->is32 ? 0x80000000ULL : 0;
	register_t s;
	u_int i, k;
	int lg;

	memset(r, 0, sizeof(*r));
	r->min_step = ~0ULL;
	prev = c->read();
	r->first = prev;
	for (k = 0; k < n; k += chunk) {
		chunk = n - k < 100000 ? n - k : 100000;
		s = intr_disable();
		for (i = 0; i < chunk; i++) {
			cur = c->read();
			if (cur >= prev) {
				d = cur - prev;
				if (d == 0)
					r->zero++;
				else {
					if (d < r->min_step) r->min_step = d;
					if (d > r->max_step) r->max_step = d;
					for (lg = 0; (d >> (lg + 1)) != 0 && lg < 63; lg++) ;
					r->hist[lg]++;
				}
			} else {
				d = prev - cur;
				if (half != 0 && d >= half) {
					r->wraps++;
				} else {
					r->back++;
					r->back_total += d;
					if (d > r->back_max) r->back_max = d;
				}
			}
			prev = cur;
		}
		intr_restore(s);
		r->reads += chunk;
	}
	r->last = prev;
}

static void
cp_report_scan(const char *what, struct cp_clock *c, struct cp_scan *r)
{
	char hist[256];
	int lg, n = 0;

	hist[0] = '\0';
	for (lg = 0; lg < 64 && n < (int)sizeof(hist) - 24; lg++)
		if (r->hist[lg] != 0)
			n += snprintf(hist + n, sizeof(hist) - n, " [%d-%d]:%ju",
			    1 << lg, (2 << lg) - 1, (uintmax_t)r->hist[lg]);
	printf("clockprobe: %s %s: %ju reads, same %ju, backward %ju (max %ju ticks, total %ju)%s%ju%s, min step %ju max step %ju ticks\n",
	    what, c->name, (uintmax_t)r->reads, (uintmax_t)r->zero, (uintmax_t)r->back,
	    (uintmax_t)r->back_max, (uintmax_t)r->back_total,
	    c->is32 ? ", wraps " : ", false wraps the kernel's 32-bit extension would see: ",
	    (uintmax_t)(c->is32 ? r->wraps : r->back), c->is32 ? "" : "",
	    (uintmax_t)(r->min_step == ~0ULL ? 0 : r->min_step), (uintmax_t)r->max_step);
	if (hist[0] != '\0')
		printf("clockprobe:   step histogram (ticks):%s\n", hist);
}

/*
 * The same scan with interrupts enabled, for SECS seconds: tc_cpu_ticks()
 * runs in a critical section but not with interrupts off, so an interrupt
 * that reads the ticker between a thread's read and its store of "last"
 * makes the thread see a decrease and add a period.  Jumps of a second or
 * more forward are counted as such; a jump is reported with the value the
 * kernel's ticker had just before.
 */
static void
cp_scan_intr(struct cp_clock *c, int secs)
{
	uint64_t prev, cur, reads = 0, back = 0, back_max = 0, jumps = 0, jump_max = 0, same = 0;
	uint64_t sec = c->hz ? c->hz : cp_ref_hz;
	sbintime_t s0 = sbinuptime();

	prev = c->read();
	while (sbinuptime() - s0 < (sbintime_t)secs << 32) {
		cur = c->read();
		reads++;
		if (cur == prev)
			same++;
		else if (cur < prev) {
			back++;
			if (prev - cur > back_max) back_max = prev - cur;
		} else if (cur - prev >= sec) {
			jumps++;
			if (cur - prev > jump_max) jump_max = cur - prev;
			printf("clockprobe:   %s jumped forward by %ju ticks (%ju s) at read %ju\n",
			    c->name, (uintmax_t)(cur - prev), (uintmax_t)((cur - prev) / sec), (uintmax_t)reads);
		}
		prev = cur;
	}
	printf("clockprobe: scan with interrupts on, %d s, %s: %ju reads, same %ju, backward %ju (max %ju ticks), jumps of a second or more %ju (max %ju ticks)\n",
	    secs, c->name, (uintmax_t)reads, (uintmax_t)same, (uintmax_t)back, (uintmax_t)back_max,
	    (uintmax_t)jumps, (uintmax_t)jump_max);
}

/*
 * cpu_ticks() against the counter on every CPU: both read back to back on
 * the CPU itself, so the difference is that CPU's extension base, i.e.
 * how many 2^32 periods it has lost (idle for over a period with no read:
 * the wrap goes unseen) or gained (a false wrap).  Reported relative to
 * the first CPU, in periods.
 */
static void
cp_ticks_lag(void)
{
	uint64_t t, c, ref = 0, off, mask = timecounter->tc_counter_mask;
	int64_t periods;
	int cpu, n = 0, lagging = 0, ahead = 0;
	register_t s;
	char line[200];
	int len = 0;

	if (mask != 0xffffffffULL)
		return;
	line[0] = '\0';
	CPU_FOREACH(cpu) {
		cp_bind(cpu);
		s = intr_disable();
		c = get_cyclecount();
		t = cpu_ticks();
		intr_restore(s);
		cp_unbind();
		off = t - c;
		if (n == 0)
			ref = off;
		periods = ((int64_t)off - (int64_t)ref) / (int64_t)(1ULL << 32);
		if (periods < 0) lagging++;
		if (periods > 0) ahead++;
		if (periods != 0 && len < (int)sizeof(line) - 16)
			len += snprintf(line + len, sizeof(line) - len, " %d:%jd", cpu, (intmax_t)periods);
		n++;
	}
	printf("clockprobe: cpu_ticks() extension base relative to cpu 0, in periods of 2^32 ticks (%ju s): %d CPUs behind, %d ahead%s%s\n",
	    (uintmax_t)((1ULL << 32) / (cp_clocks[0].hz ? cp_clocks[0].hz : 1)), lagging, ahead,
	    line[0] ? "; cpu:periods" : "", line);
}

/* Frequency against sbinuptime() over about a second. */
static void
cp_frequency(struct cp_clock *c)
{
	sbintime_t s0, s1;
	uint64_t c0, c1, dt_ns, meas;
	int64_t ppm;

	s0 = sbinuptime(); c0 = c->read();
	pause("cpfreq", hz);
	c1 = c->read(); s1 = sbinuptime();
	dt_ns = ((uint64_t)(s1 - s0) * 1000000000ULL) >> 32;
	if (c1 < c0 && c->is32)
		c1 += 1ULL << 32;
	meas = (c1 - c0) * 1000000000ULL / dt_ns;
	ppm = c->hz ? ((int64_t)meas - (int64_t)c->hz) * 1000000 / (int64_t)c->hz : 0;
	printf("clockprobe: frequency %s: measured %ju Hz%s%ju Hz, %jd ppm)\n", c->name,
	    (uintmax_t)meas, c->hz ? " (nominal " : " (no nominal rate; ",
	    (uintmax_t)c->hz, (intmax_t)ppm);
}

/*
 * Cross-CPU ordering: A reads and publishes, B waits for the publication,
 * reads, and records B - A (ticks).  A negative minimum means B's counter
 * read less than a value A had already observed: the two CPUs' views are
 * out of order by that much.  Then the roles swap.
 */
struct cp_xcpu {
	struct cp_clock	*c;
	volatile uint64_t seq, val;
	volatile int	stop, exited;
	int		cpu;
	int64_t		min, max;
	uint64_t	n, neg;
};

static void
cp_xcpu_reader(void *arg)
{
	struct cp_xcpu *x = arg;
	uint64_t seen = 0, v, mine;
	int64_t d;

	cp_bind(x->cpu);
	x->min = INT64_MAX; x->max = INT64_MIN;
	while (!x->stop) {
		if (x->seq == seen) {
			cpu_spinwait();
			continue;
		}
		seen = x->seq;
		v = x->val;
		mine = x->c->read();
		d = (int64_t)(mine - v);
		if (d < x->min) x->min = d;
		if (d > x->max) x->max = d;
		if (d < 0) x->neg++;
		x->n++;
	}
	x->exited = 1;
	cp_unbind();
	kthread_exit();
}

static void
cp_cross(struct cp_clock *c, int a, int b)
{
	struct cp_xcpu *x;
	struct thread *td;
	uint64_t i;
	int pass;

	x = malloc(sizeof(*x), M_TEMP, M_WAITOK | M_ZERO);
	for (pass = 0; pass < 2; pass++) {
		memset(x, 0, sizeof(*x));
		x->c = c; x->cpu = pass == 0 ? b : a;
		if (kthread_add(cp_xcpu_reader, x, NULL, &td, 0, 0, "clockprobe_x") != 0) {
			printf("clockprobe: cross-CPU: no thread\n");
			break;
		}
		cp_bind(pass == 0 ? a : b);
		pause("cpx", hz / 100);
		for (i = 0; i < 200000; i++) {
			x->val = c->read();
			atomic_store_rel_64(__DEVOLATILE(uint64_t *, &x->seq), i + 1);
			cpu_spinwait(); cpu_spinwait(); cpu_spinwait(); cpu_spinwait();
		}
		cp_unbind();
		x->stop = 1;
		while (!x->exited)
			pause("cpxw", 1);
		printf("clockprobe: cross-CPU %s, writer cpu %d reader cpu %d: %ju handoffs, reader minus writer min %jd max %jd ticks, negative %ju (%ju.%02ju%%)\n",
		    c->name, pass == 0 ? a : b, pass == 0 ? b : a, (uintmax_t)x->n,
		    (intmax_t)x->min, (intmax_t)x->max, (uintmax_t)x->neg,
		    (uintmax_t)(x->n ? x->neg * 100 / x->n : 0),
		    (uintmax_t)(x->n ? (x->neg * 10000 / x->n) % 100 : 0));
	}
	free(x, M_TEMP);
}

/* ---------------------------------------------------------------- the run */

static void
cp_thread(void *arg __unused)
{
	struct cp_scan r;
	struct cp_clock *c;
	int i, cpu, partner;
	sbintime_t s0, s1;
	uint64_t c0, c1;

	cp_setup_clocks();
	cpu = cp_cpu;
	if (cpu < 0 || cpu > mp_maxid || CPU_ABSENT(cpu))
		cpu = 0;
	partner = cp_partner >= 0 && cp_partner <= mp_maxid && !CPU_ABSENT(cp_partner) ?
	    cp_partner : mp_maxid;
	cp_bind(cpu);
	/* get_cyclecount() rate for the ns conversions. */
	s0 = sbinuptime(); c0 = get_cyclecount();
	pause("cpcal", hz / 5);
	c1 = get_cyclecount(); s1 = sbinuptime();
	cp_ref_hz = (c1 - c0) * 1000000000ULL / ((((uint64_t)(s1 - s0)) * 1000000000ULL) >> 32);
	printf("clockprobe: %s, %d CPUs, hz %d, timecounter %s at %ju Hz, cpu_tickrate %ju Hz, get_cyclecount %ju Hz measured; single-CPU tests on cpu %d\n",
	    MACHINE_ARCH, mp_ncpus, hz, timecounter->tc_name, (uintmax_t)timecounter->tc_frequency,
	    (uintmax_t)cpu_tickrate(), (uintmax_t)cp_ref_hz, cpu);

	for (i = 0; i < cp_nclocks; i++) {
		c = &cp_clocks[i];
		cp_spin(20);
		printf("clockprobe: cost %s: %ju ns per read warm, ", c->name, (uintmax_t)cp_cost(c));
		printf("%ju ns fresh from idle (first 100 reads after 50 ms asleep)\n", (uintmax_t)cp_cost_cold(c));
	}
	for (i = 0; i < cp_nclocks; i++) {
		c = &cp_clocks[i];
		cp_spin(20);
		cp_scan(c, cp_reads, &r);
		cp_report_scan("scan", c, &r);
	}
	for (i = 0; i < cp_nclocks; i++)
		cp_frequency(&cp_clocks[i]);
	cp_scan_intr(&cp_clocks[0], cp_intr_secs);
	for (i = 0; i < cp_nclocks; i++)
		if (cp_clocks[i].read == cp_read_cpu_ticks)
			cp_scan_intr(&cp_clocks[i], cp_intr_secs);
	cp_unbind();
	cp_ticks_lag();
	for (i = 0; i < cp_nclocks; i++) {
		c = &cp_clocks[i];
		if (c->is32 || c->read == cp_read_sbinuptime)
			continue;
		cp_cross(c, cpu, partner);
	}
	/* Every CPU: the first clock's monotonic scan, reported only where it steps back. */
	c = &cp_clocks[0];
	{
		uint64_t cpus_back = 0, total_back = 0, worst = 0;
		int worst_cpu = -1;

		CPU_FOREACH(i) {
			cp_bind(i);
			cp_spin(5);
			cp_scan(c, cp_reads / 4 > 100000 ? cp_reads / 4 : 100000, &r);
			cp_unbind();
			if (r.back != 0) {
				cpus_back++;
				total_back += r.back;
				if (r.back > worst) { worst = r.back; worst_cpu = i; }
				printf("clockprobe: cpu %3d %s: %ju backward steps in %ju reads (max %ju ticks)\n",
				    i, c->name, (uintmax_t)r.back, (uintmax_t)r.reads, (uintmax_t)r.back_max);
			}
		}
		printf("clockprobe: all-CPU scan %s: %ju of %d CPUs stepped backward, %ju steps in total, worst cpu %d with %ju\n",
		    c->name, (uintmax_t)cpus_back, mp_ncpus, (uintmax_t)total_back, worst_cpu, (uintmax_t)worst);
	}
	printf("clockprobe: done\n");
	cp_running = 0;
	kthread_exit();
}

static int
cp_sysctl_run(SYSCTL_HANDLER_ARGS)
{
	struct thread *td;
	int error, val = 0;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val == 0)
		return (0);
	if (atomic_cmpset_int(&cp_running, 0, 1) == 0)
		return (EBUSY);
	error = kthread_add(cp_thread, NULL, NULL, &td, 0, 0, "clockprobe");
	if (error != 0)
		cp_running = 0;
	return (error);
}
SYSCTL_PROC(_kern_clockprobe, OID_AUTO, run, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
    NULL, 0, cp_sysctl_run, "I", "write 1 to run; results on the console");

static int
cp_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		return (0);
	case MOD_UNLOAD:
		return (cp_running ? EBUSY : 0);
	default:
		return (EOPNOTSUPP);
	}
}
static moduledata_t cp_mod = { "clockprobe", cp_modevent, NULL };
DECLARE_MODULE(clockprobe, cp_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(clockprobe, 1);
