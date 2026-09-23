# Deferred work in kernels: a survey for the shared worker pool question

Started 2026-09-23.  Extends the libdispatch study (DISPATCH.md) to the
literature and to other kernels: how kernel programming differs from a
userspace library, what other operating systems built for "many
components share a pool of worker contexts", and where they went wrong.
Sources are linked inline; FreeBSD facts are from the local `releng/15.0`
tree and its git history.  Quotes are verbatim unless marked (paraphrase).

## 1. Why a kernel pool is not a userspace pool

The differences are well documented and consistent across systems.

**Constraints**

- **Sleeping is a property of the context, not of the code.**  Linux:
  "You cannot call any routines which may sleep, unless: You are in user
  context.  You do not own any spinlocks.  You have interrupts enabled"
  ([Unreliable Guide To Hacking The Linux Kernel](https://docs.kernel.org/kernel-hacking/hacking.html)).
  FreeBSD: spin mutexes "never block", "a thread that holds a spin mutex
  must never yield its CPU", and "Interrupt handlers should not sleep or
  use a sleepable lock to avoid starving another interrupt handler"
  ([locking(9)](https://man.freebsd.org/locking/9),
  [SMPng design document](https://docs.freebsd.org/en/books/arch-handbook/smp/)).
  NetBSD's soft interrupts "may block on synchronization objects" but "it's
  not valid for a software interrupt to sleep on condition variables or to
  wait for resources to become available (for example, memory)"
  ([softint(9)](https://man.netbsd.org/softint.9)).  Windows: a DPC "must
  not make blocking calls"; a work item on a system worker thread "can
  contain blocking calls"
  ([System Worker Threads](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/system-worker-threads)).
  A pool API must therefore encode the context class of its workers in
  the type of the queue, not leave it to the caller's discipline.
- **Stacks are small and shared.**  Linux: "about 14K on most 64-bit
  archs, and often shared with interrupts".  FreeBSD amd64:
  `KSTACK_PAGES 4` (16 KB, 24 KB with KASAN/KMSAN), arm64 the same
  ([amd64/include/param.h](https://github.com/freebsd/freebsd-src/blob/af58d0db156a4036624d7f8a0bdd4b739a5418d2/sys/amd64/include/param.h#L128-L134)).
  A pool handler that calls into a protocol stack (if_pair's workers run
  the peer's `ip_input`) inherits the deepest path of that stack; the
  worker's own frame must stay shallow, and recursion through the pool
  (a handler that enqueues to itself and drains synchronously) is out.
- **Allocation can fail and must not be needed on the hot path.**  Linux
  `GFP_ATOMIC` "doesn't sleep... but less reliable"; "You should really
  have a good out-of-memory error-handling strategy".  illumos taskq
  states the contract outright: "TQ_NOSLEEP dispatches on dynamic task
  queues are always allowed to fail"
  ([os/taskq.c](https://github.com/illumos/illumos-gate/blob/master/usr/src/uts/common/os/taskq.c)).
  libdispatch's `dispatch_async` never fails because it can allocate; a
  kernel enqueue takes an intrusive, caller-owned item and returns
  `ENOBUFS` at a bound.
- **Memory reclaim can depend on the pool.**  Linux learned this the hard
  way: "All wq which might be used in the memory reclaim paths MUST have
  this flag set", `WQ_MEM_RECLAIM`, which guarantees "at least one
  execution context regardless of memory pressure" via a rescuer thread
  ([Concurrency Managed Workqueue](https://docs.kernel.org/core-api/workqueue.html)).
  For FreeBSD this is the GELI case: a disk-encryption worker sits on the
  swap and root I/O path.  Any shared pool that GELI joins must reserve
  forward progress for it; otherwise memory pressure that needs GELI to
  write, while GELI waits for a pool thread that is blocked allocating,
  deadlocks.
- **Priority is a system resource.**  In userspace a pool's threads are
  arbitrated by the scheduler above them; a kernel pool at interrupt-class
  priority *is* the top of the hierarchy (DISPATCH.md S4).  FreeBSD's
  ithreads "run at real-time kernel priority" (SMPng); Windows DPCs
  preempt "the execution of all threads, and cannot be preempted by a
  thread or by another DPC"
  ([Introduction to Threaded DPCs](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/introduction-to-threaded-dpcs)).
  Whatever the pool does not yield, nothing below it ever gets.

**Opportunities**

- **Placement is controllable.**  The kernel knows the CPU, the cache
  topology and the NUMA node; it can pin a worker and steer an item to it.
  Userspace pools can only hint.  Linux's unbound-workqueue "affinity
  scopes" (`cpu`, `smt`, `cache`, `numa`, `system`) exist because the
  kernel can honour them
  ([A pair of workqueue improvements](https://lwn.net/Articles/937416/)).
- **Preemption is controllable.**  `critical_enter(9)`, `sched_pin`,
  `sched_bind`, priority lending: the pool can *make* its worker
  non-preemptible for a bounded slice, or demote itself to let softclock
  run (if_pair's `kern_yield(PRI_USER)` then `sched_prio(PI_NET)`).
- **The producer is known.**  Items arrive from ithreads, callouts,
  netisr or another worker, never from an untrusted caller; the pool can
  trust the CPU argument and the class registration.
- **Whole-system accounting.**  The pool can meter cycles per client
  per CPU with the TSC and expose them (DTrace SDT, sysctl) at no
  marginal cost, which a library cannot do without the kernel's help.

## 2. The recurring problem, and its two classic failure modes

Every kernel grows the same thing: work that must leave the context it
was created in.  Linux `Documentation` lists the reasons; illumos taskq.c
lists five ("non-time-critical tasks, lock conflicts, blocking
operations, completion dependencies, and parallel task launching").  Two
failure modes recur across four decades:

1. **Proliferation.**  Each subsystem builds its own threads.  Linux's
   original multi-threaded workqueues had "one worker thread per CPU"
   *per workqueue*, so "Each wq maintained its own separate worker pool",
   wasting resources while "proneness to deadlocks around the single
   execution context" remained ([workqueue.rst](https://docs.kernel.org/core-api/workqueue.html)).
   FreeBSD today: if_pair, epair (RSS kernels), if_wg, GELI, iflib each
   create `ncpu` pinned threads (NOTES.md, 2026-09-19 survey).
2. **Starvation and deadlock through sharing.**  A single shared pool
   with a bounded thread count deadlocks when work blocks on work.
   Windows documents it plainly: "Because the pool of system worker
   threads is a limited resource, WorkItem and WorkItemEx routines can be
   used only for operations that take a short period of time.  If one of
   these routines runs for too long... or waits for too long, the system
   can deadlock", so "if a driver requires long periods of delayed
   processing, it should instead call PsCreateSystemThread to create its
   own system thread".  Linux: "avoid system workqueues if your workload
   might saturate a system wq and potentially lead to deadlock".  OpenBSD's
   `taskq_barrier` callers "must not hold locks that can block the taskq.
   Otherwise, the system will deadlock" ([task_add(9)](https://man.openbsd.org/task_add.9)).
   illumos: "Do NOT call taskq_wait() from a task: it will cause deadlock."

The whole design space is the attempt to get out of (1) without falling
into (2).

## 3. What other systems built

### Linux: cmwq, softirqs, and the long retreat from both extremes

- **Concurrency Managed Workqueue (2010, Tejun Heo).**  "per-CPU unified
  worker pools shared by all wq to provide flexible level of concurrency
  on demand."  The mechanism that makes sharing safe: when a worker
  sleeps, the pool "immediately schedules a new worker so that the CPU
  doesn't sit idle while there are pending work items", keeping active
  workers "minimal but sufficient".  Blocking work therefore does not
  strand the queue - the failure mode of Windows' fixed pool - at the
  cost of scheduler hooks in the sleep path.  `WQ_CPU_INTENSIVE` (work
  that hogs the CPU "do not contribute to the concurrency level") and,
  since 2023, automatic detection: a worker running "for 10ms
  continuously" is flagged CPU-intensive, because "developers often
  forget to set the WQ_CPU_INTENSIVE flag" ([LWN 937416](https://lwn.net/Articles/937416/)).
- **Bound vs unbound, and the affinity conundrum.**  "Unless work items
  are expected to consume a huge amount of CPU cycles, using a bound wq
  is usually beneficial"; unbound queues trade locality for utilisation,
  and the 2023 affinity-scope work made the trade explicit: "The tradeoff
  between efficiency gains from improved locality and bandwidth loss from
  work-conservation deficit poses a conundrum" (Heo, LWN 937416).  One
  tester "reported no improvements despite trying the changes".  This is
  the per-CPU-pinned versus stealable choice in our sketch, with the
  honest upstream verdict that no default wins everywhere.
- **Softirqs and tasklets: the same-CPU deferral, and its costs.**
  Softirqs give Linux the cheap same-CPU queued delivery that FreeBSD's
  SWI threads do not (NOTES.md, 2026-09-07).  But "Software interrupts
  are hard for system administrators to manage and can create surprising
  latencies if they run for a long time", and their time "is nearly
  invisible to the scheduler" ([Threaded NAPI polling, LWN 833840](https://lwn.net/Articles/833840/)).
  Hence **threaded NAPI** (5.12): move network polling into kernel
  threads because "A kernel thread can have its priority changed, and it
  can be bound to a specific set of CPUs", accepting "a possible slight
  increase in overhead".  And hence the **end of tasklets** (2024): Linus
  on the API ("tasklets just should not be used in that way"), the
  community on latency ("can create surprising latencies in the kernel"),
  replaced by `WQ_BH` workqueues that "run quickly, in atomic context, on
  the same CPU" but through the workqueue API and its instrumentation
  ([The end of tasklets, LWN 960041](https://lwn.net/Articles/960041/);
  [BH workqueues](https://lwn.net/Articles/960020/)).  Heo kept the fast
  same-CPU class alive because moving everything to threads "doesn't help
  the cases where the shortest latency is required".

  Reading: Linux converged on exactly the class split in our sketch - a
  non-sleeping same-CPU class (BH), a per-CPU bound class, an unbound
  stealable class, and a rescuer guarantee for reclaim - after fifteen
  years of first having too many pools and then too few.

### Solaris / illumos: taskq and squeues

- **taskq(9F)** is the general facility, with the design comment as
  primary source ([os/taskq.c](https://github.com/illumos/illumos-gate/blob/master/usr/src/uts/common/os/taskq.c)).
  Static taskqs: fixed thread count, entries kept "between 'minalloc' and
  'maxalloc'" so that dispatch need not allocate; `TASKQ_PREPOPULATE` for
  critical clients.  **Dynamic taskqs** exist because "The task queues
  are very hot and there is a need to avoid data and lock contention over
  global resources" and "Some tasks may block for a long time, and this
  should not block other tasks in the queue": one thread per task, created
  on demand, distributed over CPU-count buckets "to split resources and
  reduce resource contention" (threads "are not bound to any CPU" despite
  the bucket numbering), self-destroying after `taskq_thread_timeout`
  (five minutes) so that "thread explosion" is bounded by idle timeouts
  rather than prevented.  Dispatch is allowed to fail and callers must
  cope; `TQ_NOQUEUE` opts out of the backlog.  `TASKQ_THREADS_CPU_PCT`
  resizes with CPU hot-plug.  Lessons stated in the file: dependent tasks
  need `TASKQ_DYNAMIC` or they deadlock; never wait for a taskq from
  inside it.
- **squeues** ("vertical perimeter", FireEngine, Solaris 10) are the
  network-specific answer and the closest ancestor of the NET class
  ([inet/squeue.c](https://github.com/illumos/illumos-gate/blob/master/usr/src/uts/common/inet/squeue.c)).
  "a general purpose high-performance serialization mechanism" - per-CPU
  queues, "strictly FIFO", "only one thread can process it at any given
  time"; a connection is bound to one squeue for life, so all of its
  packets serialize without locks.  The drain policy is the interesting
  part: the enqueuing thread may process inline "if nothing is queued",
  and the squeue "imposes a finite time limit for which a external thread
  can do processing after which it switches processing to its own worker
  thread" (`squeue_drain_ms = 20`).  Under backlog the stack "turns off
  the interrupts and switches to poll mode", pulling packet chains with a
  byte budget (`squeue_poll_budget_bytes = 150000`).  Crossbow later
  generalised this into per-flow receive rings with poll and worker
  threads, "a contention free path without the need for any fine grained
  locks", where "as soon as any backlog starts to build up, the hardware
  receive ring is switched to the poll mode and acts as the only queue in
  the path" ([Crossbow, WREN 2009](https://conferences.sigcomm.org/sigcomm/2009/workshops/wren/papers/p45.pdf)).

  Reading: squeues are the proof that "one flow, one CPU-bound queue,
  one drainer at a time, time-bounded inline drain then hand-off" scales
  a production TCP stack; the 20 ms inline limit is a time quantum, and
  the hardware ring as the only queue is backpressure done at the source.

### NetBSD: softint(9) and threadpool(9)

- **softint(9)** (2007, Andrew Doran) is the missing FreeBSD primitive:
  four fixed levels ("clock, bio, net, serial"), "Soft interrupt
  scheduling is CPU-local", handlers have thread context and "may block
  on synchronization objects, sleep, and resume execution at a later
  time" but must not "wait for resources to become available (for
  example, memory)".  Fixed levels, not client-chosen priorities.
- **threadpool(9)** (2014/2018, Taylor Campbell) is a shared pool for
  "medium- to long-term actions, called jobs, which can be scheduled from
  contexts that do not allow sleeping": "For each priority level, there is
  one unbound thread pool, and one collection of per-CPU thread pools",
  reference-counted so subsystems share instances.  Scheduling a job
  "does not allocate or even sleep at all, except perhaps on an adaptive
  lock"; workers time out after 30 s idle; a dispatcher thread per pool
  hands jobs to workers.  The author's own caveat in the source is worth
  quoting for our design: one dispatcher per CPU was chosen "to avoid
  touching remote CPUs' memory when scheduling a job, but that still
  requires interprocessor synchronization" - the per-CPU-everything
  instinct does not remove cross-CPU cost, it relocates it
  ([kern_threadpool.c](https://github.com/NetBSD/src/blob/trunk/sys/kern/kern_threadpool.c),
  [threadpool(9)](https://man.netbsd.org/threadpool.9)).

### OpenBSD: taskq(9)

The minimal shape: `taskq_create(name, nthreads, ipl, flags)`, tasks may
be added "during autoconf, from process context, or from interrupt
context", system queues `systq`/`systqmp` "can therefore provide
predictable ordering of work" because they are single-threaded, and the
barrier contract carries the standard deadlock warning.  No per-CPU
pools, no dynamic sizing: OpenBSD chose simplicity and accepts the
proliferation cost ([task_add(9)](https://man.openbsd.org/task_add.9)).

### Windows: DPCs, work items, threaded DPCs

Three tiers with hard rules: DPCs at `DISPATCH_LEVEL` (no blocking, no
preemption, "every thread will remain paused for an arbitrarily long
time" if DPCs pile up - "each ordinary DPC increases system latency,
which can hurt the performance of time-sensitive applications, such as
audio or video playback"); **threaded DPCs** (Vista) at `PASSIVE_LEVEL`,
preemptible by ordinary DPCs but not by threads, still bound by DPC
restrictions; and **system worker threads** (`ExQueueWorkItem`,
`IoQueueWorkItem`) for blocking work, from a limited shared pool with the
deadlock warning quoted above and the rule "create your own system thread"
for long work.  A telling detail: "Many existing device stacks, such as
networking and USB, do not support threaded DPC processing", so the
latency fix could not be applied where it mattered most.  Windows also
documents the idempotent-requeue pattern we use ("The driver queues the
work item only if the task list was previously empty") as the way to
avoid "corruption of system data structures" from double queueing.

### DragonFly BSD: per-CPU threads and message passing

The most radical answer: "one thread per protocol per processor",
connected by "a light weight message-and-port subsystem which is
primarily used to shove packets around in bulk in the network subsystem
and to cpu-localize network operations", with serializing tokens instead
of mutexes ("any blocking condition... will temporarily release ALL held
tokens while the thread is blocked") and critical sections "primarily
used to protect against IPIs, allowing cpu localized code to run very
efficiently" ([Locking and Synchronization](https://www.dragonflybsd.org/docs/developer/Locking_and_Synchronization/)).
Flow ownership by CPU eliminates the per-connection lock model that
costs FreeBSD its tcpinp spin time (t_20), at the price of a message hop
for anything crossing CPUs and a stack rewritten around ownership.  It
is the design if_pair's steering approximates from outside the stack.

### Darwin: libdispatch and its kernel side

Covered in DISPATCH.md.  The kernel-side lesson is that Darwin gave the
userspace pool what a kernel pool needs and a library cannot build: the
kernel sizes the pool from measured runnability (`pthread_workqueue`),
and the thread that does not exist on other platforms is replaced by a
1 Hz user-level poll of `/proc` or `kern.proc` (S3).

### FreeBSD's own history

The pieces already exist; the service does not.

| year | commit | what it added | what it did not |
|---|---|---|---|
| 2000 | `ca2e05343b60` (dfr) taskqueue(9) | "easy-to-use SWIs among other things": one queue, one context | no per-CPU, no priority classes |
| 2000-05 | SMPng | interrupt threads: "Providing a context for interrupt handlers allows them to block on locks" | ithreads at real-time priority became the top of the hierarchy that later starved softclock |
| 2008 | `8d809d5061e3` per-CPU callout wheels | timers scale per CPU | TCP still put every timer on CPU 0 without RSS (per_cpu_timers, NOTES) |
| 2009 | `d4b5cae49bff` (rwatson) netisr rewrite | per-CPU workstreams, per-protocol ordering policy (`SOURCE`, `FLOW`, `CPU`), per-workstream queue limits, "batched dequeue" | shipped with "a single, un-bound worker thread" by default, `net.isr.maxthreads=1` RDTUN; no batch/yield governor |
| 2015 | `bfa102cae1d4` (adrian/jhb) `taskqueue_start_threads_cpuset()` | pinned taskqueue threads, "a push towards NUMA awareness in drivers" | still one taskqueue per driver |
| 2016 | `23ac9029f96b` (shurd) gtaskqueue | per-CPU task groups for iflib (`TASKQGROUP_DEFINE`), attach/detach/drain | fixed `PI_SOFT`, no classes, no quanta; wg created a private group rather than share |
| 2018 | `06bf2a6aefbf` (mmacy) epoch(9) | preempt-safe read-side sections, `NET_TASK_INIT` wraps tasks in the network epoch | epoch callbacks run on the shared `qgroup_softirq` that a saturated pool can starve |

The 2009 netisr commit is the closest FreeBSD came to the shared NET
class: per-CPU workstreams, protocol-declared ordering policies, flow
placement, queue limits per workstream, batched dequeue.  It stopped at
the default: one unbound thread, boot-time knobs, no yield discipline -
which is exactly why if_pair brings its own pool (NOTES.md).

## 4. Mistakes to learn from

1. **Fixed shared pools deadlock; per-client pools proliferate.**  Windows
   and early Linux hit the first, FreeBSD lives with the second.  The only
   designs that escaped both manage concurrency actively: cmwq spawns a
   worker when one blocks; illumos dynamic taskq spawns per task and
   reaps on idle; Darwin sizes from measured runnability.  Our sketch
   dodges the problem by construction - NET and BULK never block - and
   confines it to the BLOCKING class, which must then get cmwq-style
   management or reserved threads, not a fixed count.
2. **Reclaim paths need guaranteed progress.**  `WQ_MEM_RECLAIM`'s
   rescuer is a scar.  GELI in a shared pool needs the equivalent.
3. **Invisible work is unmanageable work.**  Linux spent a decade moving
   from softirqs to threads (threaded IRQs, threaded NAPI, BH workqueues)
   because administrators and the scheduler could not see, bound or
   prioritise softirq time.  FreeBSD already has threads everywhere; the
   pool must not lose that visibility by hiding clients behind one thread
   name - per-client accounting is a requirement, not a nicety.
4. **Priority chosen by the client is a tragedy of the commons.**  Linux
   `WQ_HIGHPRI`, Solaris `pri`, FreeBSD `PI_NET` for every driver: everyone
   picks the top.  NetBSD softint's four fixed levels and libdispatch's
   closed QoS set are the alternative.  Classes, not priorities.
5. **Count budgets fail; time budgets work.**  Mogul & Ramakrishnan's
   polling thread gives each callback "a quota on the number of packets
   they are allowed to" process and round-robins among sources, and they
   add a second mechanism because count alone does not guarantee
   user-level progress: the system detects "that, over some interval, it
   has spent too much time processing packet input and output events, and
   temporarily disable interrupts to give higher protocol layers and user
   processes time to run" ([Eliminating Receive Livelock, USENIX 1996](https://www.usenix.org/legacy/publications/library/proceedings/sd96/mogul.html)).
   Their requirements list is ours: "acceptable system throughput,
   reasonable latency and jitter (variance in delay), fair allocation of
   resources, and overall system stability".  if_pair's batch counter
   failed on 64 KB chains and grew the tick check; squeue drains for 20 ms;
   Linux flags 10 ms.  Time is the unit.
6. **Feedback from the output side.**  Mogul's "Temporarily disabling
   input when feedback from a full queue, or a limit on CPU usage,
   indicates that other important tasks are pending" and Crossbow's
   "hardware receive ring... acts as the only queue in the path" say the
   same thing: do not accept work you cannot finish; leave it where it is
   cheapest to leave.  A pool that never returns `ENOBUFS` is a pool that
   grows queues without bound.
7. **Process at the receiver's priority (LRP).**  Druschel and Banga's
   Lazy Receiver Processing demultiplexes early and does protocol work
   "on behalf of" the receiving process at its priority, giving "improved
   fairness, stability, and increased throughput under high network load"
   ([LRP, OSDI 1996](https://www.usenix.org/conference/osdi-96/lazy-receiver-processing-lrp-network-subsystem-architecture-server-systems)).
   The pool cannot know the receiving process, but per-client weights
   within a class are the coarse version, and it argues against letting
   one client's flood run at the same priority as everyone's.
8. **Adaptive controllers are possible but not free.**  SEDA's stages
   each get a thread pool and an event queue, with "control mechanisms for
   automatic tuning and load conditioning, including thread pool sizing,
   event batching, and adaptive load shedding" - the batching controller
   trades throughput against latency by observing the stage's own
   throughput ([SEDA, SOSP 2001](https://people.eecs.berkeley.edu/~brewer/papers/SEDA-sosp.pdf)).
   Linux's 10 ms auto-detection is a controller of this kind and was
   added only after "developers often forget" the manual flag.  Start
   with fixed quanta and counters that would let a controller be written
   later; do not start with the controller.
9. **Pinning is not free either.**  Heo's "conundrum" and Campbell's
   "still requires interprocessor synchronization" are the two honest
   statements that per-CPU everything moves the cost rather than
   removing it.  Keep NET pinned (ordering needs it), make BULK's
   stealability a flag, and measure - our own t_19/t_21 falloff is a
   locality-versus-utilisation question, not a locking one.
10. **Infrastructure without consumers calcifies.**  RSS in FreeBSD
    (NOTES.md), netisr's per-CPU mode nobody turns on, threaded DPCs the
    network stack never adopted.  Ship the pool with its first two
    converted clients or not at all.

## 5. What this changes in the sketch (DISPATCH.md S10, NOTES.md)

- **BLOCKING is not "a class with more threads".**  It needs either
  cmwq-style concurrency management (spawn on block, reap on idle, a
  ceiling) or per-client reserved threads, and a reclaim guarantee for
  clients on the I/O path.  Until that exists, GELI keeps its own pool and
  the design says so.
- **Backpressure is part of the API contract**, not a failure: bounded
  queues, `ENOBUFS`, and a way for a client to leave work at the source
  (the NIC ring, the socket buffer) rather than in the pool.
- **Time quanta and the demote-yield are the load-bearing mechanism**,
  with a count budget only as a cheap inner check.  Expose expiries per
  client so the 10 ms-style auto-flagging can come later.
- **Per-client per-CPU cycle accounting is mandatory**, because every
  system that lacked it eventually rebuilt its deferral mechanism to get
  it.
- **The FreeBSD-native path is an extension of gtaskqueue and netisr's
  workstreams, not a third framework.**  netisr already has the
  per-protocol ordering-policy declaration and per-workstream limits;
  gtaskqueue has the per-CPU groups and lifecycle.  What neither has is
  classes, time-based DRR across clients, the yield, and accounting.

Open questions this survey did not settle: whether epoch callbacks belong
in NET (cannot be starved) or BULK (compete under DRR); how a BLOCKING
class detects a blocked worker on FreeBSD without cmwq's scheduler hook
(candidates: `sched_switch` SDT, a per-thread flag set in `sleepq_add`);
and whether ULE's preemption of a `PI_NET` worker by an ithread of equal
priority needs a policy of its own.
