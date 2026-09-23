# kwq glossary

Canonical terms for KWQ.md, PLAN.txt and the code.  Where FreeBSD has a
term, it wins; terms from other systems appear only as aliases or when an
external work is cited.  Each entry: definition, aliases, where to read
more.  "kwq:" marks terms this project defines.

## A. kwq objects and operations

- **client** (kwq) - the kernel component that owns a queue: it calls
  `kwq_create()`, supplies the handler, enqueues items and decides what a
  reject means.  if_pair, epair, netisr, if_wg and GELI are clients; the
  service never knows what an item is, only the client does.  A client
  is a *producer* when it enqueues and is the code the worker runs when
  it handles - it is never itself a *consumer* (that word is the worker).
  Aliases: *user* ("workqueue user", Linux docs), *subsystem*, *driver*.
  FreeBSD's other uses of *consumer* (GEOM consumers, "callout
  consumers") and *client* (NFS client, mbuf(9) "client of the
  allocator") are unrelated and not used in kwq documents.
- **item** (kwq) - the unit of work: a `struct kwq_item` embedded in an
  object the client owns and links into a queue; the service never
  allocates or frees it.  Aliases: *task* (taskqueue(9), illumos taskq;
  `struct task` is taskqueue's own type and is not an item), *work item*
  (Windows, Linux workqueue), *job* (NetBSD threadpool(9)), *block* /
  *continuation* (libdispatch).  See taskqueue(9), `queue(3)` STAILQ.
- **queue** (kwq) - a client's registration with the service: a name, a
  work class, a handler, per-CPU item lists with a limit, a weight.
  Created by `kwq_create()`, made enqueue-able by `kwq_activate()`, retired
  by `kwq_drain()` + `kwq_destroy()`.  Aliases: *dispatch queue*
  (libdispatch), *workqueue* (Linux, one level up: Linux's per-CPU
  "worker pool" is closer to kwq's class), *taskq* (illumos).
- **handler** (kwq) - the client function the worker calls with one
  pass's list of items: `fn(queue, head, n, ctx)`.  Aliases: *task
  function* (`ta_func`, taskqueue(9)), *callback*, *work function* (Linux).
- **name** (kwq) - the string given to `kwq_create()`; appears in sysctl
  paths, lock names, thread names, DTrace arguments and DDB.  Alias:
  *label* (libdispatch `dq_label`).  FreeBSD uses "name" (`mtx_init(9)`,
  `kthread_add(9)`), so does kwq.
- **work class** (kwq) - one of NET, BULK, BLOCKING: fixes a queue's
  worker set, its scheduler priority, whether handlers may sleep, and
  the default quantum.  Distinct from the scheduler's *scheduling class*
  (below).  Aliases: *QoS class* (libdispatch), *bound/unbound/BH
  workqueue* (Linux, roughly).
- **worker** (kwq) - one kernel thread of the service, bound to one CPU,
  serving one work class: `kwq_net/N`, `kwq_bulk/N`, `kwq_blk/N`.
  Aliases: *taskqueue thread*, *kworker* (Linux), *squeue worker*
  (illumos).  See kthread(9).
- **worker pool** (kwq) - all workers of one work class.  For NET and
  BULK exactly one worker per CPU; for BLOCKING a managed set (see
  *worker replacement*).  Alias: *threadpool* (NetBSD), *worker pool*
  (Linux cmwq).
- **enqueue** - `kwq_enqueue(q, cpu, item)`: append an item to the
  (queue, CPU) list; never sleeps, never allocates; may *reject*.
  Aliases: *dispatch* (illumos, libdispatch), *queue_work* (Linux),
  *task_add* (OpenBSD).
- **steering** - the client's choice of CPU for an item, passed to
  `kwq_enqueue()`: by flow hash (`kwq_cpu_for_hash()`, the same mapping
  TCP's per-CPU timers use), by source CPU, by `curcpu`, or
  `KWQ_CPU_ANY` for unordered BULK work.  The CPU is an ordering key;
  the service never re-steers.  Aliases: *placement*, *RSS bucket*
  (hardware), *m2cpuid/m2flow* (netisr(9) policy functions).
- **reject** (kwq) - an enqueue that returns non-zero and leaves the item
  with the caller: `ENOBUFS` at the queue's *limit*, `ENXIO` after drain
  began.  What the client does next (drop and count, or hold the item
  upstream) is *backpressure*.  See `mbufq_enqueue()` in mbuf(9) for the
  same contract.
- **limit** (kwq) - the maximum number of items per (queue, CPU) list;
  netisr(9) calls the same thing a *queue limit* (`net.isr.maxqlimit`).
  It bounds queueing delay, the client memory the queued items pin, drain
  time, and is the producer's only overload signal - not the queue's own
  memory, which intrusive linkage makes free (KWQ.md S3).
  `KWQ_LIMIT_NONE` declares a queue unbounded on purpose (closed or
  reclamation clients only); its health signal is then `maxdepth`.
  Alias: *bound*, *depth cap* - avoided, "bound" is reserved for CPU
  binding.
- **activate** (kwq) - `kwq_activate()`: make a queue enqueue-able.  A
  queue created with `KWQ_F_INACTIVE` is configured first and activated
  last, so no item can arrive before its handler and storage exist.
  Alias: *dispatch_activate* (libdispatch).  The verb *publish* is used
  in this project only in the memory-ordering sense (making a store
  visible to other CPUs), never for queues.
- **drain** - `kwq_drain()`: deactivate a queue, run (or, with
  `KWQ_F_DISCARD`, discard) every pending item on every CPU, and wait for
  in-progress passes to finish.  Same meaning as `taskqueue_drain(9)` and
  `taskqueue_drain_all()`.  Not to be confused with *flush* (`mbufq_flush()`
  takes a queue's contents; kwq calls that a *swap*) or *quiesce*.
- **swap** (kwq) - the worker's O(1) removal of a (queue, CPU) list under
  its lock, leaving an empty list for producers; the taken list is the
  *batch* for one *pass*.  Implemented with `mbufq_flush()`/`STAILQ`
  concatenation semantics.
- **requeue** (kwq) - `kwq_requeue()`: a handler handing back the items
  of the current pass it did not process, PREPENDED to the current CPU's
  list of the same queue so that items which arrived during the pass do
  not overtake them.  Handler-only; cannot fail; not an *enqueue* (no
  limit check, no doorbell).  Alias: none; earlier project text said
  "re-enqueue the remainder", which would have appended and reordered.
- **scatter** (kwq) - `kwq_scatter()`: split a list of items across the
  workers of the caller's cache domain (BULK only) and call a completion
  function once when the last chunk finishes.  Aliases: *fan-out*,
  *dispatch_apply + dispatch_group_notify* (libdispatch), *parallel
  dispatch* (illumos taskq comment).
- **steal / stealable** - a BULK worker with nothing to do taking a whole
  batch from another CPU's list of a queue created `KWQ_F_STEALABLE`.
  FreeBSD's ULE uses the same word for thread migration (`tdq_steal()`,
  `kern.sched.steal_thresh`).  Alias: *unbound workqueue* (Linux),
  *work-conserving*.  NET queues are never stolen from.
- **doorbell** (kwq) - the conditional wakeup of a CPU's worker performed
  by an enqueue that finds the (queue, CPU) state IDLE, moving it to
  WAKING under the same lock hold as the append, so a wakeup can never be
  lost and is rung once per burst rather than once per item.  Uses the
  kernel *wakeup* primitive (`wakeup_one(9)`).  A *requeue* never rings
  it (the worker is already running that queue).  Aliases: *poke*
  (libdispatch), *kick*.  Inherited from if_pair (../NOTES.md).
- **notifier** (kwq) - `struct kwq_notifier`: a preallocated, client-owned
  item with a pending state kept by kwq and a CPU fixed at init (so one
  (queue, CPU) lock protects the state).  `kwq_notify()` queues it if
  idle and is a no-op if pending; it cannot fail and is exempt from the
  limit.  kwq clears the pending state before the handler runs, so a
  signal arriving during handling causes another pass.  Aliases: *task*
  (taskqueue(9), `ta_pending`), *work item* (Windows), *work_struct*
  (Linux, `WORK_STRUCT_PENDING`).  KWQ.md S2, S3.
- **notifier pattern** (kwq) - using kwq only as the execution context:
  the client keeps its own request list and signals a notifier when the
  list goes from empty to non-empty; the handler drains the list within
  its budget and re-notifies if work remains.  Requests are never kwq
  items, so they are never refused.  KWQ.md S14 (GELI).
- **queue state** (kwq) - per (queue, CPU): IDLE (no worker will look
  until a doorbell), WAKING (doorbell rung, worker not yet running the
  queue), RUNNING (worker holds a batch from it), PARKED (deficit
  negative; skipped until it recovers).
- **reserved worker** (kwq) - a BLOCKING worker dedicated to one queue
  (`KWQ_F_RESERVE`, `nreserve`) so a client on the memory-reclaim or root
  I/O path always has an execution context.  Alias: *rescuer thread*
  (Linux `WQ_MEM_RECLAIM`).
- **worker replacement** (kwq) - the BLOCKING pool's policy of starting
  another worker when one blocks, up to a ceiling, and retiring idle ones
  after a timeout.  Alias: *concurrency management* (Linux cmwq),
  *dynamic taskq* (illumos, per-task threads).  First implemented by the
  *pool monitor*, later by a scheduler hook (KWQ.md S11).
- **pool monitor** (kwq) - a periodic (1 Hz) check of how many BLOCKING
  workers on a CPU are runnable, driving worker replacement without a
  scheduler hook.  Alias: *workqueue monitor* (libdispatch's
  `_dispatch_workq_monitor_pools`).

## B. Execution contexts and threads (FreeBSD terms)

- **kernel thread** - a thread with no user address space, created with
  `kthread_add(9)` (in a kernel process) or `kproc_create(9)`.  kwq
  workers are kernel threads in one kernel process per class.  Aliases:
  *kthread*, *system thread* (Windows).
- **interrupt thread (ithread)** - the thread context in which most
  interrupt handlers run under SMPng; may block on sleep mutexes, must
  not sleep.  Priority range `PI_*`.  May enqueue to kwq.  See
  bus_setup_intr(9), the SMPng design document.
- **interrupt filter** - a handler run in *primary interrupt context*
  (interrupts disabled for the source, no thread switch), allowed only
  spin mutexes; schedules an ithread with `FILTER_SCHEDULE_THREAD`.
  Aliases: *fast interrupt handler* (old FreeBSD), *hardirq handler*
  (Linux), *ISR* (Windows).  May enqueue to a `KWQ_F_SPIN` queue only.
- **software interrupt thread (SWI)** - a thread scheduled by
  `swi_sched(9)` to run deferred work at `PI_SOFT`; softclock and netisr
  are SWIs.  Aliases: *softirq* (Linux; not a thread), *softint(9)*
  (NetBSD), *DPC* (Windows), *tasklet* (Linux, retired).  FreeBSD has no
  non-thread softirq; kwq's NET class is the thread-based analog.
- **critical section** - `critical_enter(9)`/`critical_exit(9)`: prevents
  preemption of the current thread (interrupts still run); makes
  CPU-local data safe without a lock.
- **network epoch** - the `net_epoch_preempt` section (epoch(9),
  `NET_EPOCH_ENTER()`) that protects network stack data structures from
  reclamation while a thread reads them; NET handlers run inside it, one
  entry per pass.  Alias: *RCU read-side critical section* (Linux).
- **netisr** - FreeBSD's protocol-input dispatch framework, netisr(9):
  per-CPU *workstreams* holding per-protocol queues, with per-protocol
  ordering policies (`NETISR_POLICY_SOURCE/FLOW/CPU`) and *direct
  dispatch* (run the protocol input in the caller's context, the default).
  The first in-tree candidate for the NET class.
- **taskqueue / gtaskqueue** - FreeBSD's existing deferred-work
  facilities: taskqueue(9) (one queue, its own threads, sleep or spin
  mutex) and gtaskqueue (`TASKQGROUP_DEFINE`, per-CPU groups of
  *grouptasks*, used by iflib and if_wg).  kwq is built as an extension of
  the latter.

## C. Scheduling and priority (FreeBSD terms)

- **scheduling class** - the scheduler's own classification of a thread,
  `td_pri_class`: `PRI_ITHD`, `PRI_REALTIME`, `PRI_TIMESHARE`, `PRI_IDLE`.
  Not a kwq *work class*; kwq workers are `PRI_ITHD` (NET, BULK) or
  `PRI_TIMESHARE` at `PUSER` (BLOCKING, where GELI's workers run today).
- **priority range** - a numeric band of `td_priority`: interrupt threads
  0-47 (`PI_*`, e.g. `PI_NET` = 1, `PI_SOFT` = 2), real-time 8-39 as
  rtprio, kernel 40-55 (`PRI_MIN_KERN`..`PRI_MAX_KERN`), timeshare 56-223
  (`PUSER` = 56 is the best), idle 224-255.  See `sys/priority.h`.  The
  word *band* is avoided.
- **bound (CPU-bound thread)** - a thread restricted to one CPU with
  `sched_bind(9)` or a single-CPU cpuset; kwq workers are bound.  Alias:
  *affinitized*, *pinned* (Linux, libdispatch; in FreeBSD *pinned* means
  `sched_pin(9)`, a temporary hold on the current CPU by the running
  thread, so it is not used for workers).
- **preemption** - a higher-priority runnable thread taking the CPU from
  the running one; ULE preempts immediately only for interrupt-thread
  priorities (`preempt_thresh`).  A NET worker is preempted by nothing
  but other ithreads and real-time threads.
- **yield** (kwq) - the worker's voluntary switch at the end of a round:
  `kern_yield()` to the class's fixed `yield_prio` (default `PUSER`),
  then `sched_prio()` back to the class priority.  Never
  `kern_yield(PRI_USER)`: that resolves to `td_user_pri`, which ULE
  recomputes every tick from the worker's own run/sleep history, so it
  yields to nobody useful when lightly loaded and to every CPU-bound
  user thread for a full slice when saturated (KWQ.md S11 Missing 1).  Aliases: *relinquish* (`sched_relinquish(9)` is the
  no-demotion variant), *demote-yield* (earlier project text),
  *cond_resched* (Linux).
- **CPU-share cap** (kwq) - the substitute for a bounded yield: a worker
  that has been busy more than `cap_pct` of a window while user threads
  are runnable on its CPU sleeps for `cap_sleep_us` with `pause_sbt(9)`.
  From Mogul & Ramakrishnan's "limit on CPU usage" feedback (../POOLS.md).
- **tick** - one `hz` clock interrupt; `ticks` counts them.  kwq's *tick
  guard* forces a yield when a tick has elapsed since the last one, the
  safety net for over-long passes (inherited from if_pair's governor).
- **time slice** - ULE's allotment for a *timeshare* thread before
  `sched_slice` forces a switch (~10 ms at hz=1000).  Not a kwq term;
  kwq's unit is the *pass* and its allotment the *quantum*.

## D. Locking and synchronization (FreeBSD terms, locking(9))

- **sleep mutex** - the default `mtx_init(9)` mutex: adaptive spinning
  then turnstile blocking; a holder may be preempted; may not be held
  while sleeping.  kwq's default (queue, CPU) lock.  Alias: *mutex*,
  *adaptive mutex*, *blocking mutex*.
- **spin mutex** - `MTX_SPIN`: disables interrupts, never blocks, legal in
  interrupt filters.  Selected for a queue by `KWQ_F_SPIN`.
- **turnstile** - the kernel's priority-propagating wait queue behind
  contended mutexes and rwlocks; why a lock beats a lock-free list under
  preemption (KWQ.md S9).  Alias: *priority inheritance*.
- **sleep** - `tsleep(9)`/`msleep(9)`/condvar waits and anything that may
  call them (`M_WAITOK`, sx/lockmgr, `copyin`).  Forbidden in NET/BULK
  handlers, asserted by `THREAD_NO_SLEEPING()`.  Blocking on a sleep
  mutex is *not* sleeping.
- **lock-free** - a data structure updated with atomics and no lock
  (Concurrency Kit's `ck_ring`, libdispatch's `os_mpsc`).  Not used in
  kwq's first version (KWQ.md S9).
- **publish** - in this project only: making a store visible to other
  CPUs in the right order (release/acquire); the second step of a
  lock-free push.  Never used for queue activation.
- **WITNESS** - the lock-order and sleep-safety checker (witness(4));
  `WITNESS_WARN()` is used at handler entry/exit.

## E. Queueing, accounting and fairness

- **batch** (kwq) - the list of items one *swap* takes from a (queue,
  CPU) list, handed whole to one *pass*.  Not the count budget: if_pair's
  `net.link.pair.batch` sysctl (a packet count) is superseded by the
  *quantum*.
- **pass** (kwq) - one handler invocation on one batch by one worker;
  bracketed by `cpu_ticks()` and charged to the queue.  Alias: *slice*
  (earlier project text; avoided because ULE's *time slice* is unrelated),
  *drain* (illumos squeue).  Probes: `kwq:::pass-start`, `kwq:::pass-end`.
- **round** (kwq) - one traversal by a worker of its CPU's active queues
  of its class, each receiving up to its *quantum*; ends with the *yield*.
- **quantum** (kwq) - the CPU time a queue may consume per round, per
  class sysctl (`kern.kwq.<class>.quantum_us`), multiplied by the queue's
  *weight*.  Alias: *budget* is the remaining quantum within a pass
  (`kwq_budget_left()`), not a synonym.
- **budget** (kwq) - the part of a queue's quantum not yet consumed in
  the current pass, as returned by `kwq_budget_left()` in nanoseconds; 0
  tells a handler to requeue its leftovers and return.  Not a synonym
  for *quantum* (the per-round allotment) and unrelated to if_pair's
  count-based `net.link.pair.batch`.
- **deficit round robin (DRR)** - the fair scheduler kwq's workers run:
  each active queue's *deficit* grows by one quantum per round and shrinks
  by the CPU time its passes consume; a queue is served while its deficit
  is positive.  Shreedhar & Varghese, SIGCOMM 1995.  In kwq the resource
  is CPU time, not bytes.
- **overrun** (kwq) - a pass that consumed more than the queue's
  remaining quantum; counted per (queue, CPU) as `overruns`, probe
  `kwq:::overrun`.  Alias: *expiry* (earlier project text).  A persistent
  overrunner should requeue its remainder when `kwq_budget_left()`
  reaches 0.
- **new list** (kwq) - the worker's list of queues that just went from
  empty to non-empty; served before the ring for one quantum, after which
  the queue joins the ring.  Alias: *sparse flow* / *new flows* (fq_codel,
  RFC 8290; dummynet `dn_sched_fq_codel.c`).  KWQ.md S15.
- **park** (kwq) - skipping a queue whose deficit went negative until
  later rounds restore it; state PARKED, probe `kwq:::park`.
- **weight** (kwq) - integer 1..8 scaling a queue's quantum within its
  class; may only be set below other queues' default share.
- **queueing delay** (kwq) - the age of the oldest item at the start of a
  pass; reported as `oldest_age_ns`/`maxlat_ns`.  Alias: *latency* (the
  general word; kept for end-to-end statements).
- **producer / consumer** - the thread that enqueues an item / the
  worker that runs the pass.  Multi-producer, single-consumer per
  (queue, CPU).  Alias: *enqueuer*.
- **open / closed arrivals** - queueing-theory terms used in KWQ.md S3:
  *open* when producers are outside the machine and do not wait for
  service (network receive), so overload must be shed by discarding;
  *closed* when each producer waits for its previous request before
  issuing another (disk I/O through GEOM, the pager), so outstanding
  requests are bounded by kernel resource limits and must not be
  discarded.  A third practical case, *idempotent* deferred work (one
  item per device queue), is bounded by construction; a fourth,
  *reclamation* work (the item is a resource to release; unpaced and
  undroppable), must never travel as droppable items and uses the
  notifier pattern or stays out of kwq.  Mixed queues (if_pair: closed
  TCP plus open UDP) are designed for their open component.  A queue
  whose items are the very memory they pin (one intrusive item per object
  awaiting release) may be declared unbounded with `KWQ_LIMIT_NONE`; its
  health signal is then `maxdepth` instead of `rejected`.  The kwq
  *limit* is flow control for open clients only: signals travel by
  `kwq_notify()` (limit-exempt) and closed or reclamation items under
  `KWQ_LIMIT_NONE`, so only open producers ever see `ENOBUFS`.
- **backpressure** - refusing new work at the source (the NIC ring, the
  socket buffer) when a queue is at its limit, so that queues stay
  bounded; the client's response to a *reject*.
- **fast path** - the per-item code that must not allocate, sleep or take
  more than one lock: `kwq_enqueue()` and the swap.  Alias: *hot path*,
  *data path*.

## F. Observability

- **SDT** - statically defined tracing, the DTrace provider mechanism for
  kernel code (`SDT_PROVIDER_DEFINE`, `SDT_PROBE_DEFINEn_XLATE`); kwq's
  provider is `kwq`.  See dtrace_sdt(4), sdt(9).
- **translator** - a D definition (`/usr/lib/dtrace/kwq.d`) mapping a
  kernel struct to a stable script-facing type; kwq's is `kwqinfo_t`.
- **counter(9)** - per-CPU 64-bit counters; kwq's per-(queue, CPU) and
  per-(class, CPU) statistics under `kern.kwq`.
- **lockstat** - the DTrace provider for lock contention; sees kwq's
  mutexes by name (`"kwq <name>"`).

## G. Terms from other systems, mapped

| elsewhere | system | kwq / FreeBSD term |
|---|---|---|
| work item, work struct | Linux workqueue, Windows | item |
| task, taskq_ent | illumos taskq | item |
| job | NetBSD threadpool | item |
| block, continuation, dispatch_object | libdispatch | item |
| workqueue (WQ_*) | Linux | queue (roughly) |
| worker pool, kworker | Linux cmwq | worker pool, worker |
| bound / unbound workqueue | Linux | not stealable / `KWQ_F_STEALABLE` |
| WQ_BH, softirq, tasklet | Linux | SWI (FreeBSD), NET class (kwq) |
| rescuer, WQ_MEM_RECLAIM | Linux | reserved worker, `KWQ_F_RESERVE` |
| concurrency management | Linux cmwq | worker replacement |
| affinity scope | Linux | steal order (cache topology) |
| QoS class, root queue | libdispatch | work class |
| label | libdispatch | name |
| poke | libdispatch | doorbell |
| drain (of a lane) | libdispatch | pass |
| narrowing | libdispatch | none (pool size is fixed per class) |
| squeue, vertical perimeter | illumos | per-CPU NET queue with a bound worker |
| squeue_drain_ms | illumos | quantum |
| system worker threads, work item | Windows | BLOCKING class |
| DPC, threaded DPC | Windows | SWI / ithread |
| softint | NetBSD | SWI |
| threadpool (unbound / per-CPU) | NetBSD | worker pool (BLOCKING / NET, BULK) |
| workstream | netisr(9) | per-CPU queue set (kept when citing netisr) |
| grouptask, taskqgroup | gtaskqueue | item on a per-CPU queue (kept when citing gtaskqueue) |

## H. Terms deliberately not used

*pinned* (for workers), *slice* (for a pass), *label*, *expire/expiry*,
*consumer* or *user* (for a client),
*rescuer/rescue*, *concurrency management*, *poke*, *lane*, *QoS*,
*band*, *publish* (for queues), *job*, *task* (for a kwq item), *flush*
(for drain).  When one of these appears in a citation of an external
document it is kept verbatim and the kwq term follows in parentheses.

Last revised 2026-09-24 together with KWQ.md S13 (revision log).
