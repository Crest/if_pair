# libdispatch (GCD) as a precedent for a shared kernel worker pool

Study notes, started 2026-09-19.  Source: the FreeBSD port
devel/libdispatch, work/swift-corelibs-libdispatch-swift-6.1.1-RELEASE
(Apache 2.0), including the port's own patches.  Purpose: mine the
design of the one widely deployed "many components share one
process-wide thread pool with QoS classes and per-queue ordering"
system for lessons applicable to the shared kernel worker pool
sketched in NOTES.md (2026-09-19 design block).  Findings are
file:line referenced against that tree; quotes are from its
comments.

## 1. Structure of the pool (verified)

- **Root queues = QoS class x overcommit bit**, statically
  allocated: 6 QoS levels (maintenance, background, utility,
  default, user-initiated, user-interactive) x {normal,
  overcommit} = 12 root queues (`src/init.c:305-370`,
  `_dispatch_root_queues[]`), plus a dedicated single-threaded
  manager root queue (`:262-273`, `dgq_thread_pool_size = 1`) for
  the event/timer manager.  Root queues have `DQF_WIDTH(POOL)`:
  they are the only queues backed by threads; every user queue
  ultimately targets one of them.
- **The class model is closed.**  Clients choose a QoS class and
  an overcommit attribute; nothing else about scheduling is
  client-controllable.  Priority is encoded in one 32-bit word
  (`src/shims/priority.h:100-122`): 8 bits relative priority
  within the class, 4 bits QoS, 4 bits fallback QoS, 4 bits
  OVERRIDE (the inversion-avoidance boost, see below), 8 flag
  bits (overcommit, fallback, manager, floor, enforce,
  inherited).  Lesson: the whole scheduling contract fits in one
  word and the class enumeration is the API - the same
  "consumers pick a class, never a priority" rule the kernel
  sketch needs.
- **Overcommit** = "this queue may exceed one thread per core".
  Serial queues default to overcommit (`src/queue.c:2709-2710`,
  "Serial queues default to overcommit!"); concurrent queues
  default to non-overcommit.  Rationale: a serial queue that is
  blocked (waiting on I/O, a lock, a sync hop) must not be able
  to deadlock the pool by occupying a core-count-limited thread
  slot, so it is allowed to spill; concurrent work is by
  construction spread over cores and is capped at core count to
  prevent thread explosion.  Kernel analog: the NET class must
  never sleep (so it can be exactly one thread per CPU), and any
  work that CAN block belongs to a class that is allowed to
  overcommit - the "BLOCKING" class in the NOTES sketch, which
  this confirms should exist as a separate pool, not a flag.

## 2. The queue primitive (verified, `src/inline_internal.h:1510-1636`)

- **Intrusive MPSC list with a single atomic exchange on the tail**
  (`os_mpsc_push_update_tail`: store next=NULL, `xchg` tail with
  release; the previous tail is the "token"; if it was NULL the
  queue was empty and the pusher sets head, otherwise it links
  prev->next).  Producers never take a lock and never touch the
  head.  The consumer's `os_mpsc_get_head`/`get_next` handle the
  transient window where tail has advanced but the link is not
  yet written by SPINNING (`_dispatch_wait_for_enqueuer`) - the
  enqueuer is between two stores and cannot be far away.
- **Pop and snapshot are consumer-only** (`os_mpsc_pop_head`,
  `os_mpsc_capture_snapshot`), with a documented subtlety
  (radar 22708742): setting tail to NULL must be a release
  CAS against the popped head so a concurrent enqueuer's head
  write is not clobbered.
- **"Was empty" doubles as the doorbell**:
  `_dispatch_root_queue_push_inline` (`:1729-1735`) pokes the
  pool ONLY when the push transitioned the queue from empty -
  the same "ring on IDLE->WAKING only" rule epair/if_pair use,
  here with zero locks on the producer side.
- Lesson for the kernel pool: per-CPU per-client queues can be
  exactly this - one xchg per enqueue, no mutex, doorbell folded
  into the emptiness transition.  if_pair's `mbufq` + `pq_mtx`
  costs a lock round-trip per packet that this design shows is
  avoidable; the price is the enqueuer-spin window on the
  consumer side (acceptable in a non-preemptible-ish kernel
  context, but the spin must be bounded/epoch-aware).

## 3. Pool sizing: the internal workqueue (verified, `src/event/workqueue.c`)

- On Darwin the kernel's pthread_workqueue sizes the pool; on
  FreeBSD/Linux libdispatch falls back to a **user-level monitor**
  (`DISPATCH_USE_INTERNAL_WORKQUEUE`): one timer per second on
  the manager queue (`:349-357`) walks each QoS bucket and counts
  how many registered worker threads are actually RUNNABLE, by
  polling the OS (`/proc/[tid]/stat` on Linux, `kern.proc.pid |
  KERN_PROC_INC_THREAD` sysctl on FreeBSD - the port's patch).
- Policy (`_dispatch_workq_monitor_pools`, `:293-337`):
  `target_runnable = active_cpus` per bucket; hard cap
  `WORKQ_OVERSUBSCRIBE_FACTOR (2) * active_cpus` runnable
  globally; if a non-empty queue has ZERO runnable workers the
  program is assumed stalled and one thread is poked
  unconditionally; if below target and under the global cap,
  one more thread is poked.  Buckets are walked highest QoS
  first (`foreach_qos_bucket_reverse`) so scarce oversubscription
  budget goes to the most important class.
- Lesson: this is admission by MEASURED RUNNABILITY, not by queue
  depth - the pool grows only when its existing threads are
  blocked, which is the only condition under which more threads
  help.  The kernel analog is cheaper and exact (the scheduler
  knows a thread's state without polling), and it matters only
  for the BLOCKING class; NET/BULK threads never block, so their
  count is fixed at ncpu and this whole subsystem is unnecessary
  for them - a strong argument for keeping blocking work OUT of
  the packet-class pool rather than "handling" it.
- **Port bug found (2026-09-19)**: the FreeBSD
  `_dispatch_workq_count_runnable_workers()` added by
  `files/patch-src_event_workqueue.c` iterates the inner loop as
  `for (int j = 0; i < count; ++i)` - tests and increments the
  OUTER index `i`, never `j`.  It compares only `kp[0]` and walks
  `i` past the registered-tid loop's bound, so the runnable count
  is wrong (typically 0), which makes the monitor treat every
  non-empty root queue as stalled and poke one extra thread per
  second: the pool oversubscribes steadily under load instead of
  self-regulating.  Fix: `for (int j = 0; j < count; ++j)`.
  Also: `struct kinfo_proc kp[255]` (~1 KB each) is a ~280 KB
  stack frame on the manager thread; counting `SIDL` as runnable
  is harmless but meaningless (fork-in-progress state).
  UPDATE (same day, see libdispatch-port-bugs/BUGREPORT.md): this loop is DEAD CODE in
  the port (monitoring is not enabled for FreeBSD there), but two
  live, reproduced crash bugs were found in the port's FreeBSD
  lock shims - the thread ID is not shifted clear of the two lock
  flag bits (unrelated threads alias as one owner -> spurious
  deadlock-detection traps in dispatch_sync/dispatch_once), and
  `_dispatch_ulock_wait` is referenced but never defined (any
  wait-on-address path, e.g. dispatch_group_wait, dies with an
  rtld undefined-symbol error).  Upstream main has neither bug.

## 4. The drain loop: NO time budget, no fairness quantum (verified)

- `_dispatch_lane_drain()` (`src/queue.c:3564-3700`) runs items
  until one of: queue empty; `_dispatch_needs_to_return_to_kernel()`
  (a kernel-workqueue bookkeeping hook, Darwin only); the queue's
  width changed serial<->concurrent (switch drain flavour);
  "narrowing" (below); a workloop received a higher max_qos than
  the one being drained (`:3606-3611` - the ONE QoS preemption
  point in the loop); suspension; retarget.  The root-queue drain
  (`:6087-6135`) is a bare `while (item = drain_one(dq))`.
- The only clock anywhere is the **narrowing check**: every 50 ms
  (`:3462-3476`) one thread per QoS bucket asks the KERNEL
  `_pthread_workqueue_should_narrow(pp)` whether the pool should
  shrink, and if so latches itself into "narrowing" and returns.
  Apple-only (`src/internal.h:700-706`); compiled to `false`
  elsewhere (`:3533-3534`).  It is a pool-SIZE governor, not a
  fairness quantum.
- **Fairness between queues is not promised anywhere.**  The
  public contract (`dispatch/queue.h`, `man/dispatch_queue_create.3`)
  promises per-queue FIFO, non-FIFO completion on global queues,
  and "global queues with lower priority will be scheduled for
  execution after all global queues with higher priority" - strict
  priority between bands, nothing within a band.  No round-robin,
  no anti-starvation term exists in `queue.c`/`queue_internal.h`.
- Why GCD can afford that and a kernel pool cannot: GCD's workers
  are ordinary preemptible threads; the KERNEL SCHEDULER provides
  fairness between them (time slices, priority decay) and between
  the pool and the rest of the process.  A drainer that hogs a
  thread only hogs a thread.  A kernel worker pool at
  interrupt-class priority has no scheduler above it doing that
  job - ULE will not time-slice PI_NET against softclock - so the
  time-quantum/DRR/yield machinery in the NOTES sketch is the
  replacement for a service GCD gets for free.  Lesson: the
  fairness burden sits with whoever owns the highest priority in
  the system; GCD never does, the kernel pool always does.

## 5. Contention and thread-count control (verified)

- Root-queue dequeue uses a MEDIATOR sentinel (`:5892`, "acts both
  as a lock and a signal"); losers of the race enter
  `__DISPATCH_ROOT_QUEUE_CONTENDED_WAIT__` (`:5847-5884`): spin
  a randomized 31-127 iterations ("Use randomness to prevent
  threads from resonating at the same frequency and permanently
  contending", `src/shims/yield.h:101-111`), then exponential
  usleep 500 us -> 100 ms, marking the queue `dgq_pending` so no
  further threads are requested meanwhile; on giving up: "The
  ratio of work to libdispatch overhead must be bad. This
  scenario implies that there are too many threads in the pool.
  Create a new pending thread and then exit this thread."
  Lesson: contention on the WORK QUEUE is read as a thread-count
  signal (too many consumers per producer), not as something to
  optimize the lock for.  Kernel analog: per-CPU queues make the
  consumer side single-threaded by construction, dissolving this
  entire mechanism - the strongest argument for per-CPU rather
  than per-class shared queues.
- Thread requests are COALESCED: `_dispatch_root_queue_poke`
  (`:5792-5811`) does nothing if the queue is empty, and
  non-overcommit queues keep at most ONE outstanding thread
  request (`cmpxchg(dgq_pending, 0, n)`, else "worker thread
  request still pending").  `dgq_thread_pool_size` is a ticket
  count; pokes are clamped to `t_count - floor` (`:5728-5746`),
  so a caller can reserve capacity.  Non-overcommit pools are
  capped at `active_cpus`, overcommit pools at 255
  (`:6162-6172`, `src/shims.h:57-58`).  Idle pthread workers park
  on a semaphore with a 5 s timeout (`:6221`) then exit and
  return their ticket.
- Every successful dequeue that leaves work behind pokes one more
  thread (`:5941-5942`): the pool grows one thread per drained
  item while backlog exists, i.e. demand-proportional, not
  burst-proportional.  Lesson: doorbell coalescing (ring once
  per empty->non-empty) PLUS one-outstanding-request is what keeps
  wakeups O(bursts) instead of O(items) - if_pair's IDLE/WAKING/
  RUNNING machine is the same two ideas fused.

## 6. QoS propagation and the override (priority inversion) machinery (verified)

- Every async submission captures the submitter's QoS
  (`_dispatch_priority_propagate`, `src/inline_internal.h:2266-2295`)
  with a deliberate CAP: "Cap QOS for propagation at
  user-initiated" - a thread at the top band cannot mint
  top-band work by side effect; only explicit attributes reach
  user-interactive.  Lesson for the kernel: interrupt context
  enqueueing to the pool must not inherit interrupt priority into
  the pool's class choice - the class comes from the CLIENT's
  registration, never from the enqueueing context.
- The inversion fix is the **override**: each queue tracks
  `max_qos` = the highest QoS ever enqueued on it (state bits
  34-32, `queue_internal.h:329-334`).  When a higher-QoS item
  lands behind lower-QoS work: if the queue is currently
  drain-locked, boost the OWNER THREAD directly
  (`_dispatch_wqthread_override_start_check_owner`, `queue.c:
  4711-4715`); else walk the target-queue chain under the
  side-lock and enqueue a STEALER on the higher-QoS root queue
  (`:4770-4776`) that will drain the low queue at high priority;
  a drainer that locks a queue boosted above its own floor boosts
  itself (`inline_internal.h:1188-1246`).  Override is cleared
  only when the final drain-unlock succeeds.  Root-queue level:
  an item exceeding its root's band is re-enqueued on the higher
  root, wrapped to keep the origin queue's identity
  (`OVERRIDE_OWNING`/`OVERRIDE_STEALING`, `:4611-4692`).
- Lesson: per-flow ordering (serial queue) and priority
  inheritance COMPOSE only with owner-boosting.  A kernel pool
  with per-CPU per-client FIFO queues has the same inversion
  shape the moment two classes share a queue - which is exactly
  why the NOTES sketch keeps classes in separate threads: with
  one class per thread and DRR within, there is nothing to
  invert.  GCD's complexity here (~500 lines, four documented
  race cases at `:4733-4766`) is the cost of letting arbitrary
  queues target arbitrary queues; a flat class x CPU pool
  deliberately forgoes that generality.

## 7. dispatch_apply: batch parallelism sizing (verified, `src/apply.c`)

- Parallelism = `_dispatch_qos_max_parallelism(qos, ACTIVE)`
  (active CPUs for that band), divided by nesting depth
  (`:313-320`, "Handle nested dispatch_apply"), clamped to the
  iteration count; degenerates to serial for width-1 queues.
  Never uses an overcommit root queue (`:265-283`, `false`).
  Work items self-schedule by atomic index (`:36-70`) so one
  worker absorbs many iterations - the parallel-for shape, not
  N enqueues.
- Lesson: the man page (`dispatch_apply.3:88-107`) says why a
  pool wants this as a primitive: N individual enqueues "do not
  express the desired parallel execution semantics to the
  system, so may not create an optimal number of worker
  threads".  Kernel analog: a BULK-class "scatter N items across
  CPUs and wait" entry point (wg-style crypto fan-out) is worth
  having as an API rather than N enqueues, and it must divide its
  budget when nested.

## 8. Synthesis: what transfers to a shared kernel worker pool

TRANSFERS DIRECTLY
- Closed class model in one priority word; clients choose a
  class, never a priority (S1).
- Lock-free MPSC per queue with the empty->non-empty transition
  as the doorbell (S2) - replaces if_pair's mutex-per-packet.
- Overcommit as a CLASS property: blocking work lives in a pool
  allowed to exceed ncpu; non-blocking work is capped at ncpu
  and never mixed with it (S1, S3).  GCD's user-level runnable
  monitor is what a kernel pool gets from the scheduler for
  free, and only the blocking class needs it.
- Thread-request coalescing: one outstanding wake per queue;
  grow one worker per drained-item-with-backlog (S5).
- Propagation cap: enqueue context never promotes the work's
  class (S6).
- A batch fan-out primitive for BULK, nesting-aware (S7).
- Randomized spin before sleep on contended shared resources
  (S5) - relevant to any remaining cross-CPU lock.

DOES NOT TRANSFER / MUST BE ADDED
- GCD has NO time budget and NO inter-queue fairness because the
  kernel scheduler arbitrates its threads.  A kernel pool at
  interrupt priority IS the arbiter: the DRR time quanta and the
  demote-yield in the NOTES sketch have no GCD counterpart and
  cannot be omitted (S4).
- Override/stealer machinery is the price of arbitrary
  queue->queue targeting.  Flat class x CPU avoids needing it;
  do not import the generality (S6).
- User-level pool-size polling (S3) is a workaround for lacking
  scheduler introspection; in-kernel, ask the scheduler.

WHAT GCD GOT WRONG THAT WE SHOULD NOT REPEAT
- "Serial queues default to overcommit" is a liveness patch for
  blocking inside serial work; documented as a footgun
  ("thread explosion", `dispatch/queue.h:93-98`).  The kernel
  pool should REJECT blocking in NET/BULK (assert non-sleepable
  via WITNESS) rather than tolerate it with spare threads.
- The runnable-count monitor's correctness depends on
  platform-specific polling code that nobody exercises -
  witness the FreeBSD port bug (S3).  Any equivalent kernel
  logic needs a test that actually saturates and blocks workers.

## 9. Lock-word encoding: flag bits high or low, and why 32 bits

- The lock word is 32 bits because it is the LOW HALF of the 64-bit
  `dq_state` (bits 31-0 "drain lock", `queue_internal.h:335-349`;
  suspend count, width, dirty/enqueued, role, max_qos occupy 63-32)
  and the whole state is updated with one 64-bit CAS.  A 64-bit
  owner would need 128-bit atomics or a split state - a redesign.
  The kernel wait primitives it leans on are 32-bit too (futex is
  32-bit only; `_umtx_op` WAIT_UINT; Darwin ulock protocols), and
  every platform's thread identity fits 32 bits (mach_port_t, pid_t,
  DWORD, lwpid_t).  Two flag bits leave a 30-bit owner on ANY layout.
- Low flag bits (Darwin, Windows) suit handle-like identities whose
  low bits are CONSTANT by construction (Mach port names carry the
  rollover mask 0x3 there; Darwin's _dispatch_lock_owner ORs both
  bits back in): no shift, no range loss.  For integer identities they cost a shift on encode
  and `>> 2` on every decode, and truncate at 2^30 silently.
- High flag bits (Linux futex, FreeBSD umutex) suit integer
  identities: owner is the raw tid, no decode anywhere, the range
  check is one mask test, the kernel's own mutex word uses the same
  shape (`UMUTEX_CONTESTED` bit 31, tids in `[PID_MAX+2, INT32_MAX]`
  per `sys/umtx.h`) so kernel-assisted PI/robust locking stays
  reachable, and low-range marker values stay available.  For FreeBSD
  this is the OS-sanctioned layout; the port/upstream chose the
  Windows shape for consistency, which is defensible but forecloses
  umtx-native locking.  Lesson for any kernel-side owner encoding:
  match the identity's shape - flags where the identity is
  guaranteed to carry a known constant - rather than porting another
  platform's mask.

## 10. What the public API teaches (2026-09-23)

The public surface (`dispatch/*.h`) is small: queues with attributes,
`async`/`sync`/`barrier` submission, `apply`, groups, semaphores, sources,
`once`, blocks.  Sorted by whether the concept survives the move into a
kernel worker pool serving if_pair/epair/wg/GELI-class clients:

KEEP (the shape transfers)
- **The queue is the unit of everything.**  Class (QoS), ordering
  (serial vs concurrent), label and context are properties of the queue
  set at `dispatch_queue_create()`; work items carry none of it.  Kernel
  analog: a client registers a queue with a CLASS and gets per-CPU FIFO
  ordering; items are bare intrusive links.  Nothing is decided per
  item, so the enqueue path has nothing to look up.
- **Async submission as the only submission.**  `dispatch_async` is the
  whole data plane; everything else is bookkeeping around it.
- **Fan-out with completion by callback, never by wait.**
  `dispatch_group_notify()` (not `dispatch_group_wait()`) is the pattern a
  non-sleeping kernel client can use: scatter N items, get one callback
  when the last finishes.  wg's per-peer serial "send/recv" grouptasks
  after parallel crypto are exactly this.
- **`dispatch_apply` as a primitive** (S7): a batch fan-out entry point
  that sizes itself to the class's CPU budget and divides when nested,
  instead of N enqueues that "do not express the desired parallel
  execution semantics to the system".
- **Initially-inactive creation** (`DISPATCH_QUEUE_SERIAL_INACTIVE`,
  `dispatch_activate`): configure fully, then publish.  This is the
  cloner create-return-window lesson from the if_pair locking audit
  restated as API: a queue must not be enqueue-able before its handler,
  class and per-CPU storage exist.
- **Labels and per-queue context** (`dispatch_queue_set_specific`): the
  observability surface (sysctl tree keyed by label, SDT probes carrying
  it) costs nothing and is what made the callout-starvation diagnosis
  possible only after DTrace archaeology.

DROP (the kernel must not import these)
- **`dispatch_sync` and `barrier_sync`**: synchronous execution of work
  on another queue from the caller's context.  This is the inpcb
  recursion of the 2026-08-14 panic as an API; a kernel pool offers no
  synchronous cross-context execution at all.  Barriers lose their
  purpose too: with one consumer per CPU-queue, every item is a barrier.
- **Target queues** (`dispatch_set_target_queue`): the queue graph is
  what necessitates the override/stealer machinery (S6).  Flat class x
  CPU, no hierarchy.
- **Semaphores, `dispatch_group_wait`, `dispatch_once` blocking**: all
  sleep.  NET/BULK classes never sleep; the BLOCKING class uses the
  kernel's own sleep primitives.
- **Sources** (`DISPATCH_SOURCE_TYPE_READ/TIMER/SIGNAL/...`): event
  demultiplexing into a queue.  The kernel already has the producers
  (ithreads, callouts, netisr, kevent); they enqueue directly.  No
  separate abstraction, and no manager thread.
- **Blocks with cancellation, `dispatch_after`, main queue, autorelease
  frequency**: language-runtime and process-lifetime concerns.

ADD (the kernel needs what GCD never did)
- **A CPU argument on enqueue**: `enqueue(queue, cpu, item)`.  Steering
  (flow hash, rxq, "current CPU") is the client's knowledge; GCD hides
  CPU placement because the scheduler owns it, the kernel pool IS the
  placement.
- **Enqueue that can fail**: `dispatch_async` never fails because it
  can allocate; a kernel enqueue returns ENOBUFS at a per-queue bound
  and the client decides (drop + counter, or backpressure).
- **A batch handler signature**: `fn(queue, head, n)` hands the consumer
  the whole drained list, so clients keep the `mbufq_flush()`
  amortization instead of paying a callback per item.
- **Time-quantum drain with the demote-yield** (S4): the fairness and
  latency mechanism GCD leaves to the kernel scheduler.
- **Weights within a class, bounded, downward only** ("I am
  background"), never a priority escalator.

The sketch that falls out (kernel C, names illustrative):

    enum wp_class { WP_NET, WP_BULK, WP_BLOCKING };
    #define WP_F_STEALABLE   0x1   /* BULK only: idle CPUs may take work */
    #define WP_F_INACTIVE    0x2   /* create inactive; wp_activate() publishes */

    typedef void wp_handler_t(struct wp_queue *, struct wp_item *head, int n);

    struct wp_queue *wp_create(const char *label, enum wp_class, int flags,
                               int weight /* 1..8, default 1 */,
                               wp_handler_t *, void *ctx);
    void   wp_activate(struct wp_queue *);
    int    wp_enqueue(struct wp_queue *, int cpu, struct wp_item *);  /* 0 | ENOBUFS */
    int    wp_enqueue_list(struct wp_queue *, int cpu, struct wp_item *h,
                           struct wp_item *t, int n);
    int    wp_scatter(struct wp_queue *, struct wp_item *list, int n,
                      void (*done)(void *), void *);  /* BULK; apply+group_notify */
    void   wp_drain(struct wp_queue *);      /* unload contract, all CPUs */
    void   wp_destroy(struct wp_queue *);
    /* per-label per-CPU counters under a sysctl tree; SDT wp:::enqueue,
       wp:::drain-begin/end, wp:::quantum-expired, wp:::enqueue-full */

Everything on the KEEP list has a GCD precedent that has carried a very
large installed base; everything on the ADD list is either a consequence
of "no scheduler above the pool" (quantum, yield, weights) or of "no
allocation on the hot path" (ENOBUFS, intrusive items, batch handler).
The DROP list is where GCD's generality would import exactly the hazards
this project already hit.
