# kwq: a shared deferred-work service for the FreeBSD kernel

**Design rationale and measurements.**  Written 2026-10-01 for
`kwq.ko` as built.  This paper explains why kwq exists, why it is shaped
the way it is, what was taken from other systems and what was
deliberately rejected, and which measurements decided the open
questions.  The normative specifications are `../KWQ.md` (objects and
contracts) and `../SCHED.md` (the scheduler); the survey behind section 3
is `../../POOLS.md` (cross-OS) and `../../DISPATCH.md` (libdispatch);
the raw measurement logs are in `../../samples/` and the running
analysis in `../../NOTES.md`.

Contents: 1 the problem; 2 requirements and non-goals; 3 prior art, with
netisr(9) compared point by point in 3.2; 4 the object model; 5 the
scheduler; 6 the cost model, measured; 7 alternatives rejected or
deferred; 8 validation; 9 open questions.

---

## 1. The problem

FreeBSD has excellent deferred-work *mechanisms* and no deferred-work
*policy*.  `taskqueue(9)`, ithreads, netisr workstreams, `epoch(9)`
callbacks and the per-CPU callout wheels are all sound; what is missing
is a layer above them that arbitrates between the subsystems that use
them.  Every subsystem that needs per-CPU deferred work therefore
builds its own pool, and on a large machine there are many: if_pair,
if_epair, if_wg, GELI and iflib each create on the order of one thread
per CPU.

Three consequences were measured on this project's hardware before a
line of kwq was written.

**Starvation of everything below.**  Each pool runs at `PI_NET` or
`PI_SOFT`, because in isolation that is what its work is worth.  On a
128-core Ampere Altra, about 35 saturated if_pair workers left the
per-CPU callout threads roughly 30 % of one CPU: TCP timers stalled
machine-wide, ssh froze, and a benchmark aborted on its own control
connection (`../../samples/starve.log`).  The remedy, a bounded
demote-yield after each burst, is a *policy* decision.  Implemented
five times in five drivers it will be implemented five different ways,
and four of them will be wrong.

**No arbitration between pools.**  Two private pools on one CPU at the
same priority interleave by whatever ULE decides.  Neither can be told
to take less, because neither has a unit of account; "fair" has no
referent.

**No common observability.**  Each pool counts what its author thought
to count.  There is no answer to "which deferred-work client on CPU 7 is
responsible for this latency" that spans subsystems, and no way to build
one without touching every subsystem.

A fourth consequence is structural rather than measurable: the same
hundred lines of doorbell state machine, bounded queue, batch drain and
teardown protocol are written once per client, and each copy has its own
bugs.  The lost-wakeup protocol in particular is subtle enough that
if_pair's version carries a twenty-line comment explaining why both
halves must happen under one lock hold.

## 2. Requirements and non-goals

Requirements, in the order that constrained the design:

1. **A unit of account.**  Without one there is no fairness.  It must be
   CPU time, not items: a 64 KB TSO chain and a 64-byte ACK are one item
   each and differ by two orders of magnitude in cost.
2. **Bounded deference downward.**  A saturated worker must give the CPU
   to softclock, epoch callbacks, lower classes and interactive userland
   within a bounded interval, without a priority inversion and without a
   lower-priority fallback thread for overrun work.
3. **No allocation on the data path, and a bound.**  Items are the
   client's objects with an embedded link; admission is the only place
   work is refused, and it refuses by returning the item.
4. **Order where the client needs it.**  A flow's items must stay in
   order, cheaply, without a global lock.
5. **One observability surface for every client**, always on, with a
   measurable cost.
6. **A client's conversion must be a simplification.**  If porting a
   driver to kwq adds code, the service has failed.
7. **No new failure modes in teardown.**  Everything on the teardown
   path is infallible so that no client needs an error path there.

Non-goals, each of which is a thing some other system does:

- **No synchronous submission.**  No `dispatch_sync`, no "run it on my
  thread", no barrier.  The queue is a reentrancy break; a synchronous
  path through it reintroduces exactly the recursion it exists to
  prevent (section 4.4).
- **No client-chosen priority.**  Classes, not priorities.  A knob every
  client can turn to the top is a knob every client turns to the top.
- **No queue hierarchy.**  Flat class by CPU.  Hierarchy is what forces
  libdispatch's priority-override and stealer machinery; with one class
  per thread and DRR within it, there is nothing to invert.
- **No automatic controller in the first version.**  Fixed quanta plus
  the counters a controller would need, so that one can later be
  justified by data instead of added speculatively.
- **Not a third framework.**  kwq is an extension of what gtaskqueue
  and netisr's workstreams already do, and it ships with its first
  converted clients or not at all.

## 3. Prior art, and what was taken from it

### 3.1 Other systems

The survey covered Linux cmwq and softirqs, Solaris and illumos taskq
and squeue, NetBSD `softint(9)` and `threadpool(9)`, OpenBSD `taskq(9)`,
Windows DPCs and work items, DragonFly's per-CPU protocol threads,
Apple's libdispatch, and FreeBSD's own history from `taskqueue(9)` in
2000 through the 2009 netisr rewrite to `epoch(9)`.  Three of these
shaped kwq directly.

**illumos squeues gave the shape.**  One flow, one CPU-bound queue, one
drainer at a time, a time-bounded inline drain and then hand-off to the
worker.  `squeue_drain_ms = 20` is a time quantum in all but name, and
reading it as one is the germ of kwq's accounting.  kwq keeps the
binding of a flow to a CPU for its lifetime and the single-drainer
property; it does not keep the inline drain, because FreeBSD's
equivalent, processing a packet in the sender's call chain, is the
documented recursion panic.

**fq_codel and CAKE gave the arrival policy.**  A queue that has just
become non-empty must be served before the backlogged ones, or a sparse
flow waits a whole round behind the heavy ones.  fq_codel's new/old flow
split does exactly this, and its rule that a *new* flow which empties
goes to the tail of the old list is the seed of kwq's grace rule.  CAKE's
decaying set is the same repair against the same abuse.

**Deficit round robin (Shreedhar and Varghese, 1995) gave the
mechanism**, with one adaptation.  Classic DRR sends a packet only if
the deficit covers its length, which works because a packet's length is
known before it is sent.  An item's cost is not known before it runs, so
kwq inverts the test: run at least one item, then charge what it cost and
carry the difference.  Section 5.3 is that change and its consequences.

**libdispatch gave several mechanisms and one warning.**  Taken: the
closed class model in a single priority word; the intrusive queue with
the idle-to-queued transition as the doorbell; thread-request
coalescing, so one outstanding wake rather than one per item;
inactive-then-activate creation; and its labels and context pointer as
first-class observability, which in kwq are the queue name and the
handler's context argument.  The warning is what libdispatch does *not*
have: no time budget and no inter-queue fairness anywhere in its public
contract, only per-queue FIFO and strict priority between bands (kwq:
classes), because its workers are preemptible and the kernel scheduler
arbitrates.  As the study puts it,
the fairness burden sits with whoever owns the highest priority in the
system; libdispatch never does, and a kernel pool always does.  So the
quanta and the demote-yield have no libdispatch counterpart and cannot
be omitted.  Rejected outright: `dispatch_sync` and barriers, target
queues and the override machinery they force, semaphores and blocking
waits, and overcommit as a liveness patch for blocking work.

**cmwq provided the class split** that Linux converged on after its own
proliferation (BH, per-CPU bound, unbound stealable, and a rescuer,
which kwq would call a reserved worker), and one anti-pattern:
`WQ_HIGHPRI`, a client-chosen priority.  Its spawn-on-block concurrency
management (kwq: worker replacement) is accepted for a future BLOCKING
class only, where blocking is legitimate.

**Windows contributed the idempotent-requeue pattern**, "queue the work
item only if the task list was previously empty", which is kwq's
doorbell in all but the precise predicate: kwq rings on the
idle-to-queued transition, so a list that is empty because its pass is
still running is not rung again.  Windows also supplies the
counter-example of a fixed shared pool whose saturation can deadlock the
system.

The survey also catalogued the failure modes that the requirements in
section 2 are written against: pool proliferation (early Linux, FreeBSD
today), deadlock in fixed shared pools (Windows, Linux's system
workqueue, illumos's "do not call `taskq_wait()` from a task"), thread
explosion (illumos dynamic taskqs, libdispatch overcommit), invisible
work with unmanageable latency (Linux softirqs and tasklets), priority
as a tragedy of the commons (`WQ_HIGHPRI`, Solaris `pri`, `PI_NET` for
every FreeBSD driver), unbounded queues, and infrastructure shipped
without clients (RSS, netisr's per-CPU mode, threaded DPCs).  The last
is why kwq's plan requires two converted clients before graduation.

### 3.2 The nearest relative: netisr(9)

`netisr(9)` deserves its own treatment, because it is the closest thing
FreeBSD already has to kwq and because the first question a reviewer
asks is why the answer is not simply to finish it.  Read for this
section: `sys/net/netisr.c` (1562 lines), `netisr.h`,
`netisr_internal.h` and the IP and IPv6 registrations, in a 15.x tree of
March 2026.

**netisr has the shape and stops short of the policy.**  The 2009
rewrite already provides per-CPU workstreams, intrusive queues linked
through `m_nextpkt`, a bound per protocol per CPU, batched dequeue, and
a flow-to-CPU mapping with three ordering policies.  That is most of
kwq's object model, arrived at independently from the same pressures,
and it is why kwq is described above as an extension of netisr's
workstreams rather than a competitor.  What is missing is a unit of
account, a bound on how long one drain may hold a CPU, isolation between
the clients sharing a workstream, and any observability denominated in
time.

| | netisr | kwq |
|---|---|---|
| unit of account | none; nothing measures what a protocol consumed | the worker's own CPU time, charged per pass |
| fairness between clients on a CPU | none; service order within a pass is the protocol number, `prot = ffs(bits) - 1` (`netisr.c:971`) | deficit round robin with weights, carry, parking |
| bound per invocation | none; `swi_net()` loops `while (pendingbits != 0)` (`netisr.c:969`) and the bit is set on every enqueue even while the worker runs (`netisr.c:1009`), so a fed protocol is drained again in the same call | one quantum per pass, a tick guard spanning passes, a demote-yield at round end, an optional CPU-share cap |
| worst case in one drain | twice the limit per protocol, by the stack-local swap (`netisr.c:901`), across 16 slots: 8192 packets at the default limit of 256, 327,680 at `net.isr.maxqlimit` | one quantum, plus the overshoot of a single item |
| lock granularity | one mutex per workstream, covering every protocol's queue on that CPU (`netisr.c:253`, `netisr_internal.h:110`) | one lock per (queue, CPU); clients never contend with each other |
| default concurrency | `net.isr.maxthreads` is 1 and `RDTUN`, so one workstream, and `net.isr.bindthreads` is 0, so it is not even pinned | one worker per CPU always, each binding itself |
| admission failure | the packet is freed and `nw_qdrops` incremented (`netisr.c:1021`) | the item is returned to the caller, which decides to drop, count or push back |
| depth control | `maxqlimit` and `defaultqlimit` are `RDTUN`; a runtime change needs the protocol to call `netisr_setqlimit()`, which IP wraps in a sysctl of its own (`ip_input.c:237`) | the per-queue `limit` is writable at run time |
| placement | chosen per packet by policy, with fallbacks: flow falls back to source ordering (`netisr.c:857`), and CPU policy to `curcpu` under hybrid dispatch (`netisr.c:838`) or to source ordering otherwise (`netisr.c:842`) | the client passes the CPU; the only remapping is the load-time table that keeps a hash on a present CPU |
| observability | packet counts, a watermark and a DDB command; no time, no latency, and zero SDT probes in `netisr.c` | 19 probes with a translator, queueing latency, CPU time per queue, overruns, parking, boosts, grace, clock glitches |
| extensibility | a compile-time table of 16 protocol slots with numbers fixed in a public header, 8 assigned | any number of named queues, created at run time by the client |

Three entries deserve a sentence more than a table row gives them.

**The priority story is not the one section 1 tells about private
pools.**  A netisr SWI runs at `PI_SOFT`, because `PI_SWI(x)` is defined
as `PI_SOFT` (`sys/priority.h:105`), which makes it softclock's peer
rather than its superior; ULE then shares the CPU between them, so an
unbounded drain degrades timers instead of starving them outright.  The
deferred path is also not the common one: the default dispatch policy is
`NETISR_DISPATCH_DIRECT` (`netisr.c:151` and `netisr.c:1146`), so most
packets are processed inline in the caller, typically a NIC interrupt
thread at `PI_NET`.  The
budget question is therefore moved rather than answered, and it lands in
a context with no budget either.

**The fixed taxonomy is the structural blocker, not a limitation of
degree.**  netisr accounts per protocol, and a protocol number is a
constant in a public header that the comment at `netisr.h:47` says is
"encoded in monitoring applications and kernel modules".  There is no
way to express a queue per interface, per provider or per tunnel, which
is precisely what `if_pair`, `if_epair`, `if_wg` and GELI each need.
That is why all four built private pools rather than registering with
netisr, and it is the one gap that cannot be closed by adding a
scheduler to netisr as it stands.

**What netisr does that kwq deliberately does not.**  Direct and hybrid
dispatch process a packet in the caller's context with no queue and no
context switch, which is cheaper than any queue wherever the reentrancy
hazard does not apply; kwq refuses that path outright after the inpcb
recursion panic (section 4.4).  The `m2cpuid` and `m2flow` hooks let a
protocol decide placement with the mbuf in hand, where a kwq client must
decide before it calls.  netisr integrates with `DEVICE_POLLING`.  It is
in the base system with a stable interface and `netstat -Q`.  And it
carries IP input on every FreeBSD machine in service, which no part of
kwq yet does.

**Which is why netisr is the second conversion, not the competitor.**
Every gap in the table is policy, accounting or granularity, and none of
them requires netisr's callers to change: a protocol's per-workstream
queue becomes a NET-class kwq queue, the handler keeps its signature,
and the protocol gains a budget, fairness against the other NET clients
on its CPU, and the shared observability surface.  The conversion
retires `net.isr.maxthreads=1`, since the worker-per-CPU question stops
being netisr's to answer.  It is item 2 of the migration plan, after
`if_pair`, and it is the one that would put kwq on the path every packet
takes.

## 4. The object model

### 4.1 Why per-CPU lists, and why the CPU is the ordering domain

Every surveyed system that handles network work at scale arrives at the
same place: work is bound to a CPU, and the binding is derived from a
flow hash.  kwq makes that the primitive rather than a policy on top of
one.  A queue is a name and an accounting unit; its *lists* are per CPU,
and the client chooses the CPU per item.

The consequences are worth stating plainly because they are the whole
design:

- Ordering is promised within a (queue, CPU) list and nowhere else.
  Clients that need order hash to a CPU; clients that do not get
  parallelism for free.
- There is no global queue and therefore no global lock.  The contention
  a shared-pool design spends its ingenuity on (libdispatch's mediator
  and its thread-count signal) does not exist here.  As the libdispatch
  study concludes, per-CPU queues dissolve it entirely, which is the
  strongest argument for per-CPU rather than per-class shared queues.
- The bound is per list, so a hash skew shows up as `ENOBUFS` on one CPU
  with the others idle: a diagnosable steering symptom rather than a
  mysterious global slowdown.
- Stealing, when it arrives for BULK, is a per-class property, not a
  per-item one, and NET is never stolen: a flow's items never wait
  behind a cross-CPU cache miss they did not cause.

### 4.2 Why the handler receives batches

One lock hold per pass instead of per item; one network epoch entry per
pass; one vnet set per pass; one `if_ref()` per pass.  The batch is
whatever accumulated during the previous pass, which is Mogul and
Ramakrishnan's and LRP's "batch what has arrived" and the shape of
NAPI, with no added latency when the system is idle, because the first
enqueue into an idle list wakes the worker immediately.

Measurement decided the corollary: the client should *not* stage items
to build batches of its own (section 7.2), but should hand over a list
it already holds, because the difference between one item and four per
call is 13-fold (section 6.1).

### 4.3 Why items are the client's objects

kwq allocates nothing on the data path, so there is no `M_NOWAIT`
failure to handle in any context, no memory pressure interaction, and no
per-item allocator traffic.  For packets the link is free: an mbuf's
`m_nextpkt` is layout-identical to a `kwq_item`, checked by `CTASSERT`.
The cost is a contract the client must honour, that an item is queued at
most once, and the proof obligation is one macro call plus one assertion
inside kwq.

### 4.4 Why there is no synchronous submission

if_pair's own history supplies the argument.  Delivering a packet inline
rather than through the queue, for TCP between two local sockets, runs
the peer's input path nested inside the sender's call chain, where
`tcp_input()` takes the inpcb lock the suspended `tcp_output()` already
holds.  Under INVARIANTS that is "recursed on non-recursive mutex"; in
production the mutex silently recurses and the nested processing mutates
the connection under the outer call, whose stale send-buffer snapshot
later walks off the end of an mbuf chain and panics.  The queue is the
reentrancy break.  A `dispatch_sync` equivalent would be that panic
offered as an API.

### 4.5 Why notifiers share the item list

The first design kept a second per-CPU list for notifiers and delivered
them first, one per call, because the worker had to flip each one from
pending to idle under the lock before the handler saw it, and could not
find notifiers inside a swapped-out item list without walking it.  The
cost was non-FIFO delivery, unbudgeted signals, and the starvation case
the scheduler specification records as case 4: measured on the two-list
code, every item enqueued before a signal was delivered after it, 64
signals of 50 us ran in one pass against a 200 us quantum, and a
self-re-arming signal held an item flood to one item per pass, 0.6 % of
its solo rate on the 128-core machine.

The replacement puts a notifier on the item list at its arrival position
and marks it with the low bit of its *own* outgoing link, which is free
because every element is pointer-aligned and which no mbuf ever carries,
since an mbuf is never a notifier.  The handler's `KWQ_ITEM_NEXT(q, it)`
fetches an element's successor before the handler acts on it, and for a
tagged element that fetch is the flip, so the lost-wakeup argument is
unchanged and the flip costs one bit test per element.  Notifiers are
thereby budgeted and requeueable like items, and the per-CPU structure
lost a list head.  The one new obligation, that a handler must walk its
list, is met by every item handler already; for the notifier-only client
that ignores its list, the worker settles an unwalked head or tail
itself, telling "never consumed" from "consumed and re-armed" by a
generation kept in the spare bits of the state word.  The eight `mix`
test cases that failed on the old design pass on this one, with the
item flood beside a re-arming signal back above a tenth of its solo
rate and the 64 signals spread over passes by the budget.

## 5. The scheduler

### 5.1 CPU time as the unit

Each pass is bracketed by two readings of the worker's own CPU time, so
time spent preempted or asleep is not charged to the client.  The
difference is subtracted from the queue's deficit.  Wall time is
recorded separately, for latency diagnosis.

The alternative, counting items, was tried in production: if_pair's
`net.link.pair.batch` counted packets, and the count had to stretch with
the MTU, since 64 packets of 20 us each overran a 1 ms tick at 64 KB
MTU.  A count cannot express "this client may have 200 microseconds";
time can, and the same number is meaningful for a packet, a crypto block
and a checksum.

### 5.2 Deficit round robin per (class, CPU)

Each round, each active queue's deficit grows by `quantum x weight`,
capped at two quanta so that credit cannot be hoarded across quiet
rounds.  The worker swaps the queue's list out, runs the handler, and
charges the pass.  A queue with work left goes to the ring tail; an
empty one goes idle with its deficit reset to zero, which is DRR's usual
rule.

With one active queue this degenerates to "drain until empty or
quantum", which is what a private pool does today, at no extra cost.
That property matters: the fairness machinery costs nothing until there
is someone to be fair to.

### 5.3 The progress rule, and why overshoot is cheap

Because an item's cost is unknown beforehand, a pass always runs at
least one item and the overshoot is carried.  A pass that ends 4 us over
its 200 us budget starts the next pass with 196 us; over two rounds the
queue has received and spent exactly 400 us.  Parking, the penalty, is
reserved for a pass that exceeds its budget by more than a *whole*
quantum, which a handler checking `kwq_budget_left()` between items
cannot do.  The gap between "carried" and "parked" is deliberately wide
so that cooperative handlers never meet the penalty and budget-ignoring
ones cannot avoid it.  The penalty itself is bounded at
`penalty_rounds` quanta of debt, 32 by default.

### 5.4 The new list, the boost, and the grace rule

A queue that goes from empty to non-empty is placed on the worker's
*new* list and served with a fresh quantum, in alternation with ring
passes, before joining the ring.  Without this a sparse queue waits a
whole round behind the backlogged ones.

The boost is also an obvious thing to abuse: a producer that pauses
briefly between bursts, which every TCP sender does, would collect a
fresh quantum per burst at the expense of continuously backlogged
queues.  Xen's credit scheduler and Linux's CFS sleeper fairness were
both gamed exactly this way, and EEVDF's repair, keeping lag across
sleeps, is the same idea as kwq's: a queue that goes idle *from the
ring* records the round number, and a re-arrival within the grace window
rejoins the ring rather than the new list.  A queue that goes idle from
the new list, the genuinely light one the mechanism exists for, is
eligible again immediately.

**Rounds, not time, are the clock.**  The round counter advances only
while the worker has work.  This is not an approximation of a timer: the
harm a boost can do is measured in the service other queues lose, and
they are only served during rounds.  While the worker sleeps nobody is
being served, so a boost given or withheld then has nothing to take
from.

### 5.5 The debt rule

A queue that goes idle owing time must not be able to clear the debt by
going idle.  The deficit reset on empty is therefore conditional: a
negative deficit survives the idle period, and the next doorbell goes to
the ring rather than the new list.  This closed the one hole the
simulator found by construction: without it, a handler that returned its
items at pass end and immediately refilled the queue collected a boost
per pass and took 41 times its share instead of 1.27.

### 5.6 The estimated budget check

`kwq_budget_left()` is called per item by design, so its cost is on the
hot path.  On arm64 a clock read carries an instruction barrier and
costs 60 to 100 cycles.  The implementation therefore reads the clock at
most every `budget_check_every` calls (default 8), extrapolates in
between from a learned average cost per call, and reads early whenever
the estimate comes within a tenth of a quantum of the end.  Measured
effect: 12 % of calls read the clock, and the batched per-item cost fell
from 40 to 21 ns.  The property the handler needs, "is there time for
another item", is preserved; the number it reads is an estimate, and the
documentation says so.

### 5.7 The yield, the tick guard and the cap

At the end of every round in which something else on the CPU is
runnable, and whenever a `hz` tick boundary has passed since the worker
last gave up the CPU, the worker performs a demote-yield:
`kern_yield()` at the class's `yield_prio` (default
`PUSER`), then restore the class priority.  The restore is necessary
because `kern_yield()` rewrites `td_base_pri`, the anchor every priority
restoration mechanism unwinds to, and a pure kernel thread has no
`userret` to restore it; without the re-assert the first yield would
demote the worker for its lifetime.

The tick guard spans passes rather than being anchored at pass entry, so
a chain of individually-small passes cannot add up to unbounded
occupancy.  Userland on a CPU the worker saturates, which a one-decision
yield does not help, is served by the share limit instead: a worker may
spend `cap_permille`/1000 of each `cap_period_us` in passes while
something else is runnable on its CPU; beyond that it hands the CPU over
between passes as an idle-priority thread, which ULE runs only when
nothing else wants the CPU, and cuts a running pass short through the
budget, and a callout at the period's end raises it back while it waits
on the run queue, so the time given away is bounded by the period rather
than by a ULE slice.  An equal share by default.  The invariant behind
it, decided after measuring the alternative: a pass runs at the class
priority and is preempted by nothing but interrupt threads, so a handler
holds its locks exactly as netisr's and the drivers' threads always did
and never depends on the kernel's partial priority propagation.

Between classes the arbitration is strict priority by thread priority,
plus a per-CPU hand-back flag so that a BULK round ends as soon as a
yielded NET worker wants its CPU back, rather than waiting out the BULK
quantum.

### 5.8 The clock glitch guard

On the project's Ampere Altra, `cpu_ticks()` is not monotonic.  The
kernel builds it from a 32-bit timecounter read and takes any decrease
for a wrap, in a critical section that leaves interrupts enabled, while
the clock interrupt's `statclock()` reads the same ticker; a thread
reading it with interrupts enabled can therefore find the stored last
value newer than its own and add 2^32 ticks, 171.8 seconds, to its
CPU's base for good.  The counter itself never steps back, a probe
showed in 66 million reads; the first explanation, backwards reads,
was wrong.  Any scheduler built on CPU-time deltas has to survive that
kind of thing, and kwq now also avoids the exposed read.  kwq treats a negative delta or
one over one second as a glitch, charges the pass one quantum, and
counts it in `glitches`, which an operator can read as "this machine's
CPU-time accounting is broken".  The kernel-side issue is reported
separately; kwq does not pretend to fix it.

## 6. The cost model, measured

All figures from a 128-core Ampere Altra (Neoverse-N1 at 3.0 GHz,
FreeBSD 15.1 GENERIC) unless stated.  Raw logs in `../../samples/`,
analysis in `../SCHED.md` S15.7 and `../../NOTES.md`.

### 6.1 Per item

| per item, per side | 1 item per call | 4 per call | 16 per call | taskqueue baseline |
|---|---|---|---|---|
| items/s, one producer and one worker | 1.90 M | 24.7 M | 24.5 M | 2.26 M |
| items/s after the estimated budget check was added, same day | 1.8 to 2.2 M, bimodal | | 47.4 M, 21 ns per item | |
| cycles, worker / producer | 1352 / 1352 | 104 / 104 | 105 / 94 | 1138 / 1138 |
| instructions, worker / producer | 790 / 1061 | 177 / 118 | 178 / 137 | 560 / 923 |
| backend stall cycles | 818 / 920 | 36 / 61 | 36 / 36 | 636 / 779 |
| L1D refills | 6.8 / 5.7 | 1.0 / 1.4 | 1.0 / 0.8 | 23.5 / 7.6 |
| last-level cache read misses | 6.8 / 6.5 | 1.1 / 1.6 | 1.0 / 1.5 | 16.0 / 8.4 |

The counter rows below the first two are from the earlier build, the one
whose 16-per-call throughput was 24.5 M: at that point the harness's
item pool, not kwq, was the limit, which is why 16 per call gained
nothing over 4.  Removing most of the clock reads (section 5.6) is what
took the same configuration to 47.4 M, and that is the figure the rest
of this paper uses as the batched reference.

The single-item column is the mutex hand-off, not kwq's bookkeeping: 70 %
of all cycles are backend stalls, every L1 refill is a cache-line
transfer between the two cores, and a quarter of the enqueues find the
lock held, where the adaptive-mutex backoff turns a 100 ns hold into a
1 us wait.  A profile of the single-item case puts 48 % of the worker's
cycles in mutex lock and unlock, 16 % in clock reads, 13 % in kwq's own
code, 7 % in the spin lock and 4 % in epoch enter and exit; on the
producer, 71 % in the mutex and its backoff.

Three conclusions were drawn and acted on.  Batching at the producer
where a batch exists is worth 13-fold.  kwq's own bookkeeping is under
200 cycles, so a per-item producer on this class of core should expect
about 1300 cycles per item and the lock, not the service, is the cost.
And clock reads were worth attacking: five per pass became three, and
the estimated budget check (section 5.6) removed most of the rest.

A thread switch, for comparison, is 257 ns on this machine, which is
why a pass is a function call rather than a thread hand-off: at 16
items per call kwq moves an item for a twelfth of the cost of waking
something up to do it.

### 6.2 Scaling and fan-in

Independent producer and consumer pairs scale linearly:

| pairs | 16 items per call, aggregate | per pair |
|---|---|---|
| 1 | 47.4 M items/s | 47.4 M |
| 2 | 94.3 M | 47.0 to 47.3 M |
| 4 | 189 M | 47.0 to 47.4 M |
| 8 | 375 M | 46.7 to 47.2 M |
| 16 | 744 M | 45.9 to 46.7 M |
| 32 | 1.45 G | 44.6 to 45.8 M |
| 48 | 2.13 G | 42.5 to 45.3 M |
| 63 | 2.55 G | 34.5 to 42.9 M |

Sixty-three pairs is 126 of the machine's 128 CPUs, and the per-pair
rate at that point is 73 % of a lone pair's: the shared state is one
cache line per (queue, CPU), so what degrades is the memory system, not
a lock.

Fan-in into one (queue, CPU) list does not:

| producers | 16 per call | 1 per call |
|---|---|---|
| 1 | 39.0 M items/s | 1.99 M |
| 2 | 39.4 M | 1.90 M |
| 4 | 37.6 M | 5.25 M |
| 8 | 35.6 M | 4.88 M |
| 16 | 29.0 M | 4.65 M |
| 32 | 11.1 M | 2.80 M |
| 63 | 11.6 M | 1.56 M |

Beyond about 8 producing CPUs the list's mutex collapses: at 16
single-item producers, 698 thousand adaptive-mutex spins per second,
97 % of them producers on that one lock with an average wait of 21.6 us,
while the consumer waits 57.8 us for each of its own two acquisitions
per pass.  The consumer starves behind the producers, passes become rare
and enormous, and the cooperative handler requeues most of each pass.

This is the one known structural limit of the current implementation,
and section 7.1 is what was decided about it.

### 6.3 The scheduler's properties

Each row is a property from `../SCHED.md` S7 with the number the three
environments produced.  The simulator is a discrete-event model of the
same scheduler source; the guest is a 4-vCPU bhyve VM on a WITNESS
kernel, which taxes everything.

| property | simulator | guest (WITNESS) | a07 |
|---|---|---|---|
| fairness, equal weight, 50 vs 500 us items | 0.999 | 0.999 | 0.999 |
| fairness, weights 2:1 | 1.998 | 2.000 | 2.000 |
| latency, light 1 kHz queue against a 20 us flood, worst | 399 us, bound 826 | 273 to 434 us | 214 to 221 us, bound 420 |
| anti-gaming: burst-and-pause share of a flood's | 0.80 | 0.886 | 0.874 |
| overrun: budget-ignoring 8 ms passes, share | 1.22 | 1.22 | 1.27 |
| callout lateness on a saturated CPU | n/a | 0.4 to 1.3 ms | 66 to 69 us |

The gaming row is the grace rule working: a producer that bursts 32
items and pauses collects one boost in a three-second run and thousands
of grace-path arrivals, and ends with slightly *less* service than the
flood it competes with, so pausing gains it nothing.

### 6.4 The observability cost

| configuration | items/s | cost |
|---|---|---|
| probe sites compiled out | 47.4 to 47.5 M | reference |
| compiled in, none enabled | 47.2 M | 0.6 %, the disabled check per site |
| `pass-start`, `pass-end`, `round-end` enabled | 45.7 M | 3.6 % at 268 k firings/s, about 140 ns each |
| `enqueue` enabled with the translator | 31.0 M | 35 % at 1.9 M firings/s, about 180 ns each |

The conclusion is in the documentation rather than the code: pass-level
probes are safe to leave running, the per-item probe is for short looks,
and the always-on counters answer most of the same questions for free.

### 6.5 The first client

if_pair was ported and measured against a frozen copy of its own
taskqueue predecessor, loaded side by side on the same boot, so that
every number is an A/B pair.  Throughput between two jails over one
pair, 5 s runs at 16 KB MTU, receiver side:

| TCP connections | kwq driver | taskqueue baseline |
|---|---|---|
| 1 | 36.4 to 37.5 Gbit/s | 21.2 to 36.9 |
| 2 | 60.1 to 73.2 | 58.2 to 65.6 |
| 8 | 209 to 226 | 215 to 240 |
| 32 | 446 to 495 | 488 to 496 |
| 128 | 337 to 338 | 339 to 341 |

Both columns span three runs on two days; the run-to-run spread is wider
than the difference between the drivers at every connection count.

Parity within the run spread, and the exit criterion (within 5 % of the
recorded 479 Gbit/s peak, at least 337 at 128 connections) met.  The
port removed 209 lines from the driver on balance, 399 deleted against
190 added: the taskqueue pool and its two `SYSINIT`s, the three-state
doorbell, the batch-and-yield governor with its two sysctls and the
priority re-assertion they needed, and the per-CPU queue array.

Eight pairs at once, 8 / 64 / 256 total connections, with a ping
crossing every pair during the load.  Two runs of the same
configuration, a day apart, because they do not agree about the average:

| | kwq | baseline |
|---|---|---|
| total throughput, 2026-10-01 | 214 / 357 / 335 Gbit/s | 216 / 356 / 335 |
| total throughput, 2026-09-30 | 212 / 354 / 341 | 200 / 358 / 338 |
| round-trip average, 2026-10-01 | 0.05 / 1.0 / 7.1 ms | 0.05 / 1.6 / 35.4 |
| round-trip average, 2026-09-30 | 0.05 / 5.8 / 4.5 | 0.05 / 3.1 / 3.2 |
| round-trip maximum, 2026-10-01 | 0.4 / 64 / 89 ms | 0.3 / 48 / 785 |
| round-trip maximum, 2026-09-30 | 0.4 / 85 / 54 | 0.5 / 91 / 67 |

Throughput is a tie in both runs, and the *maximum* favours kwq in
both: whatever else varies, the round bounds the tail.  The average
does not reproduce.  On 10-01 kwq's average was a fifth of the
baseline's at 256 connections; on 09-30 it was half again worse than
the baseline's at both 64 and 256.  The honest reading is therefore
narrower than one run alone suggests: the scheduler reliably bounds the
worst case, and whether it also improves the mean depends on conditions
this pair of runs does not isolate.

The mechanism behind the bounded tail is visible in the baseline's worst
figures.  It drains a whole backlog in one task run, with the ping's own
work queued behind it, which on 10-01 reached 0.8 seconds; kwq's round
over the 16 backlogged queues on each CPU cannot exceed 16 quanta plus
the overshoot, and its worst figure across both runs is 89 ms.  A queue
depth sweep from 64 to 4096 items per
list confirmed the shape: throughput is flat from 512 up on both
drivers, drop-free from 1024, and at every depth kwq's tail is bounded
while the baseline's reaches 0.5 to 2.3 seconds at three of the seven
depths.  (The sweep's shallow depths show *higher* throughput at 64
connections on both drivers, 453 and 462 Gbit/s against about 355; that
is bufferbloat relief bought with hundreds of thousands of drops, which
TCP pays for in retransmissions, and not a reason to lower the bound.)

The single-flow case is the one place kwq is measurably behind, by about
10 % when the stack's own costs are stripped away by per-CPU TCP timers:
both directions of one flow hash to one CPU, whose worker runs 78
thousand passes per second of two to four packets each, so kwq's fixed
per-pass cost is paid 78 thousand times a second with nothing to amortise
it against.  Changing the quantum does not help, which confirms the
diagnosis.  Cheaper passes is the open item (section 9).

## 7. Alternatives rejected or deferred

### 7.1 The fan-in limit: three options, none chosen yet

Section 6.2 shows one mutex-protected list collapsing beyond about 8
producing CPUs, and if_pair's hash steering makes every CPU a producer
into every target list.  Three options were costed:

**(a) Keep the single mutex-protected list.**  FIFO across all
producers, 48 KB per queue on 128 CPUs, fine to about 8 producers.  This
is what ships.

**(b) Per-source-CPU sublists.**  No producer shares a cache line with
another, at `ncpu^2` lines, 2 MB per queue on a 128-CPU machine.  Ruled
out on footprint, and on semantics: FIFO would hold only per source, so
a flow whose sender migrates between two packets could be reordered at
the swap.  The user's ruling was explicit: too much memory, revisit when
real clients demonstrate the need.

**(c) A lock-free single-consumer list.**  One atomic exchange per
enqueue, one per swap, no extra memory, arrival order kept.  The
contention becomes a single shared cache line whose transfer rate, 5 to
10 M per second at 63 producers, bounds it; with batched enqueue each
exchange carries a list.  The cost is that it changes the locking rules,
the doorbell protocol and the drain protocol, and the drainer-exit race
it introduces is genuinely hard: the project's reading of libdispatch's
own solution (the DIRTY-bit three-step in `queue_internal.h`, recorded
in `../../NOTES.md`) is the longest comment in that file and carries a
documented race of its own.  A mutex whose measured contention cost at
128 cores is 0.17 CPU-equivalents does not justify that yet.

What decided the deferral was measuring the real client rather than the
synthetic worst case.  a synthetic model of if_pair's shape, with
realistic per-packet work on both sides, puts the sender's enqueue at
46 to 49 ns uncontended and 300 to 330 ns where seven other senders
share the list, which is a fifth of a sender's CPU at 1 us of stack work
per packet and more than half at 500 ns.  Fan-in into a *single* worker
CPU is worse, 565 ns at 8 senders and 1.2 us at 32, but it also costs
that worker only 11 % to 19 % of its time, so the queue keeps 85 % of
its single-sender throughput.  Four packets per call takes the sender
back to 67 to 78 ns at any fan-in.  This is the same lock-per-packet
shape the driver already had, so the port neither improved nor worsened
it.  The lock-free list earns its complexity when a client appears whose
packets are too cheap for batching to rescue, and not before.

### 7.2 Producer-side staging

Holding items in a per-producer buffer until N accumulate or a timer
fires buys one mutex acquisition per item and pays for it three times: a
flush timer, which is latency for the last item of every burst and a new
tunable; a staging area per (producing CPU, target CPU), because the
target is chosen per item and a batch would have to be split by it
anyway; and a second copy of the state machine kwq already runs.  The
doorbell makes the buffer unnecessary.  Rejected, with the caveat from
section 6.1 that a client which *already holds* a list must hand it over
whole.

### 7.3 A packet-count batch knob

if_pair had one, `net.link.pair.batch`.  It counted packets, a unit that
stretches with MTU until 64 of them overrun a clock tick, and it
required the driver to decide something it had no information about.
Replaced by the class quantum in CPU time, and the driver now decides
nothing about batch size: the batch is whatever accumulated, bounded by
time.

### 7.4 A calendar wheel for parked queues

The ring is a linked list and a round walks it, so a worker with many
parked queues walks past them each round.  An array of lists indexed by
quanta of debt would make that O(1).  Measured, the walk is not a cost
at realistic queue counts, and the idea is recorded in `../SCHED.md`
S11 for the day it is.

### 7.5 `KWQ_F_VNET` for movable objects

A queue can have the creator's vnet set around its handler.  That is
right for a per-vnet service created in a `VNET_SYSINIT` and wrong for
anything that moves: an interface moved with `if_vmove()` leaves the
cached vnet stale, and the interface itself is the authority.  A
`kwq_set_vnet()` was considered and rejected, because a cached copy of a
fact needs someone to update it at exactly the right instant, and the
original costs the same pointer read.  Movable clients set the vnet in
the handler, per pass.

### 7.6 An automatic controller

The counters a controller would need are exposed, per-queue overruns,
round lengths, rejection counts, steal counts, and no controller is
shipped.  Linux's "flag after 10 ms continuously" and SEDA's batching
controller are both plausible; neither can be justified on this
project's data yet, and a controller added speculatively is a second
policy to debug.

## 8. Validation

Four layers, because the scheduler is the part that cannot be tested by
inspection.

**A simulator that links the real scheduler.**  `sim/` builds
`kwq_sched.c`, the scheduler proper, against mock clocks and threads in
userland, with the invariants checked after every step and bounds
checked over 17 scenarios plus 100 randomised seeds, 9.76 million
scheduler steps in the run of 2026-10-01.  The scheduler is written as
pure functions
over explicit state for exactly this reason.  Two rules in the shipped
code exist because the simulator found their absence: the debt rule
(section 5.5) and the clock glitch guard (section 5.8).

**A synthetic client module.**  `kwq_test.ko` drives kwq through
sysctl with 17 scenarios: the P0 correctness set (lifecycle, FIFO,
notifier coalescing, rejection, discard, a deliberately sleeping handler
that must panic an INVARIANTS kernel), the scheduler property set that
produced section 6.3, cost and baseline measurements, and an `ifpair`
scenario that reproduces a real client's shape, N interfaces hashed over
worker CPUs with busy-spin handlers, which is what produced section
6.2's and 6.5's numbers.

**An ATF suite under kyua.**  `tests/kwq_test.sh`, 14 cases that assert
the property rather than the absence of a crash: fairness within 10 %,
the light queue's latency within ten quanta, at most two boosts for a
burst-and-pause producer, overruns counted and the queue parked,
exactly-once delivery over 100 lifecycle rounds.

**Two machines, and an A/B baseline for the client.**  Every change is
run in a bhyve guest with INVARIANTS and WITNESS, where assertions fire
and the lock order is checked, and on the 128-core Ampere, where the
numbers mean something.  For the if_pair port the predecessor driver was
frozen as a renamed, co-loadable module so that both run on the same
boot against the same kernel, which is the only comparison worth
reporting on a shared machine.

Three of the bugs this found are worth naming, because they are the kind
a reader should expect in their own client.  A handler that walked its
own remainder to find the tail turned a deep backlog into a spiral and
put 16 thousand items on one CPU while fifteen workers idled; the KPI
now takes a `NULL` tail.  A test harness that read a live tunable inside
its handler indexed past an array when the next run changed the knob
while the previous queue was still draining, and panicked a shared
machine eight times before the pattern was recognised.  And an
`sbintime_t` to nanosecond conversion that used a shift instead of
`sbttons()` silently wrapped after 2.1 seconds of uptime.

## 9. Open questions

- **Cheaper passes.**  The single-flow case (section 6.5) pays kwq's
  fixed per-pass cost 78 thousand times a second.  Candidates: one lock
  section per pass instead of two, fewer clock reads, and letting a
  worker take a second queue's list before sleeping.
- **The quantum against the latency tail.**  The round length sets the
  tail (section 6.5); a smaller quantum shortens it and costs passes.
  The relationship is measurable and unmeasured.
- **The fan-in remedy**, when a client needs it (section 7.1).
- **BLOCKING.**  The class is specified and refused at create.  It needs
  either cmwq-style spawn-on-block, which needs a scheduler hook
  FreeBSD does not export, or reserved workers plus a memory-reclaim
  guarantee.  Until then GELI keeps its own pool and the design says so.
- **Where epoch callbacks belong**, NET or BULK, is unsettled.
- **Whether ULE preempting a `PI_NET` worker with an equal-priority
  ithread** needs a policy of its own.

## References

Within the project: `../KWQ.md` (design), `../SCHED.md` (scheduler
specification, invariants, measurements), `../../POOLS.md` (cross-OS
survey), `../../DISPATCH.md` (libdispatch study), `../../NOTES.md`
(running analysis), `../../samples/` (raw measurement logs),
`../GLOSSARY.md` (terminology).

External: Shreedhar and Varghese, "Efficient Fair Queueing using
Deficit Round Robin" (1995); Hoeiland-Joergensen et al., RFC 8290
(FlowQueue-CoDel); the CAKE queue discipline; Mogul and Ramakrishnan,
"Eliminating Receive Livelock in an Interrupt-driven Kernel" (1996);
Druschel and Banga, "Lazy Receiver Processing" (1996); Welsh, Culler and
Brewer, "SEDA" (2001); Linux `Documentation/core-api/workqueue.rst`;
illumos `uts/common/inet/squeue.c` and `uts/common/os/taskq.c`; NetBSD
`sys/kern/kern_threadpool.c`; Apple libdispatch `src/queue.c`.
