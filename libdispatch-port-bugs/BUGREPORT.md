# devel/libdispatch (6.1.1_1,1): FreeBSD-specific code is broken - two crash bugs, one broken SPI, one latent bug

Draft bug report against the FreeBSD port `devel/libdispatch`
(MAINTAINER fluffy@FreeBSD.org).  Prepared 2026-09-19 against the
installed package and the port's patched source tree.  All three
issues are in code the PORT adds via `files/patch-*` (the 6.1.1
release tarball has no FreeBSD support); upstream `main` carries
its own FreeBSD support in which issues 1 and 2 do not exist and
issue 3 was fixed on 2026-09-02.

## Verification status

| | installed 6.1.1_1 | port source + 2 fixes below, rebuilt with the port's CMAKE_ARGS |
|---|---|---|
| `repro_min` (2 aliased threads, dispatch_sync) | SIGILL | waiter runs after ~400 ms, exit 0 |
| `repro_tid_alias` (8 threads x 20000 dispatch_sync) | SIGILL within ms | exit 0 |
| `repro_once` (4 threads, contended dispatch_once) | SIGILL | exit 0 |
| `repro_tid_ctl` (8 threads in DISTINCT id blocks) | exit 0 | exit 0 |
| `repro_group` (dispatch_group_wait) | rtld: Undefined symbol `_dispatch_ulock_wait`, exit 1 | `group wait returned 0` |
| `LD_BIND_NOW=1` any dispatch program | fails to start | runs |
| `nm -D --undefined-only libdispatch.so \| grep _dispatch` | `U _dispatch_ulock_wait` | (none) |
| `repro_runloop` (main-queue run-loop handle) | handle is the write end, never readable | read end 3 readable, exit 0 |
| `repro_group_timeout` (200 ms timed group wait) | n/a (library does not start the wait) | returns non-zero after 200 ms |

Issues 1 and 2 are therefore established by execution, not by code
reading: the crash reproduces on the shipped library, disappears
with the fix, and the control (same workload, no ID aliasing) never
crashes.  Issue 3 is established by a standalone harness of the
shipped loop plus code reading; it cannot execute in this port
because the code is compiled out.

## Minimal reproducer (issue 1)

Two threads whose kernel thread IDs share an aligned block of four;
the first holds a serial queue via `dispatch_sync` for 500 ms, the
second calls `dispatch_sync` on the same queue 100 ms later.  Correct
behaviour: the second waits ~400 ms and runs.  Observed: SIGILL.

```c
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread_np.h>
#include <semaphore.h>
#include <stdio.h>
#include <unistd.h>

#define MAXT 16
enum { EXIT, HOLDER, WAITER };
struct th { pthread_t t; int tid; int role; sem_t go; } T[MAXT];
static sem_t ready;
static dispatch_queue_t q;

static void hold(void *ctx) { (void)ctx; usleep(500000); }
static void noop(void *ctx) { (void)ctx; }

static void *worker(void *arg)
{
	struct th *me = arg;
	me->tid = pthread_getthreadid_np();
	sem_post(&ready);
	sem_wait(&me->go);
	if (me->role == HOLDER) {
		dispatch_sync_f(q, NULL, hold);
	} else if (me->role == WAITER) {
		usleep(100000);                 /* let the holder acquire first */
		dispatch_sync_f(q, NULL, noop);
		printf("waiter tid %d ran after the holder released the queue: OK\n", me->tid);
	}
	return NULL;
}

int main(void)
{
	int n, i, a = -1, b = -1;
	q = dispatch_queue_create("repro.serial", DISPATCH_QUEUE_SERIAL);
	sem_init(&ready, 0, 0);
	/* FreeBSD hands out thread IDs sequentially: a few threads suffice. */
	for (n = 0; n < MAXT && a < 0; n++) {
		sem_init(&T[n].go, 0, 0);
		pthread_create(&T[n].t, NULL, worker, &T[n]);
		sem_wait(&ready);
		for (i = 0; i < n; i++)
			if ((T[i].tid & ~3) == (T[n].tid & ~3)) { a = i; b = n; }
	}
	if (a < 0) { puts("no two threads share an ID block; rerun"); return 2; }
	printf("holder tid %d and waiter tid %d both map to lock owner %d\n",
	    T[a].tid, T[b].tid, T[a].tid & ~3);
	T[a].role = HOLDER; T[b].role = WAITER;
	sem_post(&T[a].go);
	usleep(20000);
	sem_post(&T[b].go);
	for (i = 0; i < n; i++) if (i != a && i != b) sem_post(&T[i].go);
	for (i = 0; i < n; i++) pthread_join(T[i].t, NULL);
	puts("completed");
	return 0;
}
```

```
$ cc -O1 -fuse-ld=lld repro_min.c -I/usr/local/include -L/usr/local/lib -ldispatch -lpthread -o repro_min
$ ./repro_min
Illegal instruction (core dumped)          # installed 6.1.1_1
$ LD_LIBRARY_PATH=/path/to/fixed/build ./repro_min
holder tid 1083712 and waiter tid 1083713 both map to lock owner 1083712
waiter tid 1083713 ran after the holder released the queue: OK
completed
```

## Environment

- FreeBSD 15.0-RELEASE-p12 amd64
- `libdispatch-6.1.1_1,1` from packages (port `DISTVERSION=6.1.1`,
  `PORTREVISION=1`); patched source examined in
  `work/swift-corelibs-libdispatch-swift-6.1.1-RELEASE`
- Reproducers compiled with the base compiler:
  `cc -O1 -fuse-ld=lld repro.c -I/usr/local/include -L/usr/local/lib -ldispatch -lpthread`
  (see issue 2 for why `-fuse-ld=lld` is needed).

## Issue 1 (crash): thread ID collides with lock flag bits - unrelated threads alias as the same lock owner

**Severity: high.**  Any two threads whose kernel thread IDs fall
in the same aligned block of four are indistinguishable to
libdispatch's owner checks.  Because FreeBSD allocates thread IDs
sequentially (`tid_alloc()`, `sys/kern/kern_thread.c`: `trytid =
tid + 1`), threads created back to back - a thread pool, for
example - alias almost by construction.  When two aliased threads
contend, libdispatch's deadlock/recursion detection misfires and
the process is killed with SIGILL.

### Root cause

`files/patch-src_shims_lock.h` defines the FreeBSD lock encoding as

```c
#define DLOCK_OWNER_MASK          ((dispatch_lock)0xfffffffc)
#define DLOCK_WAITERS_BIT         ((dispatch_lock)0x00000001)
#define DLOCK_FAILED_TRYLOCK_BIT  ((dispatch_lock)0x00000002)
#define _dispatch_tid_self()      ((dispatch_tid)(_dispatch_get_tsd_base()->tid))
```

The two low bits of a lock word are flag bits, so the owner
identity must not use them.  Windows and upstream's own FreeBSD
branch shift the ID left by two for exactly this reason
(`_dispatch_tid_self()` is `tid << 2` in upstream `src/shims/lock.h`
since commit b55398e, "fix freebsd build", 2025-01-21, and in the
in-tree Windows branch).  The port's definition does not shift.
Every owner comparison then masks the low two bits away:

```c
_dispatch_lock_value_from_tid(tid)      = tid & DLOCK_OWNER_MASK;
_dispatch_lock_is_locked_by(lock, tid)  = ((lock ^ tid) & DLOCK_OWNER_MASK) == 0;
```

so tids 100004, 100005, 100006 and 100007 all present as owner
100004.  The same encoding is used for the queue drain lock in the
low 32 bits of `dq_state` (`DISPATCH_QUEUE_DRAIN_OWNER_MASK ==
DLOCK_OWNER_MASK`, `src/queue_internal.h:347`), so queue ownership
aliases too.

### Consequences (all `DISPATCH_CLIENT_CRASH`, i.e. `__builtin_trap()` -> SIGILL)

- `dispatch_sync()` onto a serial queue currently held by an
  aliased thread: `__DISPATCH_WAIT_FOR_QUEUE__` reports
  "dispatch_sync called on queue already owned by current thread"
  (`src/queue.c:1613-1615`) for a perfectly legal call.
- `dispatch_once()` contended by an aliased thread:
  `_dispatch_once_wait` reports "trying to lock recursively"
  (`src/shims/lock.c:722-723`).
- `_dispatch_unfair_lock_lock_slow` (queue side-locks etc.):
  "trying to lock recursively" (`src/shims/lock.c:636/668`).
- Conversely, a REAL recursive lock or self-deadlock by an aliased
  thread's neighbour passes the "lock not owned by current thread"
  check, so genuine bugs can be masked.

### Reproduction

`repro_tid_alias.c` - eight threads doing `dispatch_sync_f` onto one
serial queue:

```c
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdio.h>
#include <stdint.h>
#define NTHREADS 8
#define ITERS 20000
static dispatch_queue_t q;
static void work(void *ctx) { (void)ctx; for (volatile int i = 0; i < 2000; i++) ; }
static void *thr(void *arg) {
	int tid = pthread_getthreadid_np();
	printf("thread %d: tid %d -> lock owner value %d\n", (int)(intptr_t)arg, tid, tid & ~3);
	fflush(stdout);
	for (int i = 0; i < ITERS; i++) dispatch_sync_f(q, NULL, work);
	return NULL;
}
int main(void) {
	pthread_t t[NTHREADS];
	q = dispatch_queue_create("repro.serial", DISPATCH_QUEUE_SERIAL);
	for (int i = 0; i < NTHREADS; i++) pthread_create(&t[i], NULL, thr, (void *)(intptr_t)i);
	for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
	printf("completed without crash\n");
	return 0;
}
```

Observed:

```
thread 1: tid 1083546 -> lock owner value 1083544
thread 2: tid 1083547 -> lock owner value 1083544
thread 4: tid 1083549 -> lock owner value 1083548
Illegal instruction (exit status 132)
```

lldb: `stop reason = signal SIGILL: privileged opcode`, frame 0 in
`libdispatch.so.1` at a `ud2` (the `DISPATCH_CLIENT_CRASH` trap).
Threads 1 and 2 alias; the crash occurs within milliseconds, before
the remaining threads even print.

Control - the identical workload, but keeping only threads whose
IDs fall in DISTINCT aligned blocks (colliding threads exit
immediately; 27 threads were created to obtain 8 non-aliasing
ones) - runs the full 8 x 20000 `dispatch_sync` iterations and
prints `completed without crash`.  Aliasing is the trigger.

Second API, `repro_once.c` - four threads racing one
`dispatch_once_f` whose initializer sleeps 1 s: also dies with
SIGILL (the "trying to lock recursively" check in
`_dispatch_once_wait`).

### Fix

Adopt upstream's encoding:

```c
-#define _dispatch_tid_self()        ((dispatch_tid)(_dispatch_get_tsd_base()->tid))
+#define _dispatch_tid_self()        ((dispatch_tid)(_dispatch_get_tsd_base()->tid << 2))
```

Every consumer of `_dispatch_tid_self()` must agree; in particular
the workqueue monitor's comparison against `kinfo_proc.ki_tid`
needs the matching `<< 2` (upstream commit 0c50788, "FreeBSD tid
are off by two bits (see lock.h)", 2026-09-02) - see issue 3.
Thread IDs are `lwpid_t` (32-bit, allocated as `index + NO_PID`,
bounded by `kern.maxthread`), so the shift cannot overflow.

## Issue 2 (crash / link failure): `_dispatch_ulock_wait` is referenced but never defined; `_dispatch_wake_by_address` is a no-op

**Severity: high.**  The installed `libdispatch.so.1.3` has an
unresolved symbol:

```
$ nm -D --undefined-only /usr/local/lib/libdispatch.so.1.3 | grep _dispatch
                 U _dispatch_ulock_wait
```

### Root cause

`files/patch-src_shims_lock.c` adds a FreeBSD branch to
`_dispatch_wait_on_address()` that calls `_dispatch_ulock_wait()`:

```c
+#elif defined(__FreeBSD__)
+	...
+	if (nsecs == DISPATCH_TIME_FOREVER) {
+		return _dispatch_ulock_wait(address, value, 0, flags);
+	}
+	do { ... rc = _dispatch_ulock_wait(address, value, (uint32_t)usecs, flags); ...
```

but `_dispatch_ulock_wait()` is defined only under
`#if HAVE_UL_COMPARE_AND_WAIT` (`src/shims/lock.c:375-390`), a Darwin
`ulock` feature that is 0 on FreeBSD.  Nothing else defines it, so
the shared library ships with a dangling reference.  The companion
`_dispatch_wake_by_address()` has no FreeBSD branch at all and falls
through to `(void)address;` (`src/shims/lock.c:565-576`) - a wake
that wakes nobody.  Even with the undefined symbol resolved, every
waiter on this path would block forever.

### Consequences

- **Linking**: `ld` from `devel/binutils` refuses to link any
  program against the library ("undefined reference to
  `_dispatch_ulock_wait`"); base `lld` accepts it only because it
  does not diagnose undefined symbols inside shared libraries.
- **Immediate binding** (`LD_BIND_NOW=1`, or any binary linked with
  `-z now`, which hardened builds commonly use): the program does not
  start:
  `ld-elf.so.1: /usr/local/lib/libdispatch.so.1: Undefined symbol "_dispatch_ulock_wait"`
- **Lazy binding**: the program dies the first time any thread
  reaches `_dispatch_wait_on_address()`.  In-tree callers:
  `dispatch_group_wait()` (`src/semaphore.c:214`) and the
  source-cancellation wait in `src/source.c:1095`.

### Reproduction

`repro_group.c` - one `dispatch_group_wait()` on a group whose single
block sleeps 1 s:

```c
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <unistd.h>
static void slow(void *ctx) { (void)ctx; sleep(1); }
int main(void) {
	dispatch_group_t g = dispatch_group_create();
	dispatch_group_async_f(g, dispatch_get_global_queue(0, 0), NULL, slow);
	printf("group wait returned %ld\n", dispatch_group_wait(g, DISPATCH_TIME_FOREVER));
	return 0;
}
```

Observed:

```
ld-elf.so.1: /usr/local/lib/libdispatch.so.1: Undefined symbol "_dispatch_ulock_wait"
(exit status 1)
```

`LD_BIND_NOW=1 ./any-dispatch-program` fails identically at startup.

### Fix

Upstream `main` implements both halves with `_umtx_op(2)`:

```c
/* _dispatch_wait_on_address() */
#elif defined(__FreeBSD__)
	(void)flags;
	if (nsecs != DISPATCH_TIME_FOREVER) {
		struct timespec ts = {
			.tv_sec = (__typeof__(ts.tv_sec))(nsecs / NSEC_PER_SEC),
			.tv_nsec = (__typeof__(ts.tv_nsec))(nsecs % NSEC_PER_SEC),
		};
		return _umtx_op((void*)address, UMTX_OP_WAIT_UINT, value,
			(void*)(uintptr_t)sizeof(struct timespec), (void*)&ts);
	}
	return _umtx_op((void*)address, UMTX_OP_WAIT_UINT, value, 0, 0);

/* _dispatch_wake_by_address() */
#elif defined(__FreeBSD__)
	_umtx_op((void*)address, UMTX_OP_WAKE, INT_MAX, 0, 0);
```

(`UMTX_OP_WAIT_UINT_PRIVATE`/`UMTX_OP_WAKE_PRIVATE` would be the
process-private variants; the addresses involved are never shared
across processes.)  Note `_umtx_op` returns -1/errno rather than an
error code; upstream returns the raw result, which callers treat
as "not zero -> retry", so this is tolerable but worth a
`return rc == 0 ? 0 : errno;` wrapper for `ETIMEDOUT` fidelity.

## Issue 3 (latent in the port, fixed upstream): loop-variable typo in the FreeBSD workqueue monitor

**Severity: low in this port (dead code); medium once enabled.**

`files/patch-src_event_workqueue.c` adds
`_dispatch_workq_count_runnable_workers()` for FreeBSD.  Its inner
loop is

```c
	for (int i = 0; i < mon->num_registered_tids; ++i) {
		dispatch_tid tid = mon->registered_tids[i];
		for (int j = 0; i < count; ++i) {          /* tests and increments i, never j */
			if ((dispatch_tid)kp[j].ki_tid != tid) { continue; }
			if (kp[j].ki_stat == SRUN || kp[j].ki_stat == SIDL) {
				++runners;
				break;
			}
		}
	}
```

Only `kp[0]` is ever compared, against `registered_tids[0]` only,
and the outer loop terminates after one iteration.  A standalone
harness with three registered, runnable workers behind the main
thread in `kinfo_proc` order reports `port loop counts 0 runnable`
versus `fixed loop counts 3 runnable`.

Why it is latent here: the 6.1.1 update (ports 3ebf8d930fab,
2026-01-20) enabled the monitor for FreeBSD, and ports 5802e9ec58ca
(2026-02-05) disabled it again because "It causes random crashes in
telegram-desktop", so today `HAVE_DISPATCH_WORKQ_MONITORING` is 0 on
FreeBSD and the entire monitor - and this function - is compiled
out.  Two consequences: (a) the typo has no effect today;
(b) the port has NO pool-size monitor at all, so when all workers of
a non-overcommit root queue block in system calls, no replacement
thread is ever created (the monitor exists precisely to "create new
threads when too many existing worker threads block", per
`dispatch/queue.h`) - a liveness gap relative to Linux.

If monitoring were enabled with the typo in place: `num_runnable`
is nearly always 0, the "we have work but no worker is runnable"
branch fires every second for every non-empty QoS bucket, and the
pool grows one thread per bucket per second (the negative `floor`
in that branch deliberately allows exceeding `active_cpus`) up to
`WORKQ_MAX_TRACKED_TIDS` (255).  `dispatch_assert()` on the
registration bound is compiled out in release builds.

Upstream status: the FreeBSD monitor arrived in b55398e (2025-01-21)
WITH this typo, and upstream enabled monitoring for FreeBSD in the
same commit; fixed by 565b739 "FreeBSD: typo in workqueue.c"
(`j < count; ++j`) and 0c50788 "FreeBSD tid are off by two bits
(see lock.h)" (`(kp[j].ki_tid << 2) != tid`), both 2026-09-02.  The
port should take both, together with the `workqueue_internal.h`
enablement, so the monitor exists AND is correct.

Minor notes on the same function: `struct kinfo_proc kp[255]` is a
~280 KB stack frame (KINFO_PROC_SIZE is 1088 on 64-bit
architectures) on the manager thread - a heap or static buffer
would be safer; counting `SIDL` as runnable is harmless but
meaningless (it is the fork-in-progress process state).

## Issue 4 (broken SPI): main-queue run-loop handle truncated to the pipe's write end

The port's queue.c builds the `__unix__` run-loop variant, which creates
a pipe and packs both descriptors as `(rfd << 32) | wfd` into a 64-bit
handle (`DISPATCH_RUNLOOP_HANDLE_PACK/RFD/WFD`, port queue.c:6524-6526),
but `private/private.h` still lists FreeBSD with Linux as
`typedef int dispatch_runloop_handle_t`.  The packed value is stored,
returned and passed as an int: the handle a run loop gets from
`_dispatch_get_main_queue_handle_4CF()` is the WRITE end, which the poke
also writes to, so polling it for readability never wakes (verified:
`handle 0x4: read end 0, write end 4`, poll times out).  On dispose,
`DISPATCH_RUNLOOP_HANDLE_RFD(handle)` shifts an int by 32 - undefined
behaviour (C11 6.5.7p3), diagnosed by clang as "shift count >= width of
type" - which on amd64 evaluates to the write end again: double close,
read end leaked.  Fix (`bugzilla/04-runloop-handle.patch`): drop
`__FreeBSD__` from the int typedef so FreeBSD falls through to the
`__unix__` `uint64_t` typedef the packing code was written for, exactly
as upstream main's private.h reads.  Rebuilt, the three queue.c warnings
vanish and the reproducer shows `handle 0x300000004`, read end readable.
ABI note: the SPI's return type changes; consumers must be rebuilt.
Upstream is heading the other way - swift-corelibs-libdispatch PR #957
(2026-09-04, open) switches FreeBSD to `eventfd(2)`, a single int handle
like Linux, paired with swift-corelibs-foundation PR #5539 - and FreeBSD
>= 13 has eventfd in libc, so the port may prefer that once it lands.

## C-correctness review of the patches (2026-09-21)

Review of the proposed patches for undefined or implementation-defined
behaviour, prompted by the signed `pid_t` tid meeting the unsigned
`uint32_t` lock word.  Findings, all fixed in the current patches:

- **Signed left shift (UB).**  `(dispatch_tid)(tid << 2)` shifts a
  signed `pid_t`; C11 6.5.7p4 makes that undefined once the result does
  not fit, i.e. for tid >= 2^29 - below the 2^30 bound the guard
  enforces, so the guard alone did not cover it.  Upstream's FreeBSD
  branch has the same form.  Fixed by converting first:
  `((dispatch_tid)tid) << 2`, defined for every value (modulo 2^32), and
  by defining `DLOCK_TID_MAX = DLOCK_OWNER_MASK >> 2` as the one bound the
  encoding and the guard share.
- **Signed-to-unsigned conversion in the guard** is well defined
  (modulo 2^32); a negative tid, impossible in practice, would trip the
  guard, which is the desired outcome.
- **Return convention of the umtx wait.**  `_umtx_op(2)` returns -1 and
  sets errno, while `_dispatch_wait_on_address()` must return 0 or an
  errno value (callers test `rc == ETIMEDOUT`).  Not UB, but a contract
  violation shared with upstream; fixed with `rc == 0 ? 0 : errno`.
  `_umtx_op_err()`, which would return the code directly, is a
  libthr-private syscall stub and not exported by libc.  Verified with a
  200 ms timed `dispatch_group_wait()`.
- **Timeout clock (upstream, and this patch until 2026-09-22).**  A bare
  `struct timespec` in the umtx call is a *relative* timeout on
  `CLOCK_REALTIME` (`umtx_copyin_umtx_time` sets `_clockid =
  CLOCK_REALTIME` for that layout; `umtx_abs_timeout_init` adds the
  interval to that clock), so a wall-clock step during a timed
  `dispatch_group_wait()` fires it early or late.  libdispatch's deadlines
  are monotonic (`CLOCK_UPTIME` on FreeBSD, `shims/time.h`); the Linux
  branch waits on `CLOCK_MONOTONIC`.  Fixed by passing a `struct
  _umtx_time` with `_clockid = CLOCK_MONOTONIC`, `_flags = 0`, as libthr's
  `_thr_umtx_timedwait_uint()` does.
- **Integer-to-pointer conversion** for the umtx size argument is
  implementation-defined (C11 6.3.2.3p5) but is the `_umtx_op(2)` ABI for
  selecting the timeout layout and the only way to name the clock; libthr
  uses the same cast.  An earlier revision of this patch avoided it by
  passing `NULL`, which silently kept `CLOCK_REALTIME` semantics: the
  wrong trade-off, reverted.  Casting away `volatile` for the wake uses
  `__DEVOLATILE()`.
- **workqueue.c comparison** `((dispatch_tid)kp[j].ki_tid << 2)` casts
  before shifting: defined.  A negative `ki_tid` converts modulo 2^32 to
  a value that matches nothing: harmless.
- **What an earlier unsigned cast can and cannot fix.**  It fixes the
  cases where the information is still intact at the cast: the two tid
  shifts (`((dispatch_tid)tid) << 2` in lock.h and
  `((dispatch_tid)kp[j].ki_tid) << 2` in workqueue.c) and the guard's
  comparison.  It cannot fix issue 4: casting the `int` handle to
  `uint64_t` before `>> 32` removes the shift UB but yields 0, because
  the read end was already lost when the packed value was stored through
  the `int` (an out-of-range unsigned-to-signed conversion,
  implementation-defined, C11 6.3.1.3p3) - the type has to change, not
  the expression.  The umtx return value, the integer-to-pointer size
  argument and the `volatile` cast are not signedness problems at all.
  Residual, shared with upstream: on 32-bit FreeBSD (i386, armv7,
  powerpc - which this port builds) the `__unix__` variant stores the
  64-bit handle through `(void *)(uintptr_t)`, a 32-bit `uintptr_t`, so
  the read end is lost again there; only a design change (eventfd, or a
  heap-allocated pair) fixes all architectures.
- **Not ours, noted:** the one remaining warning in the rebuilt tree,
  `(dispatch_lock)new_v` truncating a `uintptr_t` gate word in
  `_dispatch_once_wait` (lock.c), is upstream code and intentional.
  The build's other warnings led to issue 4 above.

## Why the tid's low bits are set, and why its top two bits could be

Everything below is linked to the exact source (FreeBSD `releng/15.0`
at af58d0db, the ports tree at 01b09545, libdispatch at tag
`swift-6.1.1-RELEASE` and `main`).

**What a FreeBSD thread ID is.**  `lwpid_t` is a plain `int32_t`
([sys/_types.h:124](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/sys/_types.h#L124)).  The kernel allocates
it in `tid_alloc()`
([kern_thread.c:234-257](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/kern/kern_thread.c#L234-L257)) by
finding the next clear bit in a bitmap starting at `trytid`, then setting
`trytid = tid + 1` ([:254](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/kern/kern_thread.c#L254)) and
returning `tid + NO_PID` ([:256](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/kern/kern_thread.c#L256)),
where `NO_PID = PID_MAX + 1 = 100000`
([proc.h:957-958](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/sys/proc.h#L957-L958)).  Consecutive threads
therefore get consecutive integers: 100004, 100005, 100006, 100007.  The
low two bits take every value, and any four threads created back to back
share the same value in bits 2-31.  This has always been so: thread IDs
were introduced as integers because core-dump notes and the gdb remote
protocol need integers
([fdcac928, 2004-04-03](https://github.com/freebsd/freebsd-src/commit/fdcac92868ae2506749bb4edc925e754f2118006)),
allocated sequentially by a unit allocator and, since
[35bb59ed, 2020-11-09](https://github.com/freebsd/freebsd-src/commit/35bb59edc59d16f69ccc73f17adb0d999aaf118a),
by the bitmap.  `pthread_getthreadid_np()` returns exactly this kernel
value ([libthr thr_getthreadid_np.c:48](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/lib/libthr/thread/thr_getthreadid_np.c#L48)),
and it is what the port stores in `tsd->tid`.

**Why the top two bits could, in theory, be set.**  The largest tid is
`NO_PID + kern.maxthread - 1`.  `kern.maxthread` defaults to
`min(maxproc * max_threads_per_proc, 1000000)` on 64-bit and `100000` on
32-bit ([kern_thread.c:554-560](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/kern/kern_thread.c#L554-L560)),
so the default ceiling is about 1.1 million, far below 2^30.  But the
sysctl is `CTLFLAG_RDTUN` with no upper clamp
([kern_thread.c:162-163](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/kern/kern_thread.c#L162-L163)); the
only bound the kernel promises is the type: the `umutex` comment states
that "TIDs values start with PID_MAX + 2 and end by INT32_MAX"
([umtx.h:48-56](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/sys/umtx.h#L48-L56)).  So bit 31 is never set
(the value is a positive `int32_t`), but bit 30 can be if an
administrator sets `kern.maxthread` above 1073641824 (a 128 MB tid
bitmap).  Nobody does, but the kernel does not forbid it, and the
shifted encoding would then alias silently - hence the guard.

**Why the same layout works elsewhere.**
- Darwin: the owner is a Mach port name, `(index << 8) | gen_byte`
  ([xnu osfmk/mach/port.h](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/port.h), `MACH_PORT_MAKE`/
  `MACH_PORT_GEN`).  The generation byte is
  `IE_BITS_GEN(bits) = (bits & 0xfc000000) | IE_BITS_ROLL_MASK` with
  `IE_BITS_ROLL_MASK 0x03000000`
  ([xnu osfmk/ipc/ipc_entry.h](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/ipc/ipc_entry.h)), so a port
  name's low two bits are always **set** (names look like 0x103, 0x207).
  libdispatch's Darwin block uses them as flag bits and restores them
  when recovering the owner
  ([lock.h:37-58 at the tag](https://github.com/swiftlang/swift-corelibs-libdispatch/blob/swift-6.1.1-RELEASE/src/shims/lock.h#L37-L58),
  `_dispatch_lock_owner` returns `lock_value | both bits`).  The mask
  works because the bits are constant, not because they are zero.
- Linux: flags in the high bits per the futex ABI
  (`FUTEX_WAITERS 0x80000000`, `FUTEX_OWNER_DIED 0x40000000`,
  `FUTEX_TID_MASK 0x3fffffff`,
  [include/uapi/linux/futex.h](https://github.com/torvalds/linux/blob/master/include/uapi/linux/futex.h));
  the kernel guarantees `pid_max <= PID_MAX_LIMIT = 4194304`
  ([include/linux/threads.h](https://github.com/torvalds/linux/blob/master/include/linux/threads.h)),
  so the raw tid always fits.  libdispatch's Linux block
  ([lock.h:59-81](https://github.com/swiftlang/swift-corelibs-libdispatch/blob/swift-6.1.1-RELEASE/src/shims/lock.h#L59-L81))
  stores it unshifted.
- Windows: a `DWORD` thread id with the Darwin-style low-bit mask,
  made safe by shifting left two
  ([lock.h:82-102](https://github.com/swiftlang/swift-corelibs-libdispatch/blob/swift-6.1.1-RELEASE/src/shims/lock.h#L82-L102),
  `_dispatch_tid_self()` is `tid << 2`).

**What the port did.**  Its FreeBSD block copies the Darwin/Windows mask
but not the Windows shift
([files/patch-src_shims_lock.h:17-21](https://github.com/freebsd/freebsd-ports/blob/01b09545a3a590187687201d238c8cbf3135e140/devel/libdispatch/files/patch-src_shims_lock.h#L17-L21)),
unchanged since the port's reintroduction
([b9662312, 2022-11-08](https://github.com/freebsd/freebsd-ports/commit/b9662312dff8f05fb241525a2fda6adc33183dbb)),
through the 5.10.1 update
([dbf3e16b, 2024-08-12](https://github.com/freebsd/freebsd-ports/commit/dbf3e16ba2f4e3752b14a67270caed69fbe53da1))
and the 6.1.1 update
([3ebf8d93, 2026-01-20](https://github.com/freebsd/freebsd-ports/commit/3ebf8d930fabe3c64048ede0f3187c07e4754ba2)).
The workqueue monitor was disabled on FreeBSD after "random crashes in
telegram-desktop"
([5802e9ec, 2026-02-05](https://github.com/freebsd/freebsd-ports/commit/5802e9ec58cadcab823527e55d66c658c1f010a6)).
Upstream's independent FreeBSD branch shifts by two since
[b55398e, 2025-01-21](https://github.com/swiftlang/swift-corelibs-libdispatch/commit/b55398e), and fixed its monitor's loop
([565b739](https://github.com/swiftlang/swift-corelibs-libdispatch/commit/565b739)) and comparison
([0c50788](https://github.com/swiftlang/swift-corelibs-libdispatch/commit/0c50788)) on 2026-09-02.

**Why the guard writes to stderr instead of using
`DISPATCH_INTERNAL_CRASH`.**  On every platform but Darwin and Windows,
the message half of the crash macros is compiled away:
`_dispatch_set_crash_log_cause_and_message(ac, msg)` expands to
`((void)(ac))` ([internal.h:998 at the tag](https://github.com/swiftlang/swift-corelibs-libdispatch/blob/swift-6.1.1-RELEASE/src/internal.h#L998)),
so `DISPATCH_INTERNAL_CRASH` and `DISPATCH_CLIENT_CRASH` reduce to a bare
`__builtin_trap()`.  That is why every existing libdispatch trap on
FreeBSD is a silent SIGILL with no text anywhere.  `_dispatch_log()` is
not a substitute in a crash path: in release builds with no
`LIBDISPATCH_LOG` setting it goes to syslog, and it runs a lazy
initialisation that reads the environment and may open a file
(`_dispatch_logv_init`, init.c).  A `write(2)` of a static string to
`STDERR_FILENO` is async-signal-safe, cannot fail for lack of memory,
needs no initialisation, and shows up where the user is looking; the
guard follows it with the same `_dispatch_hardware_crash()` the macros
use, so the process still dies the same way, just with a reason attached.

## Why upstream's fixes never reached the port

The port and upstream have two unrelated FreeBSD implementations.  The
port's dates from its 2022 reintroduction and lives entirely in
`files/patch-*`, applied to a tarball with no FreeBSD support.  Upstream's
was written by michael-yuji, merged to main in 2025, and first shipped in
`swift-6.3-RELEASE` (tagged 2026-02-19; also in 6.3.1).  It is absent from
6.1.1 and from `swift-6.2-RELEASE` (2025-08-19), whose lock.h has only the
Darwin, Linux and Windows blocks.  The port moved to 6.1.1 on 2026-01-20,
a month before 6.3 existed, and so still compiles the pre-FreeBSD tarball
plus its own patches.  Upstream's September 2026 commits fixed upstream's
code; nothing they touched is in the port.

Consequences for a port update to 6.3.x: the shim patches (lock.h,
lock.c, workqueue.c, workqueue_internal.h, private.h) conflict with or
duplicate upstream's files and should be dropped, which resolves issues
1, 2 and 4 outright.  Issue 3 survives the update: 6.3.x ships upstream's
monitor enabled, with the same loop typo and unshifted comparison, since
the fixes (565b739, 0c50788) are in no release yet.  Two of this report's
refinements are also candidates for upstream: the `((dispatch_tid)tid) << 2`
spelling (upstream still shifts the signed value) and the umtx wait's
return-value and size-argument handling.

## Upstream status per issue (verified against the tags, 2026-09-21)

| issue | upstream state | where | left open upstream |
|---|---|---|---|
| 1 tid aliasing | never present: FreeBSD block shifts `tid << 2` | b55398e; shipped in 6.3, 6.3.1 | shifts the signed `pid_t` (UB from 2^29); no range guard |
| 2 wait/wake | implemented with `_umtx_op(UMTX_OP_WAIT_UINT / UMTX_OP_WAKE)` | b55398e; shipped in 6.3, 6.3.1 | returns raw -1 (contract is 0/errno); relative timeout on `CLOCK_REALTIME` (bare timespec) rather than a monotonic clock; non-`_PRIVATE` ops |
| 3 monitor loop | fixed on `main` only: `j < count; ++j` and `(ki_tid << 2) != tid` | 565b739, 0c50788 (2026-09-02) | not in any release; 6.3.x ships the typo with the monitor enabled; `kp[255]` stack frame and `SIDL` unchanged |
| 4 run-loop handle | typedef chain: `int` for Linux only, `uint64_t` for `__unix__` | private.h in 6.3, 6.3.1 | pipe-pair packing lost on 32-bit (`uintptr_t` store); eventfd rewrite pending (PR #957, open) |
| guard | absent | - | - |

## Impact and importance

Who is affected: the port's reverse dependencies in the ports tree
are `devel/bacnet-stack`, `sysutils/bacnet-stack-apps` and
`net-im/telegram-desktop`, plus any locally built software.  The
port does not run libdispatch's test suite (no test target in the
port Makefile), and the base linker does not diagnose undefined
symbols inside shared libraries, so neither defect surfaces at
build time.

It has very likely already bitten in the field: ports commit
5802e9ec58ca (2026-02-05) disabled the workqueue monitor on FreeBSD
after "random crashes in telegram-desktop".  The monitor (with the
issue 3 loop bug) adds one worker thread per second, producing many
threads with consecutive IDs - exactly the population in which issue
1's aliased threads contend and trap.  Disabling it lowered the
collision rate without touching the cause; the signature (random
SIGILL/ud2 inside libdispatch, no message) matches issue 1.

History: the unshifted encoding has been in the port since it was
reintroduced (b9662312dff8, 2022-11-08) and survived the 5.10.1 and
6.1.1 updates unchanged.  Nothing changed on FreeBSD's side: lwpid_t
has been a sequential integer (bitmap index + NO_PID) since 2004 and
`pthread_getthreadid_np()` returns exactly that.  The 0xfffffffc
low-flag-bit layout is Darwin's (valid there because Mach port names
have their two low bits constantly SET - `_dispatch_lock_owner` ORs
them back in - so borrowing them loses nothing); Windows reuses it and
manufactures the guarantee with `<< 2`; Linux instead keeps flags in the HIGH bits
(futex layout, `FUTEX_TID_MASK` 0x3fffffff) and needs no shift.  The
port copied the Darwin/Windows layout without the Windows shift.

Why software can still appear to work:

- Issue 1 needs CONTENTION between two threads whose IDs share a
  block - a `dispatch_sync`/`dispatch_once`/side-lock attempt while
  an aliased thread holds the object.  Code that only uses
  `dispatch_async`, timers, sources and `dispatch_main` never takes
  those paths; `dispatch_once` contention is a startup-only race.
  Thread pools alias almost by construction, but a program that
  never synchronously waits on a queue another worker is draining
  does not exercise the check.  When it does trip, the failure is
  an instant SIGILL with no message on stderr, and the symbol that
  a debugger would show is "BUG IN CLIENT OF LIBDISPATCH:
  dispatch_sync called on queue already owned by current thread" -
  the bug is misattributed to the application.  That misdirection,
  more than the crash itself, is why this deserves priority.
- Issue 2 needs `dispatch_group_wait()`,
  `dispatch_source_cancel_and_wait()`, or immediate binding.  With
  the default lazy binding, programs that never call those two
  functions never resolve the missing symbol.  Any binary linked
  with `-z now` (hardening default in several build systems) fails
  at startup regardless of what it calls.
- Issue 3 is compiled out.

Severity: issues 1 and 2 make a documented, correct usage pattern
of the library (synchronous waits from a pool of threads) crash the
process, and issue 2 additionally breaks a whole class of
otherwise-correct programs at startup.  Both fixes are small and
already upstream.  Recommended: fix before the next package build;
do not wait for a 6.1.1 -> 6.2 update.

## Fix (verified)

The following diff against the port's patched tree was built with
the port's own `CMAKE_ARGS` and passes every reproducer above.  It
mirrors upstream `main` except for using the process-private
`_umtx_op` variants (the addresses involved are never shared
between processes).

```diff
--- src/shims/lock.h
+++ src/shims/lock.h
@@ -114,7 +114,7 @@
 #define DLOCK_WAITERS_BIT   ((dispatch_lock)0x00000001)
 #define DLOCK_FAILED_TRYLOCK_BIT  ((dispatch_lock)0x00000002)
 
-#define _dispatch_tid_self()        ((dispatch_tid)(_dispatch_get_tsd_base()->tid))
+#define _dispatch_tid_self()        ((dispatch_tid)(_dispatch_get_tsd_base()->tid << 2))
 
 DISPATCH_ALWAYS_INLINE
 static inline dispatch_tid
--- src/shims/lock.c
+++ src/shims/lock.c
@@ -544,18 +544,16 @@
 	if (dwMilliseconds == 0) return ETIMEDOUT;
 	return WaitOnAddress(address, &value, sizeof(value), dwMilliseconds) == TRUE;
 #elif defined(__FreeBSD__)
-	uint64_t usecs = 0;
-	int rc;
-	if (nsecs == DISPATCH_TIME_FOREVER) {
-		return _dispatch_ulock_wait(address, value, 0, flags);
+	(void)flags;
+	if (nsecs != DISPATCH_TIME_FOREVER) {
+		struct timespec ts = {
+			.tv_sec = (__typeof__(ts.tv_sec))(nsecs / NSEC_PER_SEC),
+			.tv_nsec = (__typeof__(ts.tv_nsec))(nsecs % NSEC_PER_SEC),
+		};
+		return _umtx_op((void *)address, UMTX_OP_WAIT_UINT_PRIVATE, value,
+		    (void *)(uintptr_t)sizeof(struct timespec), (void *)&ts);
 	}
-	do {
-		usecs = howmany(nsecs, NSEC_PER_USEC);
-		if (usecs > UINT32_MAX) usecs = UINT32_MAX;
-		rc = _dispatch_ulock_wait(address, value, (uint32_t)usecs, flags);
-	} while (usecs == UINT32_MAX && rc == ETIMEDOUT &&
-			(nsecs = _dispatch_timeout(timeout)) != 0);
-	return rc;
+	return _umtx_op((void *)address, UMTX_OP_WAIT_UINT_PRIVATE, value, 0, 0);
 #else
 #error _dispatch_wait_on_address unimplemented for this platform
 #endif
@@ -570,6 +568,8 @@
 	_dispatch_futex_wake((uint32_t *)address, INT_MAX, FUTEX_PRIVATE_FLAG);
 #elif defined(_WIN32)
 	WakeByAddressAll((uint32_t *)address);
+#elif defined(__FreeBSD__)
+	_umtx_op((void *)address, UMTX_OP_WAKE_PRIVATE, INT_MAX, 0, 0);
 #else
 	(void)address;
 #endif
```

Range note for the `<< 2`: it truncates tids at or above 2^30.  A
FreeBSD tid is bounded by `kern.maxthread + NO_PID`, default about
1.1 million, but the tunable has no kernel-side clamp, so
`bugzilla/01-tid-alias-guard.patch` adds a one-time check in
`libdispatch_tsd_init()` that aborts with a clear message if a tid
ever exceeds 30 bits (verified to compile; reproducers unaffected).

Encoding note: nothing in a FreeBSD build ever decodes an owner value
back into a tid.  Every consumer is a masked equality test
(`_dispatch_lock_is_locked_by`, drain-owner checks, `dsc_waiter`
comparisons); the owner is handed to the OS only on Darwin
(`thread_switch`, `_pthread_workqueue_override_start_*`, all under
`HAVE_PTHREAD_WORKQUEUE_QOS`/`TARGET_OS_MAC`, where port names make
the encoding the identity), decoded explicitly only by the Windows
workqueue monitor (`registered_tids[i] >> 2`), and decoded implicitly
by the Linux kernel in `FUTEX_LOCK_PI`, which is why Linux must store
the raw tid.  On FreeBSD the shift is therefore a free choice - with
one consequence worth recording: FreeBSD's native `umutex` word is
"raw tid, contention flag in bit 31" (`sys/umtx.h`, `UMUTEX_CONTESTED
0x80000000`, tids guaranteed in `[PID_MAX+2, INT32_MAX]`), so a
Linux-style high-bit layout would keep the lock word compatible with
kernel-assisted (`UMTX_OP_MUTEX_*`, priority-inheriting) locking
later, while the Windows-style shift forecloses it.  Either layout
needs two flag bits and hence 30-bit tids; the kernel only guarantees
31, so the guard patch is needed either way.  The reports keep the
shift for consistency with upstream.

Alternatively replace the port's FreeBSD shim patches wholesale with
upstream `main`'s `src/shims/lock.h`, `src/shims/lock.c`,
`src/event/workqueue.c` and `src/event/workqueue_internal.h`, which
also brings the workqueue monitor (issue 3) in its corrected form.
Either way, run the reproducers as regression checks.

## Files

Everything referenced here lives next to this report in
`libdispatch-port-bugs/`:

- `repro_min.c`, `repro_tid_alias.c`, `repro_once.c`, `repro_tid_ctl.c`
  (issue 1 and its control), `repro_group.c` (issue 2),
  `repro_workq_loop.c` (issue 3, standalone harness), `repro_runloop.c`
  (issue 4);
- `bugzilla/*.patch` and `libdispatch-freebsd-lock-fixes.diff` - the fixes
  as diffs against the port's patched source tree, for review;
- `Makefile` and `run.sh` - `make run` builds and runs the whole
  matrix against the installed library and prints PASS/FAIL per
  case; `DISPATCH_LIB=/path/to/build make run` runs it against a
  rebuilt `libdispatch.so`.
