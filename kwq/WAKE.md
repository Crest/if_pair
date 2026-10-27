# Wake moderation for kwq: interrupt coalescing's analogue

Design study, 2026-10-24.  PLAN P4e.  The question (crest): should kwq
grow a mechanism similar to interrupt coalescing (interrupt moderation)
to reduce wakeups and context-switch frequency, and how would that
change the API and the implementation?  This document records the
literature and the FreeBSD prior art, transfers the concepts to kwq's
producer/worker model, and proposes the designs worth building for an
A/B comparison with tests/compare.sh.  Section 5 is the proposal; the
rest is the evidence for it.  Section 7, added the same day, has the
measurements of W2 and W4 on the three machines and the latency budget
per case.

Terminology follows GLOSSARY.md: a *pass* is one handler invocation
over a swapped-out item list; a *wake* is the IDLE->WAKING transition
and the wakeup() behind it; a *switch* is a scheduler context switch.
"Moderation" means deferring or suppressing wakes so that more items
accumulate per pass, at a bounded cost in first-item latency.

## 0. One-paragraph summary

Interrupt coalescing is a producer-side mechanism: the NIC (the
producer) holds the interrupt (the wake) until either N packets or T
microseconds have accumulated, with an absolute timer bounding the
delay a steady trickle can cause.  Every software descendant of it is
one of four things: (a) the producer defers the wake behind a count and
a deadline (NIC ITR, NVMe aggregation, virtio event_idx); (b) the
consumer stays awake a while after finding nothing, so that the next
item needs no wake (NAPI with napi_defer_hard_irqs, Linux IRQ
suspension, io_uring SQPOLL, ULE's own idle spin); (c) a timer drives
the consumer and producers never wake it (tcp_hpts above its connection
threshold, polling(4), soft timers); (d) the producer becomes the
consumer and nobody is woken (netisr hybrid dispatch, iflib's mp_ring,
illumos squeues).  kwq has the lost-wakeup protocol all of them need
(the IDLE/WAKING state in the shared cache line) and already coalesces
every wake after the first of a backlog, so what remains to moderate is
the first wake of each burst: 31 k of them per second in the a07
single flow's fast regime, one or two items per pass with one or two
producers in the fan-in scenario.  Family (b) was built, measured and
declined on 2026-10-09 to 11 (kwq-spin-experiment): zero wakeups, same
throughput, the CPU went to the poll instead of to idle.  Family (d)
is barred for the NET class by the lock recursion that gave if_pair
its queues.  That leaves (a) and (c), which share one implementation
(a deferred doorbell with a count threshold and a deadline callout per
worker, section 4.2), plus one mechanism no NIC has because no NIC
shares a CPU with its producer: waking the worker at a priority that
does not preempt the producer, promoted at the deadline (section 4.4),
which is the share limit's hand-over applied to the wake.  Section 5
proposes building the deferred doorbell (static and adaptive
threshold) and the non-preempting wake beside the baseline, all off by
default, and measuring them with compare.sh and the fan-in scenario on
the three machines; the exit criterion is a wake-rate and CPU saving at
a latency cost equal to the configured deadline and no throughput loss,
or a recorded decline like the idle poll's.

## 1. The costs that moderation trades against latency

A wake that reaches an idle worker costs, in order of appearance:

1. the producer's wakeup(): sleepqueue chain lock, the thread lock,
   sched_add(), and for a remote CPU tdq_notify(): an IPI unless the
   target is in its idle loop's spin phase (sched_ule.c, below);
2. the switch into the worker on its CPU, and on a same-CPU producer
   the preemption of that producer (the wake arrives at the class
   priority, which is below preempt_thresh, so the producer loses the
   CPU at its next instruction);
3. the pass's fixed cost: list swap under the queue lock, clock reads,
   epoch entry, scheduler bookkeeping, the DRR accounting;
4. the switch out, and the cache state the worker displaced on a shared
   CPU;
5. for the producer, the burst it was producing is cut at its first
   item: everything after it is queued behind a pass that is already
   running and arrives at the next pass, so one burst becomes several
   passes instead of one.

Cost 5 is the one coalescing was invented for, and the one that the
items-per-pass counter shows directly; costs 1-4 are per wake and are
what tools/schedprobe and the switch_baseline scenario measure.  The
benefit side is first-item latency: every design below bounds it with
an explicit deadline, and the deadline is the knob a client reasons
about, exactly as rx_abs_int_delay is for em(4).

## 2. Interrupt coalescing in hardware and in FreeBSD's drivers

### 2.1 The three-knob model

Intel's e1000 family exposes the canonical set, and em(4) carries it
as loader tunables (sys/dev/e1000/if_em.c:649-662): `rx_int_delay`
(RDTR) is a timer that restarts with every received packet, so a burst
raises one interrupt when it ends; `rx_abs_int_delay` (RADV) is an
absolute timer started by the first packet, so a steady trickle cannot
postpone the interrupt forever; the TX side has the same pair (TIDV,
TADV).  Chelsio's SGE has the same two dimensions as index tables
(sys/dev/cxgbe/t4_sge.c:745-747): holdoff timers {1, 5, 10, 50, 100,
200} us and packet counts {1, 8, 16, 32}, selected per queue with
`hw.cxgbe.holdoff_timer_idx` and `holdoff_pktc_idx`.  Mellanox's mlx5
exposes `rx_coalesce_usecs`, `rx_coalesce_pkts`, their TX twins, and
`rx_coalesce_mode` (mlx5_en_ethtool.c:918-979, 1079), where the mode
chooses whether the timer starts from the first completion (CQE based,
the "restarting" semantics) or from the event (EQE based).  NVMe's
interrupt coalescing feature is the same pair, Aggregation Threshold
(completions) and Aggregation Time (100 us units), whichever comes
first.

The lesson for kwq is the shape of the parameter space: a count
threshold, a restarting timer and an absolute deadline.  Every design
in section 4 that defers a wake has these three, and the absolute
deadline is the one that must exist; the other two are tuning.

### 2.2 Adaptive moderation

Static knobs are wrong for one of the two regimes, so the drivers
adapt them from what the last interval carried:

- em(4)'s AIM (`hw.em.enable_aim`, default on; if_em.c:1652-1760) is a
  latency state machine with three targets, "lowest" at 70k, "low" at
  20k and "bulk" at 4k interrupts per second.  Each interrupt it looks
  at the bytes, packets and bytes per packet of the busiest ring and
  moves one state: many small packets go to "lowest" (the trickle must
  not wait), large packets or high byte counts go to "bulk", and the
  idle case leaves the setting alone.
- ix(4)'s AIM (`hw.ix.enable_aim`, default off; if_ix.c:2597-2660)
  derives the EITR directly from bytes per packet of the last
  interval, clamps it to 3000, and favours the mid range.  The static
  alternative is `hw.ix.max_interrupt_rate`, default 31250
  interrupts per second (32 us).
- mlx5 carries Linux's Net DIM (mlx5_en_dim.c): a sample is the
  bandwidth, the packet rate and the event rate of the last period; a
  step is "better" if bandwidth rose, on a tie if the packet rate rose,
  on a second tie if the event rate fell; the algorithm walks a profile
  table of (usec, pkts) pairs in the direction that was better, parks
  when a step makes things worse, and lengthens its own decision
  interval when it keeps deciding to stay.  A difference counts only
  above a percentage threshold.

The lesson: an adaptive controller needs a quantity to maximise, and
the NIC drivers chose throughput per interrupt with latency as a
constraint from the packet size.  kwq has a better signal than a NIC:
it sees the queue depth at the moment of the wake and the items per
pass, so it can adapt the deferral to the burst size it actually
observed rather than infer it from bytes per packet.

### 2.3 Software coalescing in the consumer: NAPI and its descendants

Linux's NAPI (Salim, Olsson and Kuznetsov, 2001, following Mogul and
Ramakrishnan's receive-livelock work of 1997) made the consumer the
moderator.  The interrupt handler disables the device's interrupt and
schedules a poll; the poll processes up to `budget` packets; only a
poll that finds less than the budget re-enables the interrupt
("IRQ should only be unmasked after a successful call to
napi_complete_done()").  Two later additions are the direct analogue of
what kwq would do:

- `napi_defer_hard_irqs` and `gro_flush_timeout` (per device, and per NAPI
  instance since the late-2024 netdev-genl work): after an empty poll the interrupt
  stays masked for up to that many further empty polls, each re-armed
  by a timer of `gro_flush_timeout` nanoseconds, so a busy queue runs
  on timer-driven polls and never takes a hardware interrupt, while an
  idle one falls back to interrupts after the count expires.
- IRQ suspension (Cai and Karsten, "Kernel vs. User-Level Networking:
  Don't Throw Out the Stack with the Interrupts", SIGMETRICS 2024;
  Damato's implementation in Linux 6.13 as `irq-suspend-timeout`): the
  application's own epoll_wait is the consumer; while it keeps finding
  events, interrupts stay suspended and the timer is deferred; the
  first empty epoll_wait re-enables the ordinary mitigation; the
  timeout is only a safety against a stalled application.  Their
  measurement is the relevant one: the user-level stacks' advantage
  was not the stack but the temporal alignment of producer and
  consumer that polling gives, and a 30-line kernel change recovered
  most of it (up to 45 % more throughput at the same tail latency).
- busy polling (`SO_BUSY_POLL`, `SO_PREFER_BUSY_POLL`, epoll's
  `EPIOCSPARAMS`): the consumer spins on the queue for a bounded time
  instead of sleeping, with "prefer busy poll" pledging to come back
  before `gro_flush_timeout` so that the interrupt can stay masked.
- threaded NAPI runs each instance in a kernel thread pinned to the
  interrupt's CPU, which is kwq's worker model.

io_uring's SQPOLL thread is the same consumer-side idea for a
submission queue: it polls for `sq_thread_idle` milliseconds after the
last submission, then sleeps and raises `IORING_SQ_NEED_WAKEUP` in the
shared ring so that the producer makes the (expensive) system call only
when the consumer is asleep.  AF_XDP's `need_wakeup` flag and
ptnetmap's CSB flags (Garzarella, Rizzo and Lettieri, ANCS 2015, where
the host kernel threads spin a configurable while before sleeping)
are the same protocol.  kwq's IDLE/WAKING word in the shared cache
line is this flag; what these systems add is the *linger* before
setting it.

### 2.4 Producer-side suppression: virtio's event index

virtio's `VIRTIO_RING_F_EVENT_IDX` turns the notification into a
threshold set by the consumer: the consumer writes the ring index at
which it wants to be kicked next, and the producer kicks only when it
crosses it.  vtnet(4) uses it on receive (if_vtnet.c:741, 4480-4498)
through `virtqueue_postpone_intr()` with hints `VQ_POSTPONE_SHORT`,
`VQ_POSTPONE_LONG` and `VQ_POSTPONE_EMPTIED`: the consumer, having
just drained a burst, asks to be woken only after a quarter, three
quarters or all of the available descriptors have been used.  The
consumer thus sets the count threshold from the burst size it saw,
with no timer, because the virtio consumer is a thread that will come
back on its own.  The Xen ring macros and netmap's sync model have the
same "re-check after enabling notifications" step that kwq's idle
transition already performs under the lock.

### 2.5 No wake at all: the producer becomes the consumer

- netisr's hybrid dispatch (sys/net/netisr.c:1156-1230,
  `net.isr.dispatch=hybrid`): if the packet's CPU is the current one
  and the workstream is neither running, nor being dispatched by
  another thread, nor already signalled, the producer runs the
  protocol handler itself under `NWS_DISPATCHING`; anything that
  arrived meanwhile is handed to the worker with one signal at the
  end.  The comment anticipates the next step, draining the backlog
  in the borrowed context under a bound.
- iflib's transmit path uses Chelsio's mp_ring (sys/net/mp_ring.c): a
  multi-producer ring whose state word carries IDLE, BUSY, STALLED
  and ABDICATED; the producer that moves it from IDLE becomes the
  consumer and drains up to a budget, later producers only append,
  and `tx_abdicate` (iflib.c:4239-4249) switches to handing the drain
  to the GROUPTASK instead.  On receive, iflib's `_task_fn_rx`
  (iflib.c:4007-4052) is NAPI: `rx_budget` (default 16) frames per
  call, re-enqueue the task if more remain, re-enable the queue's
  interrupt only on a short poll, and a one-tick watchdog callout on
  an empty one.
- illumos squeues (Tripathi et al., Crossbow, 2009) are the same
  election for the TCP/IP input path with a worker thread as the
  fallback, woken by a timer (`squeue_workerwait_ms`) rather than by
  the producer when the entering thread chooses not to drain.
- taskqueue(9) and gtaskqueue coalesce at the task, not the wake: a
  task that is already pending is counted (`ta_pending++`,
  subr_taskqueue.c) and not re-enqueued, and `TQ_FLAGS_BLOCKED`
  suppresses the wake entirely while set.  `swi_sched(cookie,
  SWI_DELAY)` (kern_intr.c) sets the handler's need bit without
  scheduling the thread: the caller promises that something else will
  schedule it, the oldest "deferred wake" in the tree.

Direct dispatch is the strongest moderation (zero wakes, zero
switches) and the one kwq's NET class cannot use for if_pair: the
producer is the sender's stack, inside its socket and inpcb locks, and
the handler is the receiver's stack; that recursion is why if_pair has
queues at all.  It stays available for BULK clients and for producers
that are already in the worker's context (a handler enqueueing to
another queue on the same CPU).

### 2.6 Timer-driven consumers

- tcp_hpts (sys/netinet/tcp_hpts.c) is the in-tree example closest to
  what a moderated kwq worker would be: one thread per CPU, driven by
  a callout with `C_PREL(precision)`, whose sleep adapts between
  `dyn_minsleep` (250 us) and `dyn_maxsleep` (5 ms): halved when a
  run processed more than `less_sleep` slots, doubled when it
  processed fewer than `more_sleep`.  Above `cnt_thresh` connections
  (default 100) and with `nowake_over_thresh` set, producers do not
  wake it at all (`tcp_wakehpts()`, line 518): "in this mode the timer
  is a backstop to all the userret/lro_flushes".  Below the threshold
  an insert earlier than the current sleep wakes it with
  `swi_sched()`.  That is the two-regime design in production: wakes
  while sparse, pure timer-driven batching while dense.
- polling(4) (sys/kern/kern_poll.c) is the pre-NAPI design: hardclock
  drives the handlers, `kern.polling.user_frac` (default 50) is the
  CPU share left to user threads, and the per-tick burst adapts to
  keep it.  It is the ancestor of the share limit, not of the wake
  path, but its header comment states the trade the same way.
- Aron and Druschel's soft timers (SOSP 1999) are the cheap timer
  kwq lacks: check the pending work at points the kernel passes
  anyway (system call return, interrupt return, idle), so that a
  "poll every 50 us" costs nothing while the CPU is busy and only a
  real timer while it is idle.  FreeBSD has no general hook for this
  outside the scheduler (`userret()` is the nearest, and tcp_hpts
  uses it); kwq must not reach into scheduler internals (the standing
  rule), so the equivalent is a callout at coarse precision, which
  the event timer subsystem already aggregates with its neighbours
  (`callout_when()`, kern_timeout.c:880-925, widens the precision to
  `kern.timecounter.alloweddeviation`, 5 % by default, unless
  `C_PREL` says otherwise).
- blk-mq's hybrid polling (Linux 4.10) sleeps for half the mean
  completion time of recent requests before polling: predict the
  arrival, sleep until just before it.  Applicable where arrivals are
  periodic (a paced sender), not to if_pair's bursts.

### 2.7 What the scheduler already moderates

- The idle thread spins before halting (sched_ule.c:3138-3168): when
  the CPU switched more than `kern.sched.idlespinthresh` times in the
  last tick it checks its run queue `kern.sched.idlespins` (10000)
  times with `cpu_spinwait()` before entering `cpu_idle()`, except on
  SMT siblings.  `tdq_notify()` (line 1185) skips the IPI when the
  target is the idle thread outside `cpu_idle()`.  So a remote wake to
  a recently busy, now idle CPU costs no IPI and no C-state exit; the
  IPI returns once the CPU has been idle for a tick.  This is the
  consumer-side linger implemented one level down, for free, and any
  kwq linger must be measured against it: on an otherwise idle CPU it
  may buy nothing.
- Preemption on wake is a priority rule (sched_shouldpreempt(), line
  455): a waking thread preempts the running one if its priority is
  numerically at or below `kern.sched.preempt_thresh`, which is
  `PRI_MIN_KERN` (40) in GENERIC, always if the running thread is
  idle-class, and remotely if the waker is interactive and the running
  thread is not.  In 15.1 `PI_NET` is `PI_INTR` (1), `PRI_MIN_KERN` is
  40, `PRI_MIN_TIMESHARE` 56, `PRI_MIN_IDLE` 224
  (sys/sys/priority.h).  A wake at the class priority therefore
  preempts a same-CPU producer at once; a wake at a timeshare priority
  waits for the producer to block, yield or exhaust its slice.
- ULE's `sched_wakeup()` resets the slice (ule-slice-reset, 2026-10-20)
  and classifies by sleep time, so a worker that sleeps between bursts
  stays "interactive" and is always eligible to preempt; moderation
  that keeps the worker awake longer changes that classification as a
  side effect, which the P4c ule branch showed matters.

Linux's equivalents (wake-affine placement, `WF_SYNC` "the waker is
about to sleep" hint, Go's rule of at most one spinning thread looking
for work before another is woken) exist to limit wake storms among
many consumers; kwq has one worker per (class, CPU), so that problem
is already solved by construction, and only the single worker's wake
frequency remains.

## 3. The literature, one line each

- Mogul and Ramakrishnan, "Eliminating receive livelock in an
  interrupt-driven kernel", USENIX ATC 1996 / ACM TOCS 1997: interrupt
  when idle, poll when busy, bound the poll by a quota, and give the
  consumer's consumers (userland) a guaranteed share.  Every later
  design is a refinement of these four rules.
- Druschel and Banga, "Lazy Receiver Processing", OSDI 1996: do the
  protocol work in the context that will consume the data and charge
  it there; the ancestor of direct dispatch and of kwq's per-queue
  accounting.
- Aron and Druschel, "Soft timers", SOSP 1999: microsecond-granular
  polling without timer interrupts, by checking at trap returns;
  showed network polling at 50-100 us intervals with no measurable
  cost on a busy CPU.
- Salim, Olsson, Kuznetsov, "Beyond Softnet", USENIX ALS 2001: NAPI;
  the consumer owns the interrupt mask, polls with a budget, and
  re-enables only when it finds the queue short.
- Dovrolis, Thayer, Ramanathan, "HIP: hybrid interrupt-polling for the
  network interface", ACM OSR 2001: the switch between the two regimes
  should be driven by the observed load, with hysteresis.
- Rizzo, "netmap", USENIX ATC 2012, and polling(4) (2002): amortise
  per-packet costs by letting the consumer decide when to synchronise;
  batching at the boundary is where the gain is, not in the per-packet
  path.
- Belay et al., "IX", OSDI 2014: adaptive batching, the batch grows
  with the backlog and shrinks when the queue is short; run to
  completion; bounded batch size keeps tail latency.
- Marty et al., "Snap", SOSP 2019: three engine scheduling modes,
  dedicated cores (spin), spreading (interrupt-driven wake per engine
  with an idle timer), compacting (few cores, timer-driven); the right
  mode depends on load, and they switch at run time.
- Ousterhout et al., "Shenango", NSDI 2019, and Fried et al.,
  "Caladan", OSDI 2020: a central thread observes queue depths every
  5 us and reallocates cores; wakes are made by the observer, not the
  producer.  The lesson that transfers is the signal: queue depth
  growing between two observations, not the arrival of one item, is
  what should trigger a wake.
- Thompson et al., "LMAX Disruptor", 2011: the taxonomy of consumer
  wait strategies, busy spin, yield, sleep, block, and "phased
  backoff" which walks through them with time; the engineering
  consensus that a consumer should spin briefly before it parks.
- Cai and Karsten, SIGMETRICS 2024: the quantified case that the
  interrupt (the wake), not the kernel stack, is the cost, and that
  suspending it while the consumer is demonstrably keeping up removes
  most of the difference to user-level stacks.
- Gilboa et al., Net DIM (Linux 4.16, 2018): the production adaptive
  controller, a hill climb on (usec, pkts) profiles judged by
  throughput, then packet rate, then event rate.

## 4. Transfer to kwq

### 4.1 What kwq already does, and what the tree already measured

The producer-side decision is edge-triggered on IDLE (kwq.c:413-430,
`kwq_kc_added()`): the first enqueue into an empty, unplaced list sets
WAKING and rings the doorbell; every enqueue after that, until the
worker swaps the list out, is a lock-append with no wake.  The worker
sleeps in `msleep_spin()` on its own channel with nothing between the
last pass and the sleep, and KWQ.md S16 states the design position:
"the doorbell makes the buffer unnecessary ... under load the batch a
pass receives is exactly what arrived during the previous pass ...
with no added latency when the system is idle", and rejects
producer-side staging for its flush timer, its tunable, its staging
area and its second state machine.  So kwq coalesces every wake but
the first of a backlog.  The question is therefore narrower than the
NIC's: what to do about the first wake of each burst, in the regimes
where bursts are short.  The tree has the numbers for those regimes:

- A sleep-wake pair costs 2-8 us of CPU across both sides (NOTES.md
  2026-10-10, t_32): `kwq_enqueue()` takes 650 ns when the list is
  live, 1.1-1.25 us when the worker is awake, 2.1-6.5 us when it is
  asleep (the IPI and its placement); on the guest the asleep case is
  16.7 us because the IPI is a VM exit.  The kernel's part of the wake
  path, sleepqueue, setrunnable, IPI and switch, is 4 us of the 6.7 us
  from enqueue to handler (t_33) and identical for a taskqueue.
- The a07 single flow is bimodal: 35-37 Gbit/s with ~31 k wakeups/s
  and 99 % of the worker's sleep gaps under 20 us, or 29-32 Gbit/s
  with ~10 k wakeups/s and 32-64 us gaps (NOTES.md 2026-10-09); the
  `cost` scenario on a07 is bistable between 3 items per pass at
  136-147 ns per item and 20-24 items per pass at 20-24 ns per item
  (2026-10-20).  lab's single flow runs at 5-9 items per pass, 109
  with a spinner on the worker CPU.
- The fan-in table (SCHED.md S15.7) is the coalescing regime in one
  row: with one or two single-item producers the worker gets 2 and 1
  items per pass and 1.9-2.0 M items/s; with four producers, 133 items
  per pass and 5.25 M items/s.  The passes of one item cost the
  throughput, not the lock: the worker's two acquisitions per pass and
  the producers' contention only appear at 16 producers.
- The guest shows the same-CPU pathology (NOTES.md 2026-10-09): the
  worker at PI_NET wakes, preempts the iperf3 receiver inside its
  socket-lock section, blocks on that lock ~9.5 k times/s, lends its
  priority and switches back, 18-19 % of its time and 50 k context
  switches/s; a second thread on the CPU (spinner or timer) drops the
  wakeups to 1.8 k/s, raises items per pass from 3 to 5 and throughput
  from 8 to 8.6 Gbit/s, because the worker then runs when the receiver
  blocks rather than at its next instruction.  On a07 and lab the
  receiver sits on another CPU and the P4d `coloc` test found no
  collapse (4.06 items per pass, rates equal across stages).
- The idle poll, PLAN P4b goal 1 idea 1 (NOTES.md 2026-10-09 to 11,
  branch kwq-spin-experiment 007f0f4): knob `idle_spin_us`, spin or
  WFE/MWAIT after an empty check, abandoned at once when
  `sched_runnable()`, attempted only after a gap shorter than the
  bound.  At 50-100 us the a07 worker's wakeups fell from 21-35 k/s to
  0-3 k/s with 88-100 % hits, the worker CPU's idle share from 20-32 %
  to 0-6 %, the flood RTT from 8 to 6-7 us; throughput did not move
  (34.8 vs 34.6/33.9 Gbit/s, inside the run-to-run spread), the CPU
  went to the inpcb SMR lock instead, and the poll's hit rate was 0 %
  when the receiver shared the CPU because rule 1 ended every poll.
  Declined: "throughput unchanged, 1 us of latency per hop, a CPU
  saving only at doorbell rates where 2-8 us per wakeup matters,
  against ~260 lines, three knobs and a per-CPU timer register written
  by a module."
- tools/schedprobe (2026-10-21) priced the primitives a deferral would
  use: `callout_reset_sbt_on()` plus `callout_stop()` 49 ns on lab and
  80 ns on a07, a priority change and back 30/80 ns, `sched_runnable()`
  under the 40 ns floor, a deadline callout accurate to 0.5 us with
  the worker back on the CPU 260-400 ns after it fires.  A deferred
  wake is therefore two orders of magnitude cheaper to arm than the
  wake it replaces.

Two conclusions follow before any design.  First, the single if_pair
flow is not wake-bound on a07: with zero wakeups it moved the same
bytes.  A moderation mechanism will be judged on CPU per packet, on
wake and switch rates and on the fan-in and same-CPU regimes, not on
single-flow throughput.  Second, the idle poll failed on the cost side,
not the mechanism side: it converted idle time into spin time.  A
deferral converts a wake into nothing and leaves the CPU idle or with
its other threads, which is the saving the poll could not show.

### 4.2 Taxonomy for kwq

Two axes: who decides (the producer at enqueue, the consumer at idle
transition, a timer, the scheduler) and what is deferred (the wakeup
call, the preemption it causes, or the whole pass).  The options,
named W0-W6 for the comparison:

| | decides | defers | prior art | status for kwq |
|---|---|---|---|---|
| W0 | producer | nothing: wake on first item | taskqueue, netisr deferred | baseline, today |
| W1 | consumer | the sleep (linger/poll after empty) | NAPI defer_hard_irqs, SQPOLL, ULE idlespin | built, declined 2026-10-11 |
| W2 | producer | the wakeup call, behind a count and a deadline | NIC ITR, NVMe, hpts below threshold | proposed |
| W2a | consumer sets W2's count from the last batch | the wakeup call | virtio event_idx, IX adaptive batching, DIM | proposed, on W2 |
| W3 | timer only; producers never wake | the wakeup call, unconditionally | hpts above threshold, polling(4) | a setting of W2 (count = infinity) |
| W4 | producer wakes at a non-preempting priority; deadline or count promotes | the preemption | share limit's hand-over (S6.3), cap_prio | proposed |
| W5 | producer runs the handler itself when the worker is idle | the pass and the wake | netisr hybrid, mp_ring, squeues | barred for NET (lock recursion); BULK later |
| W6 | consumer waits a settle time after waking before swapping the list | the pass | none in production | rejected: pays the switch and starves a same-CPU producer |

W1 and W5 are not rebuilt here; W1's branch can be rebased as a
comparison stage if a result from W2 makes the comparison interesting.
W3 and W6 need no code of their own.

### 4.3 W2: the deferred doorbell

Semantics.  A queue carries two new parameters, a count `wake_items`
and a deadline `wake_delay`.  When an enqueue finds the (queue, CPU)
list IDLE and the worker asleep, it places the queue on the worker's
lists exactly as today (so the DRR, the grace rule and the new-list
boost see nothing new) but, if `wake_delay` is non-zero and the list's
depth is below `wake_items`, it does not call `wakeup_one()`; it arms
the worker's wake callout for now + `wake_delay` unless one is armed
for an earlier time.  Every later enqueue into a deferred list whose
depth reaches `wake_items` wakes the worker at once; so does
`kwq_notify()` (a signal is by definition urgent) and the new call
`kwq_kick(q, cpu)` (the producer's end-of-burst, the `xmit_more`
FreeBSD lacks).  When the callout fires it wakes the worker.  The
worker, when it wakes for any reason, cancels nothing: an armed
callout that finds the worker awake is a no-op and is cheaper than a
`callout_stop()` on every wake; it is stopped only when the worker goes
idle with it armed, which cannot happen (an armed callout implies a
placed queue, which implies work).  There is no restarting timer (the
em `rx_int_delay` semantics): it would cost a callout_reset per item,
and the count threshold gives a burst its wake earlier anyway.

Costs.  The arm is 49-80 ns on the producer in place of a 2-6 us
wake; the deadline fires in the timer interrupt on the worker's CPU
(`C_DIRECT_EXEC`) and performs the same `wakeup_one()` as today, once
per burst instead of once per item.  Two placements of the callout are
possible and both are to be measured: on the worker's CPU
(`callout_reset_sbt_on(..., kw_cpu)`, so that the wake needs no IPI,
at the price of a remote callout-wheel lock and possibly an
`IPI_HARDCLOCK` to a CPU in deep idle), or on the producer's CPU (local
arm, the IPI at the deadline as today, once per burst).  The precision
argument should be `C_PREL` of the deadline so the event timer merges
it with its neighbours; the share limit's note on `nstosbt()` applies.

What it changes in the model.  Latency: the first item of a sparse
burst waits up to `wake_delay`; `maxlat_ns` shows it because
`kc_empty_since` is stamped at the first enqueue.  Under load nothing
changes: the worker is awake and the deferral never engages, which is
the property KWQ.md S16 values ("no added latency when the system is
idle" becomes "no added latency when the system is busy", and the idle
case pays the deadline).  Fairness: none; the DRR runs on the placed
lists as before.  Drain: `kwq_drain()` must wake the worker regardless
of the deferral.  Notifiers: immediate, as said.  The 4BSD scheduler:
nothing priority-related, so no caveat.

Against S16's objections: no staging area (the items are in the kwq
list, visible to `depth`, `maxdepth` and the limit from the moment of
enqueue); no second state machine (one armed flag and one deadline per
worker); the flush timer and the tunable remain, and are the price,
which is why the default is off (`wake_delay` 0) and the proposal's
exit criterion decides whether a class default other than 0 is earned.

Expected effect, by regime: the fan-in one- and two-producer rows
should move from 1-2 items per pass toward `wake_items`, i.e. toward
the four-producer row's 133 items and 5 M items/s; the a07 single flow
should lose most of its 31 k wakeups/s in the fast regime with
throughput unchanged and the worker's and producer's CPU down by the
wake cost; the guest's same-CPU flow should improve only insofar as
the receiver finishes its socket-lock section within the deadline
(W4 is the tool for that case); `pingpong` should get slower by
exactly `wake_delay`, which is the calibration check.

### 4.4 W2a: the consumer sets the count

virtio's consumer, having drained a burst, publishes the index at which
it wants the next kick; IX grows its batch with the backlog; DIM
climbs a profile table.  For kwq the signal is already in the worker:
items per pass of the last passes of that queue.  W2a sets the queue's
effective `wake_items` at each idle transition to a clamped moving
average of its recent batch sizes (one item when the queue has been
delivering single items, i.e. a trickle whose latency should not be
taxed; up to a cap when it has been delivering bursts), with
`wake_delay` as the static safety.  It costs one multiply at pass end
and reads nothing new.  It is the only variant that can earn a default
other than off, because it removes the tunable S16 objected to; it is
also the one most likely to oscillate (a burst that arrives after a
trickle pays the deadline), so it must be measured on the bimodal a07
flow, which is exactly the regime switch it has to follow.

### 4.5 W4: the non-preempting wake

A same-CPU producer cannot be coalesced with a timer: the wake at
PI_NET preempts it at the next instruction, so the burst never forms,
and the guest's 9.5 k lock blocks per second are the result.  The
remedy is the share limit's hand-over (SCHED.md S6.3), applied at the
wake instead of only when over the allowance: the worker lowers its
priority to `wake_prio` before it sleeps and raises it to the class
priority when its pass starts (the pass-level invariant holds: a pass
runs at the class priority and is preempted by nothing but interrupt
threads); a wake then makes the worker runnable without preempting
the producer (`sched_shouldpreempt()`: a priority above
`preempt_thresh` 40 preempts only an idle CPU, so a remote idle CPU
still runs the worker at once); the producer's burst completes and the
worker runs when the producer blocks, yields or exhausts its slice, or
at the latest when the deadline callout, the same callout as W2's,
promotes it with `sched_prio()`, 30-80 ns, and ULE preempts.  A count
threshold promotes earlier the same way.  `wake_prio` has the same two
useful values as `cap_prio`: `PRI_MIN_IDLE` 224 and
`PRI_MAX_TIMESHARE` 223, with `PUSER` 56 a third candidate that places
the worker ahead of the batch queue without preempting; 4BSD's
rewriting of kernel-thread priorities in the timeshare range applies
as in S6.3.

What it changes: delivery latency on a CPU busy with user work is
bounded by the deadline instead of being immediate, for every client
of the class, which is why it is a class knob with the default equal to
the class priority (off).  It is the P4c interactive goal reached from
the other side: the worker gives way at the wake rather than being
demoted after its allowance.  It depends on the pass-level share-limit
code (branch kwq-p4d-passlevel, which re-asserts the class priority at
pass start and owns the deadline callout), so it is built after P4d
merges, on top of W2's callout.

Expected effect: on the guest the lock blocks and the system-wide
switch rate fall and items per pass rise, as the spinner case already
showed (1.8 k wakeups/s, 5 items per pass, 8.6 Gbit/s); on a07 and
lab, where the receiver is elsewhere, nothing in the single flow, and
in the P4c battery the timer's p99 and the exec loop improve without
the share limit engaging; the risk to measure is the deadline's
latency in `udp_idle` and the flow's RTT beside a spinner.

### 4.6 The API, as proposed

Per queue, in `struct kwq_params` (zero keeps today's behaviour):

    u_int   wake_items;     /* wake the worker once this many items wait
                               on a CPU list; 0 = the class default,
                               UINT_MAX = never by count (timer only) */
    u_int   wake_delay_us;  /* deadline for a deferred wake; 0 = wake on
                               the first item (no deferral) */

and one data-path call, never sleeping, never allocating:

    void    kwq_kick(struct kwq *q, int cpu);   /* end of burst: wake now
                                                   if a wake is deferred */

Per class, RWTUN sysctls as defaults and for experiments:
`kern.kwq.<class>.wake_items` (1), `wake_delay_us` (0), `wake_adapt`
(0; W2a: 1 = set wake_items from the last passes, with wake_items as
the cap) and `wake_prio` (the class priority; W4).  Per queue the two
parameters are runtime-writable through the queue's sysctl node, as
`limit` is.  `kwq_enqueue()` and `kwq_enqueue_list()` keep their
signatures; no enqueue flags.  Notifiers wake immediately.  The
handler contract is unchanged: a handler cannot tell whether its pass
was woken by an item, a count, a deadline or a kick, and must not care.

Observability: per worker `wakes_now`, `wakes_count`,
`wakes_deadline`, `wakes_kick`, `wake_defer_ns` (the sum of the
deferrals, so that `wake_defer_ns / (wakes_count + wakes_deadline +
wakes_kick)` is the mean latency the mechanism added) and, for W4,
`wake_promotions`; `kwq:::doorbell` gains the state value 3 "deferred"
beside awake, polling and asleep; a new probe `kwq:::wake` with the
reason as its argument; DDB shows the armed deadline.  Per queue
nothing new: `maxlat_ns` and `items/passes` already show the effect.

Documentation: KWQ.md S3 (the API table), S10.7 (sysctls), S16 (the
position on producer-side batching is revised to "no staging; a
deferred doorbell is a queue parameter, default off"), SCHED.md a new
S6.4 "The wake" (W2, W2a, W4 and their interplay with the share limit's
callout), OBSERVABILITY.md, docs/reference.md and cheatsheet.md (when
to set `wake_delay_us`: a client whose producer enqueues one item per
call from a fast loop and can tolerate the deadline).

## 5. Proposal: what to build for the comparison

Build, each on its own branch from the merged P4d head, all default off:

1. **W2, the deferred doorbell**, static `wake_items` and
   `wake_delay_us`, `kwq_kick()`, both callout placements behind a
   loader tunable for the measurement, the counters and the probe.
   About 150 lines in kwq.c and kwq_worker.c, 40 in the simulator, the
   knob table entries.
2. **W2a, adaptive count**, on W2: the per-queue moving average and
   the `wake_adapt` knob.  About 40 lines.
3. **W4, the non-preempting wake**, on W2 and the pass-level share
   limit: `wake_prio`, the priority dance around the sleep, the
   promotion in the deadline handler and in the count path.  About 80
   lines, mostly in kwq_worker.c.

Measure on the guest, a07 and lab on one boot each, with the stages A
(baseline), W2 at (8, 10 us), (8, 20 us), (32, 50 us), (UINT_MAX,
50 us) (this last one is W3), W2a at cap 32 and 20 us, W4 at 223 and
56 with 50 us, W4 plus W2 (8, 20 us), and the taskqueue driver:

- tests/compare.sh with `tput1 tput16 udp_idle udp_load coloc spin
  timer exec machine` for Gbit/s, wakeups/s, items per pass,
  switched/s, worker CPU-seconds per second, the UDP idle latency and
  the P4c interactive columns;
- the kwq_test rows: `fanin` at 1, 2, 4 and 16 producers with one item
  per call (items per pass and items/s), `cost` (the bistable 3 vs 20
  items per pass), `pingpong` (the deadline calibration: round trip
  must grow by `wake_delay_us`, within a microsecond);
- tests/t_30_wakeups.sh for the sleep-gap distribution and the
  context-switch rate, tests/t_32_doorbell.sh for the enqueue cost by
  state with "deferred" added, tests/t_31_rtt.sh for the flood and
  the 1 kHz ping (the latency the deadline adds to a sparse flow is the
  cost that must appear exactly, not approximately).

Exit criteria, decided by the numbers and recorded in NOTES.md and here:

- W2 stays if at some setting on every machine the sparse-flow wake
  rate falls by at least 5x with throughput inside the run-to-run
  spread, worker plus producer CPU per packet measurably down (the
  saving the idle poll could not show), the fan-in one- and two-
  producer rows move toward the four-producer row, and the added
  latency equals the deadline (pingpong, 1 kHz ping) and nothing more.
  Otherwise it is declined and recorded like the poll, with the
  numbers.
- W2a earns a class default other than off only if it matches the best
  static setting on every machine without tuning and does not pay the
  deadline on the a07 regime switch more than the static setting does.
  Otherwise W2 stays a per-queue opt-in.
- W4 stays if on the guest it cuts the lock blocks and the switch rate
  by at least 3x and raises items per pass with throughput not down,
  if on a07 and lab the single flow does not move, and if the P4c
  columns improve or hold with the deadline bound visible in udp_idle.
  Otherwise it is recorded as the share limit's job alone.

Order: W2 can branch from kwq today and be measured with the kwq_test
rows before the a07 P4d run; W4 waits for the P4d merge.  If W2 is
declined, W2a and W4's count path fall with it and W4 is measured with
the deadline alone.

What this study does not propose: a restarting timer (one callout per
item), hybrid dispatch for NET (the recursion), a settle wait after the
wake (W6), any poll (W1, measured), and any change to the pass, the
budget or the DRR.  The one upstream item it suggests is an
end-of-burst hint in the transmit path, FreeBSD's `xmit_more`, for
KWQ.md S11's list of what is missing; `kwq_kick()` is its receiving
end.

## 6. References

- J. Mogul, K. Ramakrishnan, "Eliminating receive livelock in an
  interrupt-driven kernel", USENIX ATC 1996; ACM TOCS 15(3), 1997.
- P. Druschel, G. Banga, "Lazy Receiver Processing (LRP): a network
  subsystem architecture for server systems", OSDI 1996.
- M. Aron, P. Druschel, "Soft timers: efficient microsecond software
  timer support for network processing", SOSP 1999; ACM TOCS 18(3),
  2000.
- J. H. Salim, R. Olsson, A. Kuznetsov, "Beyond Softnet", USENIX
  Annual Linux Showcase 2001.
- C. Dovrolis, B. Thayer, P. Ramanathan, "HIP: hybrid
  interrupt-polling for the network interface", ACM SIGOPS OSR 35(4),
  2001.
- L. Rizzo, "netmap: a novel framework for fast packet I/O", USENIX
  ATC 2012; L. Rizzo, "Device polling support for FreeBSD", BSDConEurope
  2001 and polling(4).
- S. Garzarella, G. Lettieri, L. Rizzo, "Virtual device passthrough
  for high speed VM networking", ANCS 2015 (ptnetmap).
- A. Belay et al., "IX: a protected dataplane operating system for
  high throughput and low latency", OSDI 2014.
- M. Marty et al., "Snap: a microkernel approach to host networking",
  SOSP 2019.
- A. Ousterhout et al., "Shenango: achieving high CPU efficiency for
  latency-sensitive datacenter workloads", NSDI 2019; J. Fried et al.,
  "Caladan", OSDI 2020.
- M. Thompson et al., "Disruptor: high performance alternative to
  bounded queues", LMAX 2011.
- P. Cai, M. Karsten, "Kernel vs. user-level networking: don't throw
  out the stack with the interrupts", Proc. ACM Meas. Anal. Comput.
  Syst. 7(3), 2023 / SIGMETRICS 2024; J. Damato, "Suspend IRQs during
  application busy periods", Linux 6.13 (`irq-suspend-timeout`).
- Linux documentation: Documentation/networking/napi.rst (budget,
  `napi_defer_hard_irqs`, `gro_flush_timeout`, busy polling, IRQ
  suspension, threaded NAPI); Documentation/networking/net_dim.rst
  (T. Gilboa, Net DIM, 2018).
- S. Tripathi, N. Droux, T. Srinivasan, K. Belgaied, "Crossbow: from
  hardware virtualized NICs to virtualized networks", ACM VISA 2009
  (squeues, dynamic polling).
- virtio 1.x specification, "Virtqueue notification suppression"
  (`VIRTIO_RING_F_EVENT_IDX`); NVM Express base specification,
  "Interrupt Coalescing" feature.
- FreeBSD 15.1 sources read for this study on lab, 2026-10-24:
  sys/kern/sched_ule.c, sys/sys/priority.h, sys/net/netisr.c,
  sys/net/iflib.c, sys/net/mp_ring.c, sys/kern/subr_taskqueue.c,
  sys/kern/subr_gtaskqueue.c, sys/kern/kern_intr.c,
  sys/kern/kern_timeout.c, sys/kern/kern_poll.c,
  sys/netinet/tcp_hpts.c, sys/dev/e1000/if_em.c, sys/dev/ixgbe/if_ix.c,
  sys/dev/mlx5/mlx5_en/mlx5_en_ethtool.c and mlx5_en_dim.c,
  sys/dev/cxgbe/t4_sge.c and t4_main.c,
  sys/dev/virtio/network/if_vtnet.c.
- This tree: KWQ.md S16, SCHED.md S1, S6.3, S15; NOTES.md 2026-10-09
  to 11 (the idle poll, t_30/t_32/t_33), 2026-10-18/19 (the wake path
  A/B), 2026-10-21 (schedprobe, P4d); branch kwq-spin-experiment
  007f0f4.

## 7. Measured: the two candidates on the three machines (2026-10-24)

Built the same day on two branches from ead65d1, both off by default,
each with the same two knobs so that one setting means one thing:

- **W2, kwq-wake-defer 5ac763c, the deferred doorbell.**  A producer
  that finds the worker asleep does not wake it while the CPU list is
  below `wake_items` and `wake_delay_us` have not passed since the
  first deferred element; the list is flagged in its own cache line,
  one direct callout per worker carries the deadline, a notifier, a
  drain, a cancel or `kwq_kick()` wake at once.  Knobs
  `kern.kwq.<class>.wake_items` (1), `wake_delay_us` (0, off),
  `kern.kwq.wake_co_local` (0: the callout on the worker's CPU), the
  same two fields in `kwq_params` per queue; counters `wakes_now`,
  `wakes_count`, `wakes_deadline`, `wakes_kick`, `wakes_deferred`,
  `wake_defer_ns`; probe `kwq:::wake`.
- **W4, kwq-wake-prio a20dc0f, the non-preempting wake.**  The worker
  sleeps at `kern.kwq.<class>.wake_prio` (default the class priority,
  off), so its wakeup makes it runnable without preempting the thread
  on its CPU; it is raised to the class priority when it gets the CPU
  on its own, when a list reaches `wake_items`, at the `wake_delay_us`
  deadline (the same callout, armed on the producer's CPU by default),
  or at once for the urgent callers.  Counters `wakes_low`,
  `wake_self`, `wake_promotions_{count,deadline,urgent}`,
  `wake_low_ns`.  kwq_test gained `prod_prio` and a producer on the
  consumer's CPU, which is how the same-CPU rows below are produced.

A word on the three machines before the numbers.  lab and a07 run
GENERIC kernels and carry every quantitative conclusion below.  The
guest runs GENERIC-DEBUG (INVARIANTS, WITNESS, four vCPUs under bhyve,
hz 100): its lock operations are many times dearer and its timer is
late by tens of microseconds, so it is where the mechanisms are
verified (ATF, the assertions, the counters' identities) and where a
direction can be confirmed, never a magnitude.  Where a guest figure
appears beside a hardware figure it is for that purpose only.

Stages (compare.sh, three repetitions interleaved, medians;
samples/p4e_compare_2026-10-24_three_machines.txt and the TSVs): W0 the
baseline, T the taskqueue driver, D10/D20/D50/D100 W2 at `wake_items`
8 and the deadline in us, DT50 and DT20 W2 at `wake_items` 1000000
(the deadline alone), P223 and P56 W4 at `wake_prio` 223 and 56 with
(8, 50 us), P223L at (8, 200 us).  The kwq_test rows
(samples/p4e_rows_{a07,lab,guest}_2026-10-24.txt) add fan-in at 1, 2,
4 and 16 single-item producers, `cost`, `pingpong`, the ifpair shape at
1 us of producer and 1.5 us of consumer work per packet (consumer-
bound) and at 3 and 10 us of producer work against 0.5 us (the sparse
regimes, S and X), and `coloc`, the producer at a user priority on the
worker's CPU.  a07's battery ran last, in the evening, after crest
unloaded their taskqueue if_pair.ko, which the battery needs gone.

### 7.1 The sparse cross-CPU producer: W2 does what coalescing does

a07, one sender at 3 us per packet into a worker that needs 0.5 us
(ifpairS1), medians of three:

| stage | Mpkt/s | enqueue ns (% of sender CPU) | wakes per 3 s | items/pass | queue latency mean/max us |
|---|---|---|---|---|---|
| W0 | 0.26 | 844 (21 %) | 787 k | 1 | 1 / 38 |
| D10 | 0.30 | 261 (7 %) | 185 k | 2 | 6 / 47 |
| D20 | 0.31 | 188 (5 %) | 105 k | 4 | 10 / 54 |
| D50 | 0.31 | 209 (6 %) | 103 k | 4 | 11 / 31 |
| DT20 | 0.31 | 186 (5 %) | 105 k | 4 | 10 / 52 |
| DT50 | 0.32 | 82 (2 %) | 45 k | 7 | 23 / 92 |
| P223 | 0.24 | 1060 (25 %) | 722 k | 1 | 2 / 31 |

The baseline wakes the worker for every packet and the sender pays
the wake: 21 % of its CPU inside `kwq_enqueue()`.  Ten microseconds of
deadline remove three quarters of the wakes and two thirds of that
cost and lift the packet rate 15 % (the sender's loop is the
bottleneck); the deadline alone at 50 us removes 94 % of the wakes at
2 % of the sender's CPU.  Two senders (S2) and the 10 us sender (X1,
X2) show the same shape: enqueue 937 to 1133 ns down to 190 to 350 ns,
+10 to +22 % packets.  W4 is pure overhead here, as predicted: the
target CPU is idle, so the low-priority wake runs at once anyway, and
each wake now also arms and stops a callout and changes priority twice
(+25 % enqueue cost, +0.25 us round trip in `pingpong`).

lab's version of the same row (S1, the sender on the worker's SMT
sibling): 908 k wakes and 7 % of the sender's CPU at the baseline;
D10 193 k wakes and 2 % at +7 % packets; DT50 44 k wakes and 1 %; W4
+37 to +46 % enqueue cost.  The same three machines, the same shape.

a07's real single flow (tput1) is the regime the study was framed
on: 37.5 Gbit/s with 35.5 k wakes per second and 3 items per pass at
the baseline, the worker CPU 81 % busy.  W2 with the count of 8 lifts
items per pass to 5.8 and cuts the worker CPU's system share 19 to
22 % (idle 0.19 to 0.36), but removes only 4 to 10 % of the wakes,
because the count ends most deferrals after 7 us, and costs 2 to 10 %
throughput; the deadline alone at 50 us removes 87 % of the wakes
(4.7 k per second), reaches 7.4 items per pass, saves 12 % of the
CPU and costs 8 % throughput.  The flow's own loop now carries the
deadline: each direction's first packet of a burst waits for it, and
the ACK clock slows.  W4 changes nothing in this flow (CPU within
1 %, 4 to 11 % less throughput).

The guest's real flow (tput1, W2 stages): the worker CPU's system
share falls from 0.79 to 0.56 to 0.68 and its idle share rises from
0.21 to 0.32 to 0.44 at equal or better throughput (9.3 to 9.4 to
10.1 Gbit/s), wakes per second from 2520 to 1390 to 2450 and items
per pass from 4.4 to 10.3 at D100.  lab's single flow is the other
regime: the worker CPU is saturated (sys 0.996) at 137 to 139 Gbit/s
with 694 wakes per second, and no stage changes a column except DT50's
4 % of idle.  A saturated worker has nothing to coalesce.

The price is exact and doubles: lab's UDP request-reply with nothing
else running goes from 7 us p50 to 30, 51, 115 and 221 us at 10, 20,
50 and 100 us (both directions wait, plus the 1/16 precision), and
a07's `pingpong` one-way from 3.66 us to 13.9, 24.5, 56.4 and 109.6.
W4 adds 1 us on lab and 0.25 us on a07.

### 7.2 The same-CPU producer: both candidates work, differently

`coloc`, kwq_test's producer at priority 100 on the worker's CPU,
the counting handler:

| stage | a07 wall ns/item | a07 items/pass | a07 switched passes | guest wall ns/item | guest items/pass |
|---|---|---|---|---|---|
| W0 | 1486 | 1 | 16 | 4541 | 1 |
| D10 .. D100 | 259 to 282 | 7 | 50 to 34 k | 950 to 2211 | 7 |
| DT20 | 76 | 373 | 15.9 k | 540 | 102 |
| DT50 | 74 | 934 | 6.3 k | 355 | 295 |
| P223 / P56 | 284 / 283 | 7 | 25 / 29 | 1155 / 1147 | 7 |

lab's `coloc` rows read 1142 ns per item at one item per pass, 186 to
201 with the count of 8 (W2 and W4 alike) and 46 to 65 with the
deadline alone (660 to 1020 items per pass).

The baseline is the wake-and-be-preempted pattern in its pure form:
every item wakes the worker, the wake preempts the producer, one item
per pass, 1.5 us per item on a07 (2.0 M wakes in three seconds).
With a count of 8 both candidates stop at 7 items per pass, five
times better, because the count is what ends the wait; the deadline
alone lets the producer run its whole slice and hands the worker
hundreds of items, twenty times better.  W4's `self` counter is zero:
the worker never got the CPU on its own before the count promoted it.

The real flow, compare.sh's `coloc` (the iperf3 receiver pinned to W,
the client elsewhere), medians of three:

| | lab Gbit/s | lab switched/s | lab items/pass | guest Gbit/s | guest switched/s |
|---|---|---|---|---|---|
| W0 | 55.8 | 53.6 k | 4.1 | 7.06 | 6.8 k |
| T | 98.1 | | | 7.38 | |
| D10 / D20 / D50 | 62.4 / 64.2 / 69.1 | 8.1 k / 5.1 k / 3.6 k | 5.3 / 7.9 / 18.9 | 6.14 / 6.45 / 7.22 | 3.4 k / 2.9 k / 2.1 k |
| DT50 | 69.7 | 2.6 k | 19.7 | 7.26 | 2.1 k |
| P223 / P56 | 58.5 / 65.2 | 195 / 58 | 4.3 / 4.1 | 6.39 / 6.37 | 155 / 45 |

a07's `coloc` (not in the table): baseline 17.7 Gbit/s with 17.1 k
switched passes per second, the taskqueue driver 32.9 (+86 %); W2
+9, +25, +32 and +43 % at 10, 20, 50 and 100 us (items per pass 4.1
to 13.5, switches down 75 to 94 %), the deadline alone at 50 us
+34 %; W4 +9 % at 223 and -1 % at 56 with the switches down 99 %.
On a07 removing the preemptions alone recovers little; the batch is
what pays, and only W2 batches.

On lab the baseline pays 53.6 k switched passes per second, i.e. the
worker is preempted or blocked in nearly every pass, and the taskqueue
driver, whose thread wakes less and runs longer per wake, is 76 %
faster.  W2 recovers 12 to 25 % with the switches down 85 to 95 %; W4
at 56 recovers 17 % with the switches gone (58 per second) and items
per pass unchanged, which says the gain comes from not preempting the
receiver inside its socket-lock section rather than from batching.
On the guest W2 at 10 and 20 us loses 9 to 13 % and W4 loses 10 %
with the switches down 98 %: on four slow vCPUs the receiver that is
not preempted runs on, and the flow's other direction waits.

### 7.3 Fan-in with a free handler: the pass rhythm, not the wake

The fan-in rows (a07, medians of three) are bistable: one producer
gives 8.4 M items/s at 195 items per pass on the baseline, two
producers 3.0 M at 3 items per pass, four 4.4 M at 10, sixteen 7.0 M
at 509; the earlier two-repetition sweep had the two-producer case at
4.8 M and 20 per pass.  Every W2 and W4 setting puts the one-producer
case into the 3-items-per-pass attractor (3.1 to 3.2 M, 5.1 M for
P223), the deadline-only settings too in this sweep though not in the
earlier one, while four producers improve under D100 and DT50 (7.6 to
7.8 M, 108 to 114 per pass).  The worker in these rows is not asleep
(idle 125 to 190 ms of 3 s): with a 22 ns handler the pass takes less
time than the producer needs to refill, so each pass finds what one
pass produced, and whether the system settles into long or short
passes depends on who leads at the start.  A wake mechanism cannot
address it and should not be tuned on it; it is the same bistability
SCHED.md S15.7 and NOTES.md 2026-10-20 recorded for `cost`, now with
a second knob that flips it.

lab's fan-in rows add one reading the other two machines do not
show, because its producer sits on the consumer's SMT sibling: the
deadline alone more than doubles the one-producer rate (12.8 M at 4
items per pass with 217 k wakes to 29.1 M at 25 per pass with 33 k
wakes at DT20; 14.5 M at 1011 per pass at DT50) and lifts two
producers from 9.9 M to 16.3 to 17.5 M, while the count of 8 loses
7 to 25 % and W4 loses a third (the low-priority wake on a sibling
that is busy with the producer waits for the count).  On a shared
core a sleeping sibling is a faster sibling, and a deferral that
lets the producer finish its burst before the consumer competes for
the core's resources pays twice.

### 7.4 Side effects

Interrupts: every deferred or low-priority wake arms a callout, and
`machine intr_s` rises 21 to 99 % on the three machines (lab 23.7 k
to 32 to 44 k per second, a07 284 k to 342 to 461 k).  The mechanism
is inherent: the event timer runs one-shot, the next hardclock is a
millisecond away, and a 10 to 100 us deadline with a precision of a
sixteenth cannot merge with any existing event, so each arm
reprograms the timer and each deadline is a timer interrupt.  It
fires even when the deferral ended early: W2 leaves the callout
pending by design, and W4's callout_stop() removes the entry from the
wheel but not the programmed timer, whose interrupt then finds
nothing to run.  A deferral that the count or a kick ends early
therefore costs both the interrupt and the wake it meant to save; the
deferral pays only when the deadline ends it and the wake it replaced
would have been an IPI and a switch (2 to 8 us) against one timer
interrupt (about a microsecond).  A NIC's coalescing timer is free;
kwq's is not, and this column is where that shows.

Interactive threads beside the flow on a07: the 1 kHz timer's p99
lateness doubles to triples under W2 (100 us to 197 to 307) and rises
56 to 110 % under W4, and the fork+exec loop's average 12 to 24 %
under W2; lab's and the guest's do not move.  The worker's passes
grew (3 to 5.8 and 7.4 items on a07), and a thread that runs between
passes waits for the longer pass; the share limit bounds what it
waits for a pass-level allowance, not a single pass.  A moderation
setting changes the pass length and therefore the interactive bound,
which belongs in SCHED.md S6.3's accounting if either lands.  The share limit's columns (`spin`, `timer`,
`exec`, `machine` lateness) do not move: the candidates act before the
pass, the limit between passes.  Sixteen flows do not move.  The UDP
flow beside the bulk flow on its CPU (`udp_load`) pays W2's deadline
as the idle one does (lab p50 14 us to 23, 33, 66, 119) and gains
under W4 at 223 (p50 9, p99 15 us, the best of any stage) while the
bulk flow loses 9 % there and the worker's wakes rise tenfold to 6 k
per second at 3 items per pass; at 56 the bulk flow loses 4 % and the
latency is the baseline's.  ULE's `static_boost` rewrites a
timeshare-class sleeper's priority to 120 when it is worse, so 223
and 120 are the same sleep and 56 is the one setting with a distinct
meaning.  The guest's W4 stages lose 5 to 7 % on the single flow; the
hardware's lose 1 %.

### 7.5 How much latency the NET class should spend, by case

The deadline buys three things, and each case buys a different one:

1. **A sparse producer on another CPU** (the first wake of every
   small burst) buys the producer's CPU and the IPI.  The return is
   front-loaded: on a07 the first 10 us remove 76 % of the wakes and
   two thirds of the sender's enqueue cost, 20 us remove 87 % and
   three quarters, 50 us 94 % and nine tenths.  The cost is linear and
   doubled for request-reply traffic: a 10 us deadline adds 21 us to a
   7 us round trip on lab and 20 us to a 13 us one on a07.  On a07's
   real TCP flow the same deadline saves 20 % of the worker's CPU and
   costs 2 to 10 % of the throughput, because the flow's ACK clock
   pays it too.  Knee: 10 to 20 us, worth it only for a queue whose
   producer is known to be sparse and whose consumer neither answers
   requests nor clocks the producer, and only as a per-queue opt-in
   (`kwq_params`), never as a class default.  A saturated worker
   (lab's flow) gains nothing and should not be asked to pay, and a
   count that ends the deferral early pays the timer interrupt and
   the wake both.
2. **A producer on the worker's own CPU** buys the producer's
   uninterrupted burst, and here the two candidates differ in what
   they charge.  W4 charges nothing while the CPU is idle (the
   low-priority wake runs at once) and at most the deadline while a
   user thread runs, and it recovered 17 % of the pinned receiver's
   throughput on lab at a 1 us idle-latency cost; a 50 to 200 us
   deadline is affordable because it binds only when a user thread
   would otherwise have been preempted mid-section.  W2 charges its
   deadline always and recovers more (25 % on lab, 43 % on a07)
   because it also batches; on a07, where the receiver's stack work
   per packet is small, W4 alone recovers 9 % at 223 and nothing at
   56: there the batch is the gain, not the preemption.
   For the same-CPU case the budget is W4's: as much as the user
   thread's own slice policy allows, bounded at 50 to 200 us, with the
   count promotion removed or set high (the count of 8 is what capped
   every same-CPU row at 7 items per pass).
3. **Two producers into one list** buy nothing: the worker is awake
   and the rhythm of its passes, not their wake, sets the batch.  No
   latency should be spent there; the lever is the pass (a settle or
   a minimum pass interval), which is a different study.

The fan-in rows on lab add a fourth, smaller case: producer and
consumer on SMT siblings, where the deadline alone doubles the rate
at the deadline's latency, and nothing else does.

Recommendation after the measurement: keep both off by default; keep
W2 as the deadline-only variant (a count threshold hurt in two of the
three cases, helped in none, and makes every early-ended deferral pay
twice) with `wake_delay_us` a per-queue
parameter in the 10 to 20 us range for sparse producers; keep W4 as a
class knob at `wake_prio` 56 for hosts whose receivers share CPUs with
their workers, and before merging either, give W4 the one fix the
cross-CPU rows ask for: when the worker's CPU is idle at the wake
(`pcpu_find(cpu)->pc_curthread` is its idle thread), wake at the class
priority and arm nothing, so that the common case keeps today's cost
and the mechanism engages only where a thread would be preempted.
Give W2 a coarser precision (a quarter to a half of the deadline) so
that neighbouring deferrals on a CPU share one timer interrupt, and
record the pass-length effect on the interactive bound in SCHED.md
S6.3.  Then unify the two branches behind the shared knobs and
measure once more with compare.sh, with a many-sparse-connections
test added to the battery (the case neither tput16 nor machine
covers: tens of request-reply flows at a few kpps each across the
workers), which is where the per-connection saving and the per-wake
interrupt meet.  The latency numbers of this section are the
budget: 2 x deadline + 1 us on a round trip, exactly, on hardware.

## 8. The TCP stacks over the pair (2026-10-25)

Asked after the first battery: does the result depend on the TCP
stack?  Twelve stages on each machine, three repetitions (samples/
p4e_stack_2026-10-25_three_machines.txt): for each of freebsd, rack
and bbr, the baseline W0, the taskqueue driver T, W2 at (8, 20 us) and
the deadline alone at 50 us.  The stack is selected inside both vnet
jails per stage, through compare.sh's new `jail:` knob, because the
default-stack pointer is per vnet and a new jail starts on the freebsd
stack whatever the host's `net.inet.tcp.functions_default` says
(verified on lab with the host set to rack).  That also settles a
question about the earlier batteries: all of them, a07's included, ran
on the freebsd stack, although a07's host default is rack.

The guest's columns in this section are a debug kernel's (section 7's
caveat): the paced stacks' collapse there is a direction, its size is
not transferable.

### 8.1 The stacks against each other

The stack is the largest effect in the whole campaign, it is the same
with the taskqueue driver as with kwq, and it differs by machine:

| Gbit/s | a07 freebsd / rack / bbr | lab freebsd / rack / bbr | guest freebsd / rack / bbr |
|---|---|---|---|
| one flow (tput1) | 36.5 / 34.2 / 2.0 | 135 / 85 / 2.1 | 9.2 / 0.34 / 2.8 |
| sixteen flows | 366 / 110 / 31 | 74 / 106 / 33 | 15.6 / 8.3 / 11.3 |
| 2 x ncpu flows (machine) | 335 / 381 / 315 | 71 / 92 / 64 | 14.7 / 4.8 / 10.3 |
| receiver on the worker CPU (coloc) | 17 / 0.3 / 1.9 | 57 / 0.6 / 1.8 | 7.1 / 0.04 / 2.2 |
| UDP beside the flow (udp_load) | 38.7 / 8.0 / 1.9 | 140 / 65 / 2.3 | 9.1 / 0.28 / 2.7 |

bbr runs every single-flow test at 2 Gbit/s on all three machines,
and sixteen flows at a tenth to a half of freebsd's rate: its pacing
rate, derived from a delivery-rate estimate on a link with no delay
and no queue, never opens.  rack is two stacks: on a07 one flow is
within 6 % of freebsd and the 256-flow machine test 14 % better, but
sixteen flows run at 30 % and a flow sharing its CPU with a UDP flow at
20 %; on lab one flow runs at 63 % and sixteen flows 43 % better; on
the guest everything collapses (0.3 Gbit/s), its hz of 100 giving the
pacer a 10 ms tick.  The receiver pinned to the worker CPU is where
both paced stacks fail outright (0.04 to 0.8 Gbit/s on every
machine): the hpts thread, the receiver and the worker share one
CPU, and the pacer's slots pass while the receiver holds it.  All of
this is the stacks' behaviour on a loopback-like link, not kwq's: T
and W0 agree within each stack on every machine.

Two readings in rack's favour belong in the record: with rack a07's
single flow wakes the worker 161 times a second instead of 34.7 k,
because the pacer sends in timed bursts and the stack, not kwq, does
the coalescing; and the unpinned timer in the machine test is 99 %
less late under rack on all three machines (a07 20.9 ms to 0.19 ms),
since the paced flows do not saturate the worker CPUs.

### 8.2 The moderation under each stack

W2's effects do not depend on the stack.  Under rack and bbr the
UDP round trip pays the same 2 x deadline + 1 us (a07 13 to 55 and
118 us, lab 7 to 51 and 115 us, identical to the freebsd columns);
the worker CPU's system share falls by the same 17 to 24 % on a07's
single flow (freebsd) and by 3 to 11 % on lab's rack flow; and the
deadline alone keeps removing wakes where there are wakes to remove
(a07 freebsd 34.7 k to 5 k per second; lab rack 15.7 k to 2.2 k).
Where the stack already paces, there is nothing left for kwq to
coalesce (a07 rack: 161 wakes per second before any moderation),
and the deferral only adds its latency.  Under rack on a07, sixteen
flows improve from 110 to 194 and 215 Gbit/s with W2, the one case
where moderation changes a multi-flow number: the pacer's bursts
arrive at the worker one packet per wake, and the deferral gathers
them.

Conclusion for the study: the stack choice is a prior, larger
decision than any wake policy, the freebsd stack is the right one for
the pair's shape of link, and the moderation's budget of section 7.5
holds under every stack.  For the pair as a product, the paced stacks'
failure with a receiver pinned to the worker CPU is a finding of its
own: it is the share limit's case (a pacer thread that needs the CPU
while a worker runs on it), and belongs in P4d's follow-up rather
than here.

## 9. Two more candidates: batched doorbells and the idle-class wait (2026-10-25)

Built after section 7's recommendation, each on its own branch from the
same bases, then combined:

- **W7, kwq-wake-batch 74443c0, pass-scoped doorbell batching.**  A
  worker that, inside its own pass, rings the doorbell of a sleeping
  worker records the target in a per-worker table (eight entries, a
  full table rings at once) and performs the wake when the pass ends,
  before anything that may switch; the target's list is placed and
  WAKING as before, so only the `wakeup()` call waits.  Notifiers,
  drains and cancels are never batched; a worker never sleeps with a
  table outstanding (asserted).  Knob `kern.kwq.<class>.batch_doorbell`
  (0), counters `db_batched`, `db_flushed`, `db_flush_noop`,
  `db_overflow`, doorbell state 5.  No timer, no latency beyond the
  pass already running: netisr's single signal at the end of a hybrid
  dispatch, inside kwq.
- **W4 with the idle-class wait and the idle-CPU shortcut,
  kwq-wake-idle ab7bb9d.**  W4 as measured in section 7 is the
  non-preempting wake: the worker sleeps at `wake_prio` instead of the
  class priority, so a wake makes it runnable without preempting the
  thread on its CPU, and it is raised to the class priority when it
  gets the CPU on its own, when a list reaches `wake_items`, at the
  `wake_delay_us` deadline, or at once for an urgent caller.  Two
  things the first measurement asked for: with `wake_idle` set the
  wait happens in the idle scheduling class at PRI_MIN_IDLE
  (`sched_class`, restored with the priority at the promotion), which
  ULE runs the moment the CPU would otherwise idle and never ahead of
  a user thread, and whose sleep priority ULE does not rewrite to 120
  as it does a timeshare sleeper's (`static_boost`); and a wake whose
  target CPU is idle at that moment (its current thread is the idle
  thread, read without a lock) raises the worker at once and arms no
  callout, since an idle CPU runs it immediately anyway.  Counter
  `wake_idle_cpu`, wake reason 6.
- **kwq-wake-combo 12a7788**, the merge, with the batched flush routed
  through the W4 wake path.

Stages (samples/p4e_combo_2026-10-25_three_machines.txt, three
repetitions): W0, T, BD (batching on), WI (`wake_prio` 224,
`wake_idle` 1, no count, 200 us deadline), WS (`wake_prio` 56, count 8,
50 us: section 7's P56 with the shortcut), CB (BD and WI together).
The rows (p4e_rows_{a07,lab,guest}_2026-10-25.txt) and a short
follow-up battery with the new counters (p4e_combo_db_*) complete it.

The guest's figures below are a debug kernel's (section 7's caveat):
they confirm the direction and the mechanism on four slow vCPUs, the
magnitudes are lab's and a07's.

### 9.1 What changed, and in which direction

**The same-CPU producer, up by a lot.**  The pinned receiver (coloc):

| Gbit/s | W0 | T | BD | WI | WS | CB |
|---|---|---|---|---|---|---|
| lab | 55.3 | 97.6 | 57.6 (+4 %) | 76.5 (+38 %) | 62.7 (+13 %) | 77.3 (+40 %) |
| a07 | 15.9 | 32.2 | 17.2 (+8 %) | 26.9 (+69 %) | 19.9 (+25 %) | 26.9 (+69 %) |
| guest | 7.02 | 7.13 | 7.03 (0 %) | 8.26 (+18 %) | 6.35 (-10 %) | 8.24 (+17 %) |

WI's items per pass go from 4 to 67 (lab), 24 (a07) and 13 (guest),
its passes per second fall 62 to 92 %, the switched passes fall 85 to
98 %, and the wake episodes last the deadline (203 to 253 us): the
receiver never yields, so the worker runs when the deadline promotes
it, with everything the receiver produced meanwhile.  This is what
W4 at 56 with a count of 8 could not do (+13 to +25 % on hardware,
-10 % on the guest, 4 items per pass): the count ended its wait after
8 items, and the first measurement's loss on the guest is gone.  In
the rows the same case reads 46 ns per item at 1023 items per pass on
lab (W0 1140 ns at 1), 74 ns at 1023 on a07 (W0 1502 at 1) and 294 ns
at 988 on the guest (W0 4581 at 1), with every hardware episode ended
by the worker's own turn after 35 us (lab) or 59 us (a07), the
producer's slice, and six in ten of the guest's.  The taskqueue driver is still ahead (lab 98, a07 32).

**The single flow and the idle-latency cost, no longer down.**  The
first W4 lost 4 to 11 % on the single flow and 5 to 7 % on the guest;
with the shortcut WI reads lab -2 %, a07 +8 %, guest 0 %, the worker
CPU's system share within 2 % except a07's +7 %, and the UDP round
trip with nothing else running +1 us on every machine (lab 7 to 8,
a07 13 to 15, guest 414 to 410 us).  `wake_idle_cpu` accounts for
essentially every wake in the single-flow tests (lab 2.37 k of 2.37 k
per second), so the shortcut is what carries the single flow, and
`pingpong` on an idle target CPU got faster, 1.91 to 1.29 us on lab,
although the enqueue itself costs 250 ns more; the saving is on the
worker's side of the wake.

**Interrupts, mostly back.**  WI adds 3 % of interrupts on a07 and 23
to 37 % on lab and the guest in the machine test, against 74 to 99 %
for the first W4 and 91 to 100 % for WS, whose count-8, 50 us setting
still arms a callout per wake on a busy CPU.

**The interactive columns, unchanged or better.**  Timer lateness,
exec and the spinner's share do not move on lab and the guest; on a07
the pinned timer's p99 improves 40 % under WI and 57 % under WS (a
worker waiting in the idle class lets the timer thread go first),
where section 7's W2 had doubled it.

### 9.2 What stayed the same, and why

**Doorbell batching did nothing, anywhere, and the counters say why.**
`db_batched` is zero in every real flow on all three machines, BD's
columns equal W0's within noise, and the corrected same-CPU rows (the
sweep had paired the batch build with its own kwq_test.ko, which lacks
the same-CPU knobs, so BD's first coloc row was a cross-CPU run) equal
the baseline's.  A probe on the guest shows the reason: in a TCP flow
over one pair both directions hash to the same worker CPU (the flow
hash is symmetric in the addresses and ports), so the worker that
sends the ACKs from inside its pass rings the doorbell of a queue it
serves itself, and the target is by definition awake (53 k doorbells
in state awake from the worker, 6 k in state asleep from the iperf3
thread, 0 batched).  Forwarding between two pairs keeps the flow key
and therefore the CPU, so the same holds there.  A worker produces
into another CPU's sleeping worker only when the flow key changes
between the two hops, which is a tunnel or a crypto stage (P6's wg,
GELI), not a pair.  I expected batching to carry the ACK direction;
it cannot, by construction, and the mechanism should be measured
again only with such a client.

**The sparse cross-CPU producer, not helped.**  The idle-CPU shortcut
replaced the callout arm and stop by an immediate promotion, which
costs the producer the same or more: the sparse sender's enqueue on
lab reads 348 to 365 ns under WI/WS/CB against 232 at W0 (+50 %; the
first W4 +43 %), its packet rate 0.29 against 0.30 M/s; on a07 it
reads 1560 against 882 ns (+77 %; the first W4 +25 %) and the packet
rate falls 19 %, 0.26 to 0.21 M/s: raising a thread on another CPU
under its thread lock is dearer on the 128-core machine than arming a
local callout was.  I had hoped the shortcut would make the cross-CPU
idle case free; it made the wake's consumer side cheaper (pingpong
-33 % on lab, +7 % on a07) and the producer side no cheaper, and the
producer side is what a sparse sender pays.  The cheap-looking
remedy, arming nothing and promoting nothing on an idle target and
letting the worker raise itself when it runs (the `self` path, free
for the producer), gives up the bound: ULE counts a CPU whose only
runnable thread is idle-class as idle and migrates work onto it, and
a sparse producer sends no second item that could promote by count,
so the worker could wait a whole slice behind a thread that arrived
in the microseconds between the wake and its first instruction.  The
producer-side cost is the price of the bound, and for the sparse
cross-CPU case W2's deferral, which arms one callout per burst rather
than per wake, remains the cheaper mechanism.  W2's deferral remains the
only mechanism for that case.

**Fan-in and the pass rhythm, as before.**  The SMT-sibling fan-in on
lab loses a fifth to a third under every W4 variant (12.6 to 8.0 M at
one producer) inside the bistability of section 7.3; the guest's and
a07's fan-in columns move within their noise.  Nothing here was
expected to change and nothing did, except that the W4 variants
confirm the wait is not free when producer and consumer share a core.

**Two instabilities to record.**  CB's single flow on lab ran in two
modes between batteries: 137 Gbit/s at 629 wakes per second in the
three-repetition run and 104 Gbit/s at 34.9 k wakes per second (every
one through the shortcut, 5.75 items per pass) in the two-repetition
follow-up; WI's `maxlat` in the single flow rose to 268 to 341 us,
the deadline, on both hardware machines, so some wakes land while the
worker's CPU is busy with something other than the flow and then wait
the full deadline.  And the UDP flow beside the bulk flow pays WI's
wait on the guest (p50 121 to 158 us) and on lab's follow-up battery
nothing (14.5 to 13.5), so the deadline's cost to a small flow sharing
the CPU depends on whether the bulk flow's receiver holds the CPU.

### 9.3 Where this leaves the recommendation

The idle-class wait with the shortcut and no count is the W4 to keep:
it is the first variant that improves the same-CPU case on all three
machines (+17 to +69 %) while leaving the single flow, the idle
latency, the fairness columns and most of the interrupt budget alone.
Its open parameter is the deadline, which is the whole wait in the
pinned-receiver case (200 us here) and the bound on `maxlat` in the
single flow; 50 to 100 us is the next sweep, together with the
two-mode single flow on lab.  Batching stays on its branch until a
client with a changing flow key exists.  Section 7.5's budget stands:
W2 for sparse cross-CPU producers at 10 to 20 us per queue, this W4
for shared CPUs, nothing for fan-in.

## 10. The SPEC-p4e tests: the tick regime, the accounting modes and the priority assertion (2026-10-27)

Section 9 ended with two rules from crest and a test plan
(kwq/tests/SPEC-p4e.md): a NET worker runs every handler at its
configured priority and may demote or yield only between handlers;
and wall-time accounting is suspect, because a worker that is
switched out mid-pass is charged for the interruption, so any
alternative has to be measured under deliberate interruptions, with
the punishments (overruns, parks) counted and the two clocks compared.
This section is what the tests found.  Everything here is on branch
kwq-wake-tick (71ca39e module and kwq_test, 5258b7e harness, fixes
through 6d09c21); nothing is merged.  The guest is GENERIC-DEBUG:
direction only, never magnitude.

### 10.1 What was built

**The tick regime** (`kern.kwq.<class>.tick_us`, default 0 = off).
A NET worker has two sleep regimes.  In the doorbell regime it sleeps
without a timeout and the producer's first enqueue after a pass wakes
it, as today.  In the tick regime its sleep has a timeout of L (the
tick, `msleep_spin_sbt` with precision L/8), the doorbell is skipped
(`doorbells_skipped`) and the worker serves what accumulated when the
tick fires.  Entry: two doorbell wakes within L of each other (the
producer is dense enough that a tick would have served both); exit: a
tick that found no more than one element (the producer is sparse
again); a regime episode that never coalesced anything costs an
exponential entry hold-off (2L doubling to 1024L), one that did costs
none, so a steady dense producer re-enters at once and an isolated
pair of wakes does not keep flapping (5685f2e; the first build
judged the entering round and flapped on ping-pong).  No callout, no
per-queue state: one timeout per worker.  Counters `ticks`,
`ticks_empty`, `ticks_single`, `tick_enters`, `tick_exits`, `tick_ns`,
`tick_barren`, per queue `doorbells` and `doorbells_skipped`, probe
`kwq:::tick`.

**The charge modes** (`kern.kwq.net.charge`, default 0): `wall` charges
a NET pass the wall time it spanned (today); `cpu` charges the thread's
CPU time (`td_runtime` plus the running segment, read with interrupts
off) to everything, the deficit and the share limit alike; `split`
charges the thread's CPU time to the deficit and wall time to the
share limit and the round; `hybrid` charges wall time unless the pass
turns out to have been switched out, then CPU time.  `overruns_switched`
counts, beside `overruns`, the overruns of passes during which the
worker left the CPU (`td_runtime` moved more than the pass's own
segment), so an overrun is either the handler's or the system's.

**The priority assertion.**  Every pass begins by checking that the
thread is at the class priority and class (`td_priority <= kw_pri`,
`td_pri_class == kw_pri_class`); a failure counts in
`prio_violations` and, when built with `KWQ_HARD_INVARIANTS=1`,
panics.  The default build is soft (`-DKWQ_SOFT_INVARIANTS`).

**The tests.**  kwq_test gained `burst` (B items back to back, gap G,
a producer with a duty cycle), `intr_load` (a `C_DIRECT_EXEC` callout
re-armed from its own handler on the worker CPU, spinning
`intr_work_ns` every `intr_us` in interrupt context), `prod_gap_us`
for the ifpair senders and `prod_prio` for a same-CPU producer;
compare.sh gained `sparse` (N request-reply flows spread over the
workers), `udp_burst` and `udp_burst_load` (the victim fires B
requests, then collects the replies; first- and last-in-burst RTT),
`COMPETITOR=intr`, the coloc counters (`overruns`, `parks`, `debts`,
the receiver's involuntary switches) and per-CPU timer interrupts
(`intr_w_timer_s`, `intr_timer_s`); the row runner moved into the tree
(tests/kwq_rows.sh, kwq_rows_report.py) with the conformance rows;
four ATF cases (prio_invariant, tick_hysteresis, charge_modes,
no_arm_when_idle; 25/26 on the guest, charge_modes skips for want of a
blocking scenario).

**Batteries.**  Stages W0 (kwq ead65d1), T (the taskqueue driver),
TK20/50/100 (the tick build at 20/50/100 us, wall) on the default
tests plus coloc, sparse, udp_burst (samples/p4e_tick_* and
p4e_burst_*); the accounting stages CHW/CHC/CHS/CHH (the tick build at
50 us under the four charge modes) beside W0 on twoq, coloc, rwlock,
preempt, spin, timer and udp_load under the `burst` competitor (a 1 kHz
timeshare thread with 400 us of work per wake on W) and the `intr`
competitor (20 us of interrupt context every 100 us on W),
samples/p4e_charge2_* with the split mode re-run as p4e_charge3_*
(10.6); the kwq_test rows p4e_rows_*_2026-10-27.txt.  Three
repetitions for the tick and burst batteries, two for the accounting
ones, medians.

### 10.2 The tick regime against the lower bound (SPEC 2.4, 2.3)

The conformance rows drive one cross-CPU producer at lambda = 10 k,
100 k and 1 M items/s with constant and exponential spacing against
the tick at 20/50/100 us and compare the sleeps per second (wakes plus
ticks) and the items per pass with section 4's bound, lambda / (1 +
lambda L) passes and 1 + lambda L items.

| lab, constant spacing | lambda L | sleeps/s measured (bound) | items/pass measured (bound) | last-item max us |
|---|---|---|---|---|
| 10 k, L = 20..100 | 0.2..1 | 8775 (7464..4674): regime off | 1 (1.2..1.9) | 3..17 (W0 24) |
| 100 k, L = 20 | 2 | 39483 (33196) | 2 (3.0) | 53 (W0 227) |
| 100 k, L = 50 | 5 | 16384 (16641) | 4 (6.0) | 63 |
| 100 k, L = 100 | 10 | 8298 (9084) | 6 (10.9) | 124 |
| 1 M, L = 20 | 20 | 20114 (47475) | 7 (19.8) | 76 (W0 5) |
| 1 M, L = 50 | 50 | 8489 (19585) | 13 (48) | 128 |
| 1 M, L = 100 | 100 | 4333 (9895) | 24 (95) | 159 |

a07 reads the same within 10 % (100 k: 39654/16457/8329 sleeps/s;
1 M: 16465/8292/4132), the exponential process the same within 20 %.
Three things to read off.  At lambda L below one the regime never
enters, by design: the sleeps equal W0's and nothing is paid.  At
lambda L of 2 to 10 the sleep rate lands on the bound within 20 %
(39.5 k against 33.2 k, 16.4 k against 16.6 k, 8.3 k against 9.1 k)
and the items per pass at 55 to 67 % of the bound's: the pass count
is higher than the bound's because kwq's pass ends when the queue
empties and the worker re-checks before it sleeps, so a tick's worth
of items is served in two or three passes; the sleeps, which are what
cost a context switch, are at the bound.  At lambda L of 20 and
above the sleeps fall *below* the bound (20.1 k against 47.5 k) and
the last-item latency exceeds L (76 us at L = 20): the worker is 50 %
busy (1 M items at 500 ns each), the queue rarely stays empty for a
whole tick, and the drain after a tick lasts longer than L, so the
regime's episodes are drain-limited and the bound's premise (every
item served within L of arrival) no longer holds.  That is the
regime's saturation behaviour, the same as a saturated worker's in
section 7: fewer sleeps than any bound asks for, latency set by the
pass, not the tick.  SPEC 2.4's pass criteria (sleeps within 1.5x,
items within 0.7x, maxlat within L + pass + precision) hold for the
constant process at lambda L of 2 to 10 and fail on the items-per-
pass criterion at 1 M; the criterion was written for the bound's
passes and should be re-stated on sleeps, which the rows now carry.

**Sparse senders (SPEC 2.3).**  The ifpair rows with one and eight
senders at 20, 100 and 1000 us gaps:

| lab, senders x gap | lambda L at 50 us | sleeps/s W0 -> TK50 (bound) | items/pass (bound) | enqueue ns W0 -> TK20/50/100 | latency mean/max us at TK50 |
|---|---|---|---|---|---|
| 1 x 20 | 2.5 | 49107 -> 16865 (14262) | 2 (3.5) | 202 -> 201 / 39 / 24 | 28 / 248 |
| 1 x 100 | 0.5 | 8773 -> 8775: off | 1 (1.4) | 270 -> 256 / 263 / 266 | 1 / 15 |
| 1 x 1000 | 0.05 | 888 -> 888: off | 1 (1.04) | 265 -> 262 / 261 / 251 | 1 / 2 |
| 8 x 20 | 20 | 74545 -> 13593 (19041) | 15 (20.9) | 301 -> 100 / 54 / 46 | 24 / 158 |
| 8 x 100 | 4 | 31221 -> 17855 (15556) | 3 (4.5) | 385 -> 380 / 187 / 109 | 23 / 143 |
| 8 x 1000 | 0.4 | 5552 -> 5599: off | 1 (1.35) | 320 -> 323 / 311 / 280 | 3 / 58 |

a07: 8 x 20 reads 187059 -> 13665 sleeps/s (bound 19036), 16 items
per pass (20.7), enqueue 909 -> 300/210/186 ns; 1 x 20 reads 47203 ->
16908 (14238), 165 ns against 1089.  The prediction holds where the
regime engages: sleeps within 20 % of the bound (8 x 20 at L = 50
comes in 29 % *under* it, again the drain), items per pass at 67 to
72 % of the bound at L = 50 and 49 % at L = 100, and the sender's
enqueue cost falls to the append-without-doorbell cost of the `batch
16` row (lab 46 to 54 ns, a07 186 to 210 ns) once the doorbell is
skipped, which is the -80 to -90 % of section 7's W2 without W2's
callout.  The queue latency's mean is L/2 as a uniform arrival within
a tick predicts (24 to 28 us at L = 50, 47 to 55 at L = 100).  Below
lambda L = 1 the rows equal W0 to the third digit.

**Through the pair (compare.sh `sparse`, 16 and 64 flows at 1 k and 5 k
requests/s each).**  On a07 the per-worker rate stays below 2.5 k/s
(128 workers) and the regime never enters: wakes, RTT and the
interrupt counts equal W0 at every L.  On lab 64 flows at 5 k/s put
20 k/s on each of sixteen workers, lambda L = 1 at 50 us, and the
regime engages at 50 and 100 us: wakes -42 / -80 %, the machine's
system share -10 / -36 %, median RTT 7 -> 23 / 48 us (p99 12 -> 88 /
155).  That is the section 7.5 budget again: a sparse request-reply
flow pays the tick in full and gains CPU it was not short of.  SPEC
3.1's criterion (p50 within 2L + 2 us) holds at 50 us (23 <= 102) and
at 100 us; the "system share does not rise" clause holds everywhere.

### 10.3 A producer with a duty cycle (SPEC 2.1, 3.2)

The hypothesis: a dense burst costs one wake plus one tick per L, an
isolated item is left alone, a burst boundary costs one wake and at
most one empty tick.

**In the module (kwq_test `burst`, handler 500 ns per item).**
Cross-CPU bursts never enter the regime at any L: the producer's B
enqueues take B x 100 ns, the worker woken by the first serves them in
one or two passes, and the next burst comes after a gap of G > L.
Wakes per burst 1, ticks 0, latencies equal to W0's (first item 2 us,
last item 1 to 11 us) for every (B, G) from (1, 2 ms) to (64, 50 ms) on
all three machines.  Nothing to coalesce, nothing paid.  The same-CPU
producer (at priority 100 on the worker's CPU) is the other extreme:
at W0 every enqueue's doorbell preempts the producer, so a 64-item
burst is 64 wakes and 64 one-item passes (lab, a07), each item served
within 2 us; under the tick the first wake enters the regime (the
second item's doorbell comes within L), the burst's remainder waits
for the tick, and the burst costs 2 wakes, 2 ticks (one of them
empty, the exit) and 3 passes, with the last item served at L + 5 to
15 us (21-25 / 58 / 115 us at L = 20 / 50 / 100).  The predictions hold
to the letter (wakes 1 + the entry, ticks ceil(duration / L) + the
empty exit, last item within L + one pass), and the count of what is
paid is also exact: the same-CPU burst trades 62 preemptions of the
producer for one tick's latency.  Section 9's idle-class wait does the
same trade without the latency (one wake when the producer sleeps,
every item served within the burst's own duration), which is the
better tool for the same-CPU producer; the two mechanisms are
orthogonal and should be combined, not chosen between.

**Through the pair (`udp_burst`, nothing else running).**

| first / last reply RTT us, W0 -> TK20 / TK50 / TK100 | lab | a07 |
|---|---|---|
| (1, 2 ms): isolated requests | 8 -> 9 / 8 / 8 | 35 -> 35 / 35 / 36 |
| (16, 200 us) first | 8 -> 7 / 8 / 8 | 20 -> 19 / 17 / 19 |
| (16, 200 us) last | 15 -> 32 / 67 / 122 | 26 -> 38 / 72 / 129 |
| (16, 200 us) bursts/s | 17.6 k -> -7 / -18 / -32 % | 16.3 k -> -5 / -15 / -28 % |
| (64, 2 ms) first p99 | 69 -> 34 / 35 / 34 | 284 -> 205 / 181 / 173 |
| (64, 2 ms) last p50 | 40 -> 49 / 81 / 137 | 267 -> 182 / 228 / 326 |
| (64, 50 ms) first p99 | 67 -> 44 / 43 / 38 | 298 -> 208 / 189 / 174 |
| (64, 50 ms) last p50 | 34 -> 58 / 80 / 135 | 195 -> 188 / 232 / 333 |
| (64, 50 ms) wakes / ticks per burst | 9.3 / 0 -> 1.9 / 1.6..1.9 | 9.6 / 0 -> 1.9 / 1.5..1.9 |

The isolated request equals W0 within 1 us at every L on both
machines (SPEC 3.2's first criterion).  The 16-request burst every
200 us is the case the regime gets wrong: its sixteen requests arrive
over 30 to 60 us, the first wakes the worker, the second enters the
regime, and the burst's tail waits for the tick, so the last reply
costs L on top of W0's (lab 15 -> 67 / 122 us at 50 / 100), and since
the victim fires the next burst only after collecting, the burst rate
falls 15 to 32 %.  The 64-request bursts show both faces: the first
reply improves 30 to 50 % (lab p99 69 -> 34 us, a07 284 -> 173),
because sixty-three of the sender's enqueues skip the doorbell and the
sending loop finishes sooner, and the last reply pays L (lab 34 -> 80
/ 135 us at 50 / 100), with a07's slower sender (a 64-request loop
takes 150 to 250 us there) coming out ahead at 20 us (last p50 267 ->
182, p99 395 -> 209).  Wakes per burst fall from 9 to 2 and the ticks
replace them one for one.  Beside the bulk flow (`udp_burst_load`) the
isolated request also pays L (a07 34 -> 48 / 70 us), as udp_load did in
the tick battery: the bulk flow keeps the regime on and the request
is a doorbell skipped.

So the hypothesis holds in its own terms and the system-level finding
is about the entry rule: "two wakes within L" is satisfied by every
burst of two or more items, which makes a bursty producer with long
sleeps the regime's worst customer, paying a tick on every burst's
tail for a saving of one or two wakes per burst.  Section 4's
prescription for that producer was the opposite, a linger after the
first wake (serve, then wait a few microseconds before sleeping) and
no tick; the entry should at least demand density over a window
longer than one burst, say three consecutive sleeps shorter than L,
or an items-per-pass of 2 or more over the last episode.

### 10.4 The accounting modes under interruptions (SPEC 2.2, 3.4)

The claim under test: a worker switched out mid-pass is charged the
absence under wall time, so a well-behaved queue overruns and parks
for the system's behaviour; CPU-time charging ends that; `cpu` lets
the worker hold its CPU at the neighbours' expense while `split` does
not.  Two competitors make the interruptions.  The `burst` competitor
is a timeshare thread on W that wakes 1000 times a second and works
400 us: it can preempt the worker only while the worker is demoted by
the share limit (nothing timeshare preempts PI_NET), which the twoq
and P4d tests arrange, so it is the P4d interruption, a thread.  The
`intr` competitor spins 20 us in interrupt context every 100 us on W
(kwq_test's stressor), the interruption the priority invariant cannot
exclude.  All CH* stages run the tick at 50 us, so the comparison that
isolates the charge mode is against CHW, not W0.

| per 8 s run, lab / a07, burst competitor | CHW (wall) | CHC (cpu) | CHS (split, 10.6) | CHH (hybrid) |
|---|---|---|---|---|
| twoq q1 overruns | 230 / 404 | 0 / 0 | 0 / 0 | 0 / 0 |
| of which switched | 230 / 403 | 0 / 0 | 0 / 0 | 0 / 0 |
| twoq q1 parks | 232 / 413 | 0 / 0 | 0 / 0 | 0 / 0 |
| twoq q1 debts | 12.5 / 56.5 | 1.5 / 54 | 1.5 / 22 | 0 / 38.5 |
| twoq share1 | 0.501 / 0.510 | 0.497 / 0.501 | 0.50 / 0.498 | 0.502 / 0.504 |
| spin: spinner's CPU share | 0.516 / 0.512 | 0.358 / 0.408 | 0.514 / 0.512 | 0.513 / 0.511 |
| timer late p99 us | 518 / 308 | 541 / 358 | 516 / 360 | 517 / 352 |
| preempt: preemptions/s | 233 / 228 | 98.5 / 264 | 314 / 240 | 310 / 256 |
| udp_load RTT p50 / p99 us (lab) | 12.5 / 62.5 | 13 / 56 | 12 / 83.5 | 16.5 / 72.5 |
| udp_load prio_violations (lab) | 0.5 | 3.5 | 6.5 | 22.5 |

The split column is the re-run of 10.6 on the same boot as its own
wall stage, which read 241 / 420 overruns, all switched, debts 17 /
37.5, spinner 0.512 / 0.511, timer 516 / 298 us, preemptions 296 /
238 per second; the other columns are the second battery's.

**Wall time punishes exactly as claimed.**  Under the burst competitor
the well-behaved twoq queue overruns 230 times on lab and 404 on a07
per run, every single one a switched pass (`overruns_switched` equals
`overruns`), and parks as often: the queue is charged the competitor's
400 us and loses its turn for it.  W0 reads the same (224 / 420) and
could not say why; the new counter says all of it is the system's.
Every CPU-time mode brings the figure to zero on both hardware
machines, `split` the debts too (lab 12.5 -> 1.5, a07 56.5 -> 9).  On
the guest the overruns fall 84 to 90 % rather than to zero (132 ->
21 / 16 / 13) and the remainder is unswitched: a bhyve vCPU's steal
time is CPU time to the guest's thread clock, so a virtual machine's
"interruptions" are invisible to both clocks.  **`cpu` costs the
neighbours**, as predicted: the spinner's share falls from 0.51 to
0.36 (lab) and 0.41 (a07), the pinned timer's lateness grows 5 / 16 %,
the worker's preemptions halve on lab; the share limit's period runs
on the worker's own clock, so a period lasts longer in wall time than
it should and the worker gets more of the CPU than its permille.
**`split` keeps the fairness of wall time** once its two clocks were
kept apart (10.6): the spinner's share 0.514 against wall's 0.512,
the timer's lateness and the preemption rate equal to wall's, the
UDP flow's median 33 % better and its p99 19 % worse on lab, on a07 the UDP median -6 % and the p99 equal, the pinned timer's p99 +21 % under burst and -10 % under intr (its own wall column moved from 298 to 320 us between the two runs).  **`hybrid`** matches `split` on the overruns and
wall on fairness, but it is the mode with the most priority
violations (lab 22.5 against 6.5 for split and 0.5 for wall) and the
worst UDP median (16.5 us): charging CPU time only after the fact
means the pass that was switched out had already run on the wall
budget, so it gives up nothing and the next pass starts demoted more
often.  Nothing recommends it over `split`.

**The interrupt competitor is invisible to both clocks.**  Under
`intr` no mode overruns (0 in every column on every machine), the
debts are unchanged, `overruns_switched` stays 0 because an interrupt
is not a context switch, and the two clocks agree: `td_runtime`
includes the interrupt time spent on the thread's CPU.  The 20 us
stolen per 100 us are below what a 200 us quantum notices, so nothing
is punished either way, and the stressor's cost shows only in latency
(udp_load p50 on a07 22 -> 38.5 us at CHW under intr against 40.5
under burst: the tick, not the interrupt).  The second half of crest's
rule, "measure the impact of wall clock against NET thread runtime
under such interruptions", therefore has an answer in two parts: for
thread preemption the two clocks differ by exactly the preemption
(230 to 400 parks per run against none) and CPU time is the right
charge; for interrupt context they do not differ at all, and if that
time is ever to be excluded it must come from the CPU's interrupt
accounting (kern.cp_time's intr column per CPU, or a PMC), not from
the thread.  `twoq`'s throughput under `split` on lab (+37 % in the
second battery) is the broken run's number, see 10.6; the fixed
re-run reads -0 %.

### 10.5 The priority assertion

`prio_violations` is nonzero in every tick stage on the hardware
machines: lab 0.5 to 35 per run, a07 17 to 206 (the tick battery's
tput1 at TK20: 206, udp_load 80), and zero only where the run never
demoted the worker.  Every one of them is the base tree's share limit:
the kwq branch demotes the thread when it runs over its allowance and
restores it at the next period or hand-over, so a pass can begin with
`td_priority` above the class priority, which is precisely what crest's
first rule forbids and what the P4d pass-level branch
(kwq-p4d-passlevel, SCHED.md S6.3) fixes by restoring the priority
before every pass.  The assertion therefore measured the known gap
rather than a new one, which is the right outcome for a tripwire, and
it fixes the merge order: the pass-level branch before any of this,
or the hard build panics on the first demoted pass.  W0 cannot report
the counter; T has no such thread.

### 10.6 What the tests found in the code

Six defects, all found by the tests and fixed on the branch before the
batteries that stand: the tick's exit was judged on the entering round
(every episode ended at once); ping-pong re-entered the regime on every
second wake (the exponential hold-off); the hold-off was applied after
coalescing episodes too (5685f2e); `split` charged the thread's CPU
time to the share limit at pass end (78143f6) and, after that, still
handed the limit the CPU clock at pass begin (b323af4), so the period
rolled over at every pass end and the limit never engaged (the second
battery's CHS column: spinner share 0.03 on lab and 0.26 on a07, 22 ms
timer lateness, zero preemptions, twoq +37 %, the numbers in
p4e_charge2_* under CHS are that bug; p4e_charge3_* are the fixed mode
against CHW on the same boot); the victim's burst was a dependent
chain (a31f251); the intr competitor wrote `intr_secs` before
`intr_cpu` (01d9b02).  And one in the base kernel: unloading the pair
module while a vnet jail is still dying deadlocks `vnet_destroy`
(holds ifnet_detach_sxlock, wants the vnet sysinit lock) against
`vnet_deregister_sysuninit` (the reverse); the guest sat in it for
thirty minutes under the intr battery, was panicked for a dump
(vmcore.0 there) and compare.sh now waits for `jls -d` to forget the
stage's jails (6d09c21).  The wait is a mitigation, not a fix: on the
guest the cap of 20 s was hit after every `udp_load` under the intr
competitor (iperf3's TIME_WAIT connections keep the jail alive for
their 2MSL) and the unload then went through, since the race is only
with `vnet_destroy` itself, which had not started; the lock order
belongs to the base kernel.

### 10.7 Does the evidence support the hypothesis?

Taken as three claims:

1. *A tick regime cuts the sleep rate to the lower bound's for dense
   traffic and costs nothing for sparse traffic.*  Supported where
   lambda L >= 2 (sleeps within 20 % of the bound on both hardware
   machines, the enqueue cost at the append's), supported by absence
   where lambda L < 1 (every row equals W0), and bounded above by the
   drain at saturation.  The price is the one section 7.5 named, L on
   every item that arrives while the regime is on, and the regime is
   on for one wrong customer, the short burst with a long sleep, whose
   tail pays L for a saving of one wake.  The entry rule, not the
   tick, is what to change.
2. *Wall-time accounting punishes well-written handlers for
   interruptions they do not control, and a CPU-time charge does not.*
   Supported for the interruption that exists on the hardware machines,
   preemption of a demoted worker by a thread: 230 to 400 switched
   overruns and parks per run under wall, none under any CPU-time
   charge.  Not testable for interrupt context by either clock, which
   both include it.  Of the three alternatives `split` is the one the
   fairness columns allow: zero punishment, the share limit on wall
   time, the spinner, the timer and the preemption rate equal to
   wall's.  `cpu` fails fairness, `hybrid` adds nothing.
3. *NET workers must be at the class priority before every handler.*
   The tripwire counts the base tree's demoted passes and nothing
   else; the pass-level branch is the fix and has to merge first.

What this suggests for the recommendation of 9.3: `split` charging as
the NET default once the pass-level invariant is in (it changes
nothing on an uninterrupted worker and removes the punishment on an
interrupted one); the tick regime as a class opt-in at 20 to 50 us
for dense cross-CPU producers, with its entry made stricter before
anyone turns it on; the idle-class wait of section 9 for the same-CPU
producer; and W2's deadline for the sparse cross-CPU sender, which the
tick cannot help below lambda L = 1 by construction.  The guest's
re-run after the deadlock (p4e_charge4_*) reads the same way in
direction: the twoq overruns fall 80 % under the fixed split (132 ->
27, the switched ones 110 -> 2.5, parks 169 -> 40), the spinner's
share and the timer's lateness equal wall's, and under the interrupt
competitor every mode overruns alike (40 to 52, none switched), the
debug kernel's own slowness and the hypervisor's steal time being what
is charged there.
