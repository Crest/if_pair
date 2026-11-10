# kwq(9) reference

The complete surfaces of the kwq kernel work queue service: the KPI, the
types and flags, the error and context rules, the sysctl tree, the
DTrace provider, the DDB command and the build.  Names, ranges and
defaults are exact as of 2026-10-01 and were read from the code, not
from the design documents.  Where this and `../KWQ.md` or `../SCHED.md`
differ, those are normative.  The entries added from 2026-10-28 on, the
two optional wake regimes, the urgent exemption and their counters,
were read from the merged module on 2026-11-02; the BULK entries
(stealing, scatter, placement, `KWQ_F_PROMPT`, the items that stop part
way and `item_min_us`) on 2026-11-10.

Contents: 1 synopsis; 2 types; 3 flags and constants; 4 functions;
5 the handler contract; 6 errors; 7 caller contexts; 8 what a handler
may do; 9 classes and their defaults; 10 memory; 11 sysctl; 12 DTrace;
13 DDB and other tools; 14 build and install; 15 implementation status.

---

## 1. Synopsis

```c
#include <sys/param.h>
#include <sys/mbuf.h>		/* optional: enables the mbuf macros */
#include "kwq.h"		/* kwq/kwq/kwq.h */

MODULE_DEPEND(myclient, kwq, 1, 1, 1);
```

Link with nothing: kwq exports its KPI through `EXPORT_SYMS` in
`kwq.ko`, and the module dependency loads it.

## 2. Types

### struct kwq

Opaque.  A pointer from `kwq_create()`, valid until `kwq_destroy()`.

### struct kwq_item

```c
struct kwq_item {
	STAILQ_ENTRY(kwq_item)	kwi_link;
};
```

Embedded in the client's object, at any offset; recover the object with
`__containerof()`.  One pointer, 8 bytes.  An item is in exactly one of
three states: not queued, on one (queue, CPU) list, or owned by the
handler that received it.  The client is responsible for never
enqueueing an item that is already queued.

| macro | meaning |
|---|---|
| `KWQ_ITEM_NEXT(q, it)` | the next element of a handler's list, `NULL` at the end.  Call it before acting on `it`; for a notifier that call is the pending-to-idle flip (section 5) |
| `KWQ_ITEM_INIT(it)` | clear the link; call before every enqueue |
| `KWQ_MBUF_ITEM(m)` | the item inside an mbuf (its `m_stailqpkt` link) |
| `KWQ_ITEM_MBUF(it)` | the mbuf an item belongs to |

The mbuf macros exist only if `<sys/mbuf.h>` was included first; two
`CTASSERT`s check that `m_stailqpkt` is layout-identical to a
`kwq_item` and shares the `m_nextpkt` offset.

### kwq_handler_t

```c
typedef void kwq_handler_t(struct kwq *q, struct kwq_item *head, int n,
    void *ctx);
```

Called by a worker with a private list of items in FIFO order.  `ctx` is
the pointer given to `kwq_create()`.  See section 5.

### struct kwq_params

```c
struct kwq_params {
	u_int	limit;		/* items per CPU list; 0 = class default (4096) */
	u_int	weight;		/* 1..8; 0 = 1 */
	u_int	nreserve;	/* BLOCKING + KWQ_F_RESERVE only; must be 0 today */
	int	domain;		/* NUMA domain for kwq's own memory; -1 = the CPU's own */
};
```

Zero the structure before filling it, then set `domain` explicitly.
`limit`, `weight` and `nreserve` treat 0 as "the default", but `domain`
does not: a zeroed `domain` means NUMA domain 0 literally, and every
per-CPU structure of the queue is then allocated there.  `-1` is "each
CPU's own domain" and is what a client wants unless it knows better.
Passing `p == NULL` to `kwq_create()` gets `-1` and the class defaults.
`limit` is per CPU list, not per queue.

### struct kwq_notifier

```c
struct kwq_notifier {
	struct kwq_item	kn_item;
	int		kn_cpu;
	u_int		kn_state;	/* kwq's, do not touch */
};
```

Client-allocated and client-owned, bound to one CPU for its lifetime.
Must outlive every `kwq_notify()` on it and be cancelled or idle before
the memory is freed.

### enum kwq_class

```c
enum kwq_class { KWQ_NET = 0, KWQ_BULK = 1, KWQ_BLOCKING = 2 };
#define KWQ_NCLASS 3
```

## 3. Flags and constants

| flag | value | meaning |
|---|---|---|
| `KWQ_F_INACTIVE` | 0x01 | create inactive; enqueues get `ENXIO` until `kwq_activate()` |
| `KWQ_F_STEALABLE` | 0x02 | BULK only: other CPUs may take batches of it, idle BULK workers under `place` 0 and 2, the busy worker's hand-off under `place=1` (SCHED.md S16, S17); no `kwq_notify()` on such a queue |
| `KWQ_F_DISCARD` | 0x04 | `kwq_drain()` hands the backlog to the handler with `n < 0` instead of running it |
| `KWQ_F_RESERVE` | 0x08 | BLOCKING only; asserted at create today |
| `KWQ_F_VNET` | 0x10 | the worker sets the creator's vnet around the handler |
| `KWQ_F_SPIN` | 0x20 | per-CPU list locks are spin mutexes: producers may be interrupt filters or hold spin locks |
| `KWQ_F_PROMPT` | 0x40 | for BULK: the queue's passes come ahead of the ring and the new list, each with a fresh quantum, until the prompt queues have had one quantum of service since the last ring slot (SCHED.md S4.8); for short items that should not wait behind long ones.  `kwq_create()` does not check the class |
| `KWQ_F_ALL` | 0x7f | the mask of valid flags |

| constant | value | meaning |
|---|---|---|
| `KWQ_CPU_ANY` | -1 | BULK only: a CPU chosen by `kern.kwq.bulk.place`; under 2 and 0 the nearest CPU in cache-topology order whose BULK worker sleeps with this queue's list empty first.  Under 2 (P8, the default on one NUMA domain) the search covers the caller's group of CPUs and two near ones, then one far group's idle line, then the caller's CPU unless it is a donor, the nearest CPU that is not, and the shallowest of the caller's and the next four lists; under 0 (today's code, the default on more domains) the caller's CPU if its list is empty, then the shallowest of four; under 1 gossip's `kp_place()`, which keeps a resubmitting handler's item on its own CPU below L_local and tries an item cheaper than 20 us on the SMT sibling and the own CPU before an idle core (SCHED.md S16, S17).  Bounded on purpose, under 0 by `steal_scope` and `steal_scan`, under 2 by the near groups and one far line: scanning a whole 128-CPU machine would read 128 cold lines per enqueue |
| `KWQ_NAMELEN` | 32 | buffer size; a name is 1 to 31 characters |
| `KWQ_LIMIT_NONE` | `UINT_MAX` | unbounded; the effective bound is `INT_MAX` so a batch's `n` fits an `int` |

## 4. Functions

### kwq_name_valid

```c
bool kwq_name_valid(const char *name);
```

True if `name` is 1 to 31 characters, each from `[A-Za-z0-9_-]`.

Exported with the rest of the KPI, so a client that derives a queue
name from user-controlled text can check it before `kwq_create()`,
which refuses an invalid name with a line in the kernel log.

### kwq_create

```c
struct kwq *kwq_create(const char *name, enum kwq_class cls, uint32_t flags,
    const struct kwq_params *p, kwq_handler_t *fn, void *ctx);
```

Allocates the queue and one per-CPU structure per CPU, on the CPU's NUMA
domain, and registers `kern.kwq.<class>.queue.<name>`.  **Sleepable
context only.**  Allocation uses `M_WAITOK`, so memory is not a failure
mode.

Returns `NULL` in two different senses, and a client should treat them
differently.  An invalid name, a name already used in the class, and
`KWQ_BLOCKING` are *runtime* refusals with an explanatory line in the
kernel log: handle them.  A NULL name or handler, a bad class,
undefined flags, `KWQ_F_RESERVE`, `KWQ_F_STEALABLE` on a non-BULK
queue, `weight > 8` and `nreserve != 0` are *programming errors*: they
panic under INVARIANTS and return `NULL` silently otherwise.  No
`limit` is ever refused.  See section 6.

Create with `KWQ_F_INACTIVE` whenever the handler's context is not ready
at create time, which is almost always.

### kwq_activate

```c
void kwq_activate(struct kwq *q);
```

Makes the queue accept work.  Configuration time only; activating twice
or after a drain is a bug and is asserted.  Cannot fail.

### kwq_enqueue

```c
int kwq_enqueue(struct kwq *q, int cpu, struct kwq_item *it);
```

Appends one item to the (queue, `cpu`) list and, if that list was idle,
rings the doorbell in the same lock hold.  Idle is the precise
condition, not merely empty: a list whose pass is still running, or
which is already on the worker's ring or new list, is not rung again.

Returns 0 on success, and then the item belongs to kwq.  On error the
item is untouched and still the caller's.  Never sleeps, never
allocates.  Errors: `ENOBUFS`, `ENXIO`, `EINVAL` (see section 6).

`cpu` is a CPU id, or `KWQ_CPU_ANY` for a BULK queue.  An invalid or
absent id maps to the caller's CPU.

CPUs are enumerated once, at module load, because FreeBSD has no
hot-plug event to subscribe to; workers exist for every CPU present
then.  Enqueueing to a CPU that has since been halted administratively
delays the work rather than losing it: a bound thread still runs when an
IPI wakes its CPU.  There is no drain-to-neighbour path, so a client
that steers by hash over a machine with halted CPUs will see latency on
those lists.

### kwq_enqueue_urgent

```c
int kwq_enqueue_urgent(struct kwq *q, int cpu, struct kwq_item *it);
```

`kwq_enqueue()` for an item whose latency the caller will not trade for
the optional regimes of KWQ.md S16: the doorbell is rung at once, past
the tick regime's bound and past the idle-class wait, as a notifier, a
drain and a cancel are, and an item that joins a list already placed
but not yet served wakes the worker as well (the urgent nudge,
`wakes_urgent`).  The one exception is the tick regime with
`tick_urgent_us` set below `tick_us`: there the item waits at most
`tick_urgent_us`, and the urgent items within that bound share one
wake.  That knob applies to these items only; notifiers, drains and
cancels always ring at once.  Same arguments, same errors, same
guarantees.

The wake is the cost, and the wake is what the regimes save, so this is
for a small latency-bearing item among big ones, not for the common
case: if every item is urgent the class may as well have both knobs at
0.  if_pair exposes it as `net.link.pair.urgent_bytes` (0 = off):
packets shorter than that many bytes ring urgently, which is the knob
for a pair carrying bulk segments and small requests and replies
through one worker.  A class whose items are all alike does not
need it.  An urgent ring from the worker's own CPU ends the idle-class
wait as well, so a client whose same-CPU producer marks its own small
items urgent loses the wait: a receiver pinned to its worker's CPU with
`urgent_bytes` 256 parked not once and fell back to the doorbell's
throughput (WAKE.md S12.5).  if_pair therefore never marks a TCP
segment without data that only acknowledges; the acknowledgements of
TCP in a tunnel or of a protocol over UDP it cannot see and still
marks by size.

### kwq_enqueue_list

```c
int kwq_enqueue_list(struct kwq *q, int cpu, struct kwq_item *head,
    struct kwq_item *tail, int n);
```

The same, for a list of `n` items already linked through their
`kwq_item` links, `head` first and `tail` last.  All-or-nothing: on
error nothing is queued.  One lock hold, one limit check, one doorbell.
`n` must be the true length and positive.

### kwq_cpu_for_hash

```c
int kwq_cpu_for_hash(uint32_t hash);
```

Maps a flow hash onto a CPU that has workers.  `hash % (mp_maxid + 1)`,
the same mapping TCP uses for `net.inet.tcp.per_cpu_timers`, so a
connection's kwq worker and its timers share a CPU; an absent id is
remapped through a table built at module load to a fixed present CPU, so
one hash always yields one CPU even where CPU ids have holes.  Any
context, no locks.

### kwq_scatter, kwq_group_leave

```c
struct kwq_group {		/* client-owned; kwq keeps the fields */
	volatile u_int	kg_left;
	u_int		kg_n;
	void		(*kg_done)(void *);
	void		*kg_arg;
};
int kwq_scatter(struct kwq *q, struct kwq_group *g, struct kwq_item *list,
    int n, void (*done)(void *), void *arg);
void kwq_group_leave(struct kwq_group *g, int n);
```

Fan the `n` items of `list` (plain links, NULL-terminated, no
notifiers) across the caller's CPU and the idle BULK workers near it,
one chunk each in order, and arm `g`.  The handler calls
`kwq_group_leave(g, k)` for every `k` items of the group it has
finished (the client's item knows its group); the leave that finishes
the last item calls `done(arg)` once, in that handler's context, after
every item's work is visible.  All or nothing: `ENOBUFS` if a target
list has no room (checked before the first append; the appends are then
exempt from the limit, as a requeue is), `ENXIO` if the queue is
draining or inactive, `EINVAL` for a non-BULK queue or a NULL argument;
on any error nothing is queued and `done` is never called.  The group
may be reused once `done` has run.  Not from an interrupt filter or with
a spin mutex held: it takes the read side of an rmlock that fences the
queue's drain (SCHED.md S16).

### kwq_notifier_init, kwq_notify, kwq_notify_cancel

```c
void kwq_notifier_init(struct kwq_notifier *nf, int cpu);
bool kwq_notify(struct kwq *q, struct kwq_notifier *nf);
void kwq_notify_cancel(struct kwq *q, struct kwq_notifier *nf);
```

`kwq_notifier_init()` binds the notifier to `cpu` for its lifetime;
`KWQ_CPU_ANY` is refused (asserted), because one CPU's list lock is what
protects the notifier's state.

`kwq_notify()` cannot fail.  The signal joins the item list of the
notifier's CPU at the tail and counts toward that list's `depth`, though
it is never refused by the limit; at most one instance of each notifier
is ever pending, so the slots signals can take are bounded by the number
of notifier objects.  It returns `true` if the notifier was idle
and is now queued, and `false` otherwise: already pending (counted in
`coalesced`), the queue draining or inactive (counted in `rejected`,
with a `kwq:::reject` probe carrying `ENXIO`), a cancelled notifier (not
counted), or a `KWQ_F_STEALABLE` queue, which is also asserted.  Exempt
from `limit`.  Legal
from any context the queue's lock type allows, including from inside the
handler for the notifier being handled.

`kwq_notify_cancel()` marks a pending notifier cancelled and waits until
the handler has consumed it, ringing the doorbell if the list was idle
so that a pass comes, and then waits for the pass in progress on that
CPU to end, so a run already under way has returned when the cancel
returns.  A notifier found idle, which includes one the handler is
running right now, is marked dead at once.  Either way the notifier
refuses signals until `kwq_notifier_init()` is called again.  On a queue
that is continuously busy the wait for a pass boundary can take tens of
milliseconds, as it did in the two-list design.  **May sleep**; teardown
only.  It is not needed after `kwq_drain()`: the drain runs or discards every pending notifier, so
when it returns each one is idle and not running, and the client may
free the memory directly.  Cancel is for releasing one notifier while
the queue stays in service.

### kwq_requeue

```c
void kwq_requeue(struct kwq *q, struct kwq_item *head, struct kwq_item *tail,
    int n);
```

Handler only.  Prepends `n` already-admitted items to the current
(queue, CPU) list, ahead of anything that arrived during the pass, so
per-CPU FIFO is preserved.  Requeued items are exempt from `limit`, so a
requeue can take `depth` and `maxdepth` above it: the bound governs
admission only.  Cannot
fail.  `tail` may be `NULL`, meaning "to the end of the list this pass
handed over", which the worker knows: a handler never walks its
remainder.  Pass an explicit tail only when requeueing a list that is
not the pass's own suffix.

`kwq_requeue()` is a list operation, not a return barrier: it is legal
to run an epilogue afterwards, which is what the real client does
(`kwq_requeue()`, then `break`, then restore the vnet and release the
interface reference).  What a handler must not do after it is touch the
requeued items, which now belong to the queue again.

### kwq_budget_left

```c
uint64_t kwq_budget_left(struct kwq *q);
```

Handler only.  Nanoseconds of this pass's budget still unspent, in the
worker's own CPU time.  0 means stop: requeue the remainder and return.
Returns 0 in exactly four cases: a higher class is waiting for this CPU,
the budget is spent, the share limit's allowance is spent while
something else is runnable on the CPU (counted in `cap_cuts`), or the
clock read looked impossible (negative, or over one second).  A positive
value is an *estimate*: the clock is read at least every
`budget_check_every` calls, and early whenever the estimate comes within
a tenth of a quantum of the end, so the answer to "is there time for
another item" is reliable near the boundary while the number itself may
be a few microseconds out mid-pass.  Never an error.

The tick guard that bounds a worker's CPU occupancy is *not* visible
here: it runs between passes, in the worker, and does not affect what a
handler's budget query returns.  See the cadence guidance in
[cheatsheet.md](cheatsheet.md) section 5.

### kwq_item_begin, kwq_item_stop

```c
void kwq_item_begin(struct kwq *q);
bool kwq_item_stop(struct kwq *q);
```

Handler only, for an item the handler can leave part done and go on
with in a later pass (kwq-geli's bios).  The handler calls
`kwq_item_begin()` when it starts or resumes such an item in a pass and
`kwq_item_stop()` at each point where it could stop.  True means stop
here: keep where the item stopped, `KWQ_ITEM_INIT()` it, requeue it in
front of the remainder and return.  A higher class waiting stops the
item at any such point; the pass's budget and the share limit, where
`kwq_budget_left()` reads 0, stop it only once it has run
`kern.kwq.<class>.item_min_us` in this pass, because what a part-done
item leaves behind goes cold while its queue waits (SCHED.md S4.9).
With `item_min_us` 0, `kwq_item_stop()` is `kwq_budget_left() == 0`.
An item shorter than `item_min_us` finishes and overruns its budget by
at most that.

### kwq_drain, kwq_destroy

```c
void kwq_drain(struct kwq *q);
void kwq_destroy(struct kwq *q);
```

`kwq_drain()` stops admission (further enqueues get `ENXIO`), waits for
every pass in flight, and disposes of the backlog on every CPU: run
through the handler, or handed back with `n < 0` for a `KWQ_F_DISCARD`
queue.  When it returns, no pass is running and no item or notifier is
queued.  **May sleep.**  Calling it from a handler deadlocks and is
asserted.

`kwq_destroy()` frees the queue and removes its sysctl subtree.  Asserts
that the queue was drained.  **May sleep.**  Neither can fail.

The assertion holds even for a queue that never accepted work: a client
that creates a queue, then fails its own next setup step, must still
call `kwq_drain()` before `kwq_destroy()` or panic under INVARIANTS.
The drain of an inactive queue is cheap and has nothing to hand back.

### kwq_name, kwq_class

```c
const char	*kwq_name(const struct kwq *q);
enum kwq_class	 kwq_class(const struct kwq *q);
```

Introspection, any context.

## 5. The handler contract

```c
void fn(struct kwq *q, struct kwq_item *head, int n, void *ctx);
```

- `head` is a private list of `|n|` items in FIFO order.  The handler
  owns them and must consume every one: process, free, hand on, or give
  back with `kwq_requeue()`.  It cannot refuse work.
- `n > 0`: run them.  `n < 0`: release `-n` items without running them;
  this happens only during `kwq_drain()` of a `KWQ_F_DISCARD` queue.
- Pending notifiers are *in* the list, at their arrival positions and
  counted in `n`, so items and signals are delivered in one arrival
  order (since 2026-10-08; before that notifiers came first, one per
  call).  The handler tells a notifier from an item by pointer identity
  against its own notifier objects.  Fetching an element's successor
  with `KWQ_ITEM_NEXT(q, it)` before acting on it, which the usual walk
  does, is what flips a notifier from pending to idle, so a signal that
  lands after that fetch is a fresh arrival and one that landed before
  is covered by the level the handler reads next; re-arming from inside
  the handler is therefore safe.  A notifier is budgeted like an item:
  the handler may stop before it and hand it back with `kwq_requeue()`
  as part of the remainder, as long as it has not yet fetched that
  element's successor.  Never `KWQ_ITEM_INIT()` a notifier.
- A handler that returns without walking leaves a notifier pending.  The
  worker settles an unwalked *head or tail* of the pass on the handler's
  behalf, recognising it by a state word unchanged since the swap-out
  (a consume bumps a generation kept in the spare bits of `kn_state`),
  and asserts under INVARIANTS so a test shows the mistake at once.  That
  covers a lone signal, which is the notifier-only client's whole case;
  a notifier in the middle of a batch is reached only by the walk, and
  the drain asserts that none is left pending.
- At least one item is processed per call: the progress rule guarantees
  the first item has budget even if the queue is in debt.
- NET and BULK handlers run with `THREAD_NO_SLEEPING()` in force; a NET
  handler also runs inside the network epoch, entered once per pass, and
  with the creator's vnet set if the queue has `KWQ_F_VNET`.
- No service lock is held on entry, and none may be held on return
  (none is passed to the handler, so this is automatic).
- The handler may be called concurrently on different CPUs for the same
  queue, one pass per CPU at a time.

### Priority, promised

A handler runs at its class's priority from the call to its return and
may assume it; the worker demotes, yields or hands the CPU over only
between handlers, so a handler must not depend on priority propagation
to anything it blocks (KWQ.md S3, SCHED.md S6.3).  A module built with
`KWQ_HARD_INVARIANTS` asserts that no pass begins below the class
priority, a panic on an INVARIANTS kernel; the default build counts it
in the worker's `prio_violations`, which reads zero on a conforming
build (it read the share limit's demoted passes on the tree before the
pass-level limit).

### Latency the items see

The bounds a client may design against, from SCHED.md S7:

| row | case | bound | measured |
|---|---|---|---|
| B5a | an item that finds its worker asleep | one sleep-wake and nothing else: the doorbell, the switch-in, one pass.  With `tick_us` and `wake_delay_us` at 0, the defaults, nothing in kwq may delay the first wake of a backlog; with one set, an item waits at most its bound (KWQ.md S16) | 8 to 14 us through a pair on the two hardware test machines, within 1 us of a direct wakeup |
| B5b | an item beside a busy queue on the same CPU | the pass in progress (B1) plus one quantum-bounded pass of the other queue; with `tick_us` and `wake_delay_us` at 0; the tick regime adds up to `tick_us` to this row, the idle-class wait up to `wake_delay_us` for an item from the worker's own CPU | p99 25 to 38 us for a request-reply flow beside a bulk flow |
| B5c | a user thread competing for the worker's CPU | at most one `cap_period` plus one pass, the share limit's hand-over, not a ULE slice: a deliberate fairness cost | p99 120 to 720 us, against 62 us for the taskqueue driver that never yields |

B6, anti-gaming, is stated in the same service unit as the grace rule:
a queue obtains at most one boost per idle period during which the
worker delivered more than one ring's worth of service to other
queues, and the share limit's budget cut does not change that window
(SCHED.md S4.6, S7).

### How a pass is charged

A NET pass is charged the wall time it spanned, a BULK or BLOCKING
pass the thread's CPU time, so a BULK pass is not charged for time NET
preempted it and a BLOCKING pass not for its handler's sleep; the share
limit counts passes the same way (SCHED.md S1a.4, S6.3).  For NET the
wall-time charge is exact up to interrupt threads and lock blocks, both
counted in the worker's `switched`, and it stays: interrupt *context* is inside both
the wall clock and the thread's `td_runtime`, so no charge mode can
exclude it, and the hundreds of switched overruns per run that wall
time was seen to charge came from the earlier form of the share limit,
a demoted worker preempted inside its passes, which the pass-level
invariant removes.  Split charging (the thread's CPU time to the
deficit, wall time to the limit and the round) is the one alternative
with a measured contract and is the candidate if `switched` ever shows
interrupt threads or lock blocks in numbers; it is to be decided by a
battery that produces them, not by argument.  `overruns_switched`
attributes an overrun: `overruns - overruns_switched` is the handler's
own excess, the remainder is the system's.

## 6. Errors

| call | error | cause and what the caller does |
|---|---|---|
| `kwq_create` | `NULL` | **logged, and a legitimate runtime failure:** an invalid name, a name already used in the class, or `KWQ_BLOCKING`.  **Asserted, i.e. a programming error that panics under INVARIANTS and returns `NULL` otherwise:** a NULL name or handler, a bad class, undefined flags, `KWQ_F_RESERVE`, `KWQ_F_STEALABLE` on a non-BULK queue, `weight > 8`, `nreserve != 0`.  No `limit` value is refused: 0 means the class default and anything above `INT_MAX` is clamped |
| `kwq_enqueue`, `kwq_enqueue_list` | `ENOBUFS` | the chosen CPU's list holds `limit` items.  Item still the caller's: drop and count, or apply upstream backpressure |
| | `ENXIO` | `kwq_drain()` has begun, or the queue is still `KWQ_F_INACTIVE` |
| | `EINVAL` | `KWQ_CPU_ANY` on a non-BULK queue |
| `kwq_scatter` | `EINVAL` | not a BULK queue, a NULL group, list or `done`, or `n <= 0` |
| | `ENOBUFS` | a target list had no room; nothing queued |
| | `ENXIO` | the queue is draining or inactive; nothing queued |
| `kwq_notify` | never fails | `false` means coalesced or draining, not an error |
| `kwq_requeue`, `kwq_budget_left`, `kwq_activate`, `kwq_drain`, `kwq_destroy` | never fail | misuse is asserted under INVARIANTS |
| module unload | `EBUSY` | a queue still exists |

Enqueue never fails for contention, a busy worker, memory pressure or
interrupt context.  The only data-path failure is a *reject*, and it
returns the item.

## 7. Caller contexts

| context | allowed | not allowed |
|---|---|---|
| thread, sleepable | everything | - |
| ithread, SWI, callout, netisr, another kwq handler | `kwq_enqueue`, `kwq_enqueue_list`, `kwq_notify`, `kwq_scatter`; in a handler also `kwq_budget_left`, `kwq_requeue` | `kwq_create`, `kwq_drain`, `kwq_destroy`, `kwq_notify_cancel` (they sleep); `kwq_activate` |
| interrupt filter, or holding a spin mutex, or inside `critical_enter()` | the same calls but `kwq_scatter`, **on a `KWQ_F_SPIN` queue only** | the same calls on a sleep-mutex queue (WITNESS panics); `kwq_scatter` (it takes an rmlock); everything else |
| NMI, `SCHEDULER_STOPPED()`, KDB | nothing | everything.  After a panic the queue mutexes degrade to no-ops, so a `kwq_drain()` on the shutdown path needs no special case, but no new work may be submitted |

Client locks may be held across `kwq_enqueue()`: the per-CPU list locks
are leaves.

## 8. What a handler may do

| operation | NET / BULK | notes |
|---|---|---|
| `kwq_enqueue` to another queue, or this one | yes | to this queue it is new work, behind what is already queued |
| `kwq_notify` to any queue, including its own notifier | yes | the way to say "still not empty" |
| `kwq_requeue` the pass's remainder | yes | prepends, FIFO kept, notifiers among it; never a notifier whose successor the handler has already fetched (an item may be, after `KWQ_ITEM_INIT()`) |
| `kwq_item_begin`, `kwq_item_stop` | yes | around an item that can stop part way; true means requeue it and return |
| `kwq_budget_left` | yes | 0 means requeue and return |
| sleep mutex, rwlock, rmlock | yes, briefly | |
| spin mutex, `critical_enter`, `sched_pin` | yes, release before return | while held, only `KWQ_F_SPIN` queues may be enqueued to |
| `M_NOWAIT` allocation | yes, handle `NULL` | |
| `kwq_scatter` to a BULK queue | yes | `done` runs later, in the handler that leaves the group's last item |
| the network epoch | NET: already inside, and nesting your own `NET_EPOCH_ENTER` is legal.  BULK: enter it yourself if you need it | kwq enters the epoch once per pass for NET only |
| touching user memory | **never** | a worker is a kernel thread with no process address space; `copyin` is also sleeping |
| sleeping in any form | **no**, asserted | `tsleep`, `cv_wait`, sx, lockmgr, `M_WAITOK`, `copyin` |
| `kwq_create`, `kwq_activate`, `kwq_drain`, `kwq_destroy` | **no**, asserted | |
| waiting for another queue's work | **no** | deadlock; use a `kwq_scatter` completion callback instead |
| calling a peer's input path synchronously | only if it cannot loop back to a lock the enqueuer holds | the queue is the reentrancy break |

## 9. Classes and their defaults

| | NET | BULK | BLOCKING |
|---|---|---|---|
| worker priority | `PI_NET` | `PI_SOFT` | `PUSER` |
| thread names | `kwq_net/<N>` | `kwq_bulk/<N>` | none exist |
| workers | one per CPU | one per CPU | none created; a pool is specified for P7 |
| handler may sleep | no | no | yes |
| network epoch entered by kwq | yes | no | no |
| `quantum_us` default | 200 | 5000 | 5000 |
| `cap_period_us` default | 1000 | 10000 | 1000 |
| `limit` default | 4096 | 4096 | 4096 |
| stealing | never | `KWQ_F_STEALABLE`: idle workers nearest first in cache topology (`place` 0 and 2), or the busy worker's hand-off (`place=1`), batch moved and charged where it runs (SCHED.md S16, S17) | n/a |
| status | available | available | **refused at create** |

Between classes there is strict priority, enforced by the thread
priorities plus a per-CPU hand-back flag: a BULK round ends as soon as a
NET worker on that CPU wants its CPU back.  Within a class, deficit
round robin by weight.

## 10. Memory

Per queue, independent of how much is enqueued:

- one `struct kwq`, 256 bytes;
- one `struct kwq_cpu` per CPU, allocated on that CPU's NUMA domain and
  padded to a multiple of `KWQ_LINE`, which is
  `max(CACHE_LINE_SIZE, 128)` bytes.  Three such blocks in the current
  layout, one shared with producers and two private to the worker, 384
  bytes in total (asserted by compiling a `CTASSERT`, 2026-10-01): 48 KB
  per queue on a 128-CPU machine.

The first line of `kwq_cpu` is the producer/consumer shared block, with
every field the enqueue path touches inside its first 64 bytes;
`CTASSERT`s hold both boundaries.  Items cost nothing extra: the link is
in the client's object.

Queued work, worst case, is `limit` items per CPU:

| | items | at 16.9 KB per packet (16 KB MTU: one 16 KB cluster plus two mbufs) | at 2.3 KB per packet (1500-byte MTU) |
|---|---|---|---|
| one CPU list, `limit` 4096 | 4,096 | 69 MB | 9.4 MB |
| one queue, 128 CPUs | 524,288 | 8.9 GB | 1.2 GB |

A machine's mbuf pools usually bind first: on a 128-core Ampere with
549 GB of RAM, filling one queue's lists with 16 KB clusters would need
31 % of the machine's entire 16 KB cluster zone, and with 64 KB packets
94 %, so the senders block before the bound is reached.  Choose `limit`
from the client's own flow control, not from the default.

## 11. sysctl

### Tree

    kern.kwq                                  service-wide
    kern.kwq.<class>                          class knobs, read-only class data
    kern.kwq.<class>.cpu.<N>                  one worker thread
    kern.kwq.<class>.queue.<queue>            one queue
    kern.kwq.<class>.queue.<queue>.cpu.<N>    that queue's list on one CPU

`<class>` is `net`, `bulk` or `blocking`.  `<queue>` is the name passed
to `kwq_create()`, verbatim.

### Service-wide, read-only

| node | meaning |
|---|---|
| `kern.kwq.version` | KPI version, currently 1 |
| `kern.kwq.ncpu` | CPUs with workers |
| `kern.kwq.nqueues` | queues that exist, active or not |

The other read-only nodes, `kern.kwq.clock`, `kern.kwq.topo` and BULK
placement's under `kern.kwq.bulk.shard` (`group`, `ngroups`, `idle`,
`pick_*`, `spills`) and `kern.kwq.bulk.gossip` (`torus`, `place_*`,
`spills`, `handoffs`, `handed`, `merges`, `expired_used`), are listed
with their formats in `../OBSERVABILITY.md` section 3.

### Class knobs

All per class, except BULK's `steal*`, `place`, `shard.*` and
`gossip.*`, which exist under `kern.kwq.bulk` only.  RWTUN nodes are
also read from `loader.conf` when the module is preloaded.  Out-of-range values are refused with `EINVAL`.

| node | flags | range | default net / bulk / blocking | effect |
|---|---|---|---|---|
| `quantum_us` | RWTUN | 10 .. 1000000 | 200 / 5000 / 5000 | CPU time a weight-1 queue may use per round.  Larger: fewer hand-offs per item, longer worst-case wait for others |
| `limit` | RWTUN | 1 .. UINT_MAX | 4096 | default per-CPU bound for queues created with `limit = 0`; applies to queues created afterwards, clamped to `INT_MAX` there.  0 is refused (EINVAL; a loader tunable of 0 leaves 4096 and the kernel prints `Setting sysctl ... failed: 22`): every queue created afterwards would refuse every enqueue |
| `penalty_rounds` | RWTUN | 1 .. 1024 | 32 | how many quanta of debt a queue may accumulate, i.e. the longest parking |
| `budget_check_every` | RWTUN | 1 .. 64 | 8 | how often `kwq_budget_left()` reads the clock; 1 is exact and expensive |
| `item_min_us` | RWTUN | 0 .. 1000000 | 0 / 2000 / 0 | an item that can stop part way (`kwq_item_stop()`) stops for the pass's budget or the share limit only after running this long in a pass; a higher class stops it at any point; 0: at any point (SCHED.md S4.9) |
| `cap_permille` | RWTUN | 1 .. 1000 | 500 | share limit: the share of its CPU the worker may spend in passes per period while other threads are runnable, in 1/1000; the rest of the period it waits between passes as an idle-priority thread.  Equal share by default, 1000 = off, and the worker then obeys ULE's slice end as a process would (SCHED.md S6.3) |
| `cap_period_us` | RWTUN | 100 .. 1000000 | 1000 / 10000 / 1000 | the share limit's period, the longest the CPU is given away at a time |
| `cap_prio` | RWTUN | class priority + 1 .. 255 | 224 (`PRI_MIN_IDLE`) | the priority the worker waits at between passes while over its allowance, never the priority of a pass.  224 runs only when the CPU would otherwise idle and is preempted by anything that wakes; 223 (`PRI_MAX_TIMESHARE`) is the other useful value, the last batch slot, which ULE does not treat as idle when placing a waking thread.  The rest of the range is for experiments (SCHED.md S6.3) |
| `tick_us` | RWTUN | 0 .. 100000 | 0 (off) | the tick regime's bound `L` (KWQ.md S16, opt-in): while a worker's producer is dense its sleep has a timeout of `tick_us`, producers append without ringing the doorbell, and the timeout serves what accumulated.  0 is the doorbell regime, one wake per backlog |
| `tick_urgent_us` | RWTUN | 0 .. 100000 | 0 (at once) | inside the tick regime, an urgent item (`kwq_enqueue_urgent()`) that finds the worker asleep wakes it this much later instead of at once, so that urgent items within the bound share one wake (`tick_urgent_deferred`, `tick_urgent_wakes`); a bound at or above `tick_us` is the tick's own and arms nothing; outside the regime, and for a notifier, a drain or a cancel, nothing changes (WAKE.md S12.5, S12.8) |
| `wake_delay_us` | RWTUN | 0 .. 1000000 | 0 (off) | the idle-class wait's deadline (KWQ.md S16, opt-in): an enqueue from the worker's own CPU makes the worker runnable in the idle class at `cap_prio` instead of preempting the producer, and it runs when the producer sleeps or at this deadline, whichever is first.  0 is the preempting doorbell.  An enqueue from another CPU rings the doorbell at either setting |
| `yield_prio` | RW | class priority .. 223 | 56 (`PUSER`) | priority of the end-of-round yield.  223 lets every user thread run a full slice, which is the setting for the case where an interactive process turns CPU-bound and its latency spikes with `yields` normal and no kwq counter explaining them.  The one knob that is not a loader tunable |
| `steal` | RWTUN | 0, 1 | bulk: 1 | 1: a BULK worker with nothing of its own takes batches of stealable queues before it sleeps, and producers wake sleeping workers to do so (under `place=1` the busy worker hands batches off instead, with no steal wake); 0: no stealing, and a stealable pass takes its whole list again (SCHED.md S16) |
| `steal_scope` | RWTUN | 0 .. 8 | bulk: 8 | rings of the cache topology that a steal, the steal wake, `kwq_scatter()` and, under `place=0`, the `KWQ_CPU_ANY` pick look at, nearest first (P8's pick reads its group lines and the far probe first and is bounded by this reach only in its last two steps, the nearest non-donor and the shallowest of four; gossip's pick reads its tables); 1 is the CPUs sharing the smallest group with this one (the SMT sibling on an SMT machine); 0 keeps every item on its CPU |
| `steal_scan` | RWTUN | 1 .. 4096 | bulk: 16 | other CPUs probed at most per steal attempt, steal wake, scatter or, under `place=0`, pick |
| `steal_max` | RWTUN | 1 .. 4096 | bulk: 64 | items one steal takes at most; otherwise half the victim's live list, the victim's lock held while the stealer walks them |
| `steal_batch` | RWTUN | 1 .. 4096 | bulk: 16 | items a pass of a stealable queue takes at most; one while a BULK worker near it sleeps, which it then wakes for the rest |
| `steal_wake` | RWTUN | 0 .. 4096 | bulk: 2 | an enqueue that leaves a stealable list this deep, or at a doubling of it, wakes the nearest sleeping BULK worker; 0: never |
| `nstealable` | RD | - | - | bulk: stealable queues in existence |
| `place` | RWTUN | 0, 1, 2 | bulk: 2 on one NUMA domain, else 0 | BULK placement and stealing, flippable under load (SCHED.md S17): 0 today's code (S16); 1 the gossip scheme, the hand-off at a pass end in place of stealing; 2 P8, the idle mask in group lines, a donor line in place of the idle path's scan, the starvation watchdog, the mailbox and the chain |
| `shard.probe` | RW | 0 .. 4 | 2 | place=2: groups a producer reads besides its own before the far probe |
| `shard.donor_us` | RW | 0 .. 1000000 | 500 | place=2: the stealable backlog, us, that makes a CPU a donor |
| `shard.pull_us` | RW | 0 .. 1000000 | 500 | place=2: a worker below this backlog at a pass end pulls from a donor; 0: never |
| `shard.watch_us` | RW | 0 .. 1000000 | 1000 | place=2: the starvation watchdog's period; 0: off |
| `shard.directed` | RW | 0 .. 2 | 2 | place=2: 1 a wake to steal names its victim in the woken worker's mailbox; 2 also the chain, an idle thief retries the victim of its last batch |
| `shard.directed_us` | RW | 0 .. 65535 | 10 | place=2: the mailbox and the chain only for lists whose items cost at least this many us; 0: every list |
| `shard.group`, `shard.ngroups` | RD | - | 16 (loader tunable `kern.kwq.bulk.group`, 1 .. 64) | CPUs per idle line, and the lines |
| `gossip.h_us`, `gossip.l_us`, `gossip.llocal_us`, `gossip.handoff_n` | RW | - | 2000, 500, 50, 4 | place=1: the hand-off and spill backlog H, the most a hand-off's target may hold, the producer's own thread's bound, hand-offs per pass end (SPEC-p6g.md S2.9) |
| `priority` | RDTUN | 0 .. 55 (net, bulk), 0 .. 223 (blocking), net <= bulk <= blocking | 1 (net), 2 (bulk), 56 (blocking) | the class's worker priority, settable only before the module loads (loader.conf or kenv(1)); a setting outside the range or out of order resets all three to the defaults with a console message.  Every class knob exists for `blocking` too (BULK's `steal*`, `place`, `shard.*` and `gossip.*` only under `bulk`), although the class has no workers |

Service-wide (`kern.kwq.`): `clock` (RD) names the scheduler's tick source
and its rate, `tsc`, `cntvct`, `timebase`, or the kernel's `cpu_ticks` as
the fallback; `clock_fast` (RDTUN, default 1) set to 0 at load keeps
`cpu_ticks()`, for A/B measurements (SCHED.md S15.2).

The anti-gaming grace window is not a knob: it is `GRACE_SERVICE`, one
ring's worth of service, `quantum_us` times the ring length read at the
doorbell, counted on a per-worker service counter that stands still
while the worker sleeps and carries the sub-microsecond rest, so passes
shorter than a microsecond count too.  The compile-time `GRACE_ROUNDS`
of one round that it replaced on 2026-10-28 is gone, because the share
limit's budget cut ends passes early and a round then no longer stands
for one quantum of service per competitor (SCHED.md S4.6, S8).

### The two optional wake regimes

Both are class knobs, both off by default, neither part of the KPI: a
client cannot ask for them and cannot refuse them except item by item
with `kwq_enqueue_urgent()`, and both are sized so that an isolated
item still pays only one sleep-wake (B5a).  Specified 2026-10-28
(KWQ.md S16), measured on branch `kwq-wake-tick` and `kwq-wake-idle`
(WAKE.md S9, S10), implemented on branch `kwq-p4f` and refined on
`kwq-p4g` (WAKE.md S11, S12): both are in the merged module.

**`tick_us`, the tick regime.**  Range 0 to 100000 us, default 0, off.
A worker enters the regime from a doorbell wake once its recent wakes
were denser than `tick_us` over more than one burst (three consecutive
sleeps shorter than `tick_us`), not from a single dense pair of wakes.
Inside it the sleep has a timeout of `tick_us` with precision
`tick_us/8`, a producer that places a list counts `doorbells_skipped`
and wakes nothing, and the timeout runs one round over what
accumulated.  It leaves on a tick that served one element or none,
back to the untimed sleep, so a silence costs one timer interrupt and
nothing stays armed; an episode in which no tick's round served more
than one element doubles an entry hold-off from `2 x tick_us` to
`1024 x tick_us`, one in which a tick's round did resets it.
*Contract*: an item that arrives while the regime is on waits at most
`tick_us` plus the pass in progress; sleeps per second within 20 % of
the interval-stabbing bound where the arrival rate times `tick_us` is
2 to 10, equal to the doorbell's below 1, drain-limited above 20.
*Cost*: the producer pays only the append (46 to 54 ns on the smaller
hardware test machine, 186 to 210 ns on the 128-core Ampere, against
200 to 1100 ns with the wake); the worker pays one timer interrupt per
tick; every
co-scheduled small flow on that worker pays a part of `tick_us`, more
the more often the worker sleeps in the regime.  *When to use*: a
dense cross-CPU producer on a worker that is busy but not saturated,
where the wake is dear, at 20 us; never as a default.  Measured on the
128-core Ampere (WAKE.md S12.5): 20 us frees 10 points of the worker's
CPU on a single flow at an unchanged rate and halves the tail of a
burst of 64 small requests, and costs a request-reply flow on the same
worker 8 us at p50 and 11 at p99 (50 us: 16 and 40); on a worker the
bulk flow saturates it sleeps a few hundred times a second either way,
so there is little to gain and 20 us costs nothing measurable.  For a
latency-sensitive flow sharing the worker, see `kwq_enqueue_urgent()`
and its limits, and `tick_urgent_us` (0 to 100000 us, default 0, at
once), the urgent items' own bound inside the regime.  Do not judge
the knob on a single saturated flow's throughput: that row is bimodal
per run on both hardware machines with or without it (WAKE.md S12.2).

**`wake_delay_us`, the idle-class wait for same-CPU producers.**
Range 0 to 1000000 us, default 0, off.  An enqueue from the worker's
own CPU does not preempt the producer: the worker is made runnable in
the idle class at `cap_prio` and runs when the producer sleeps or at
the deadline, whichever is first, raised by the deadline callout; an
urgent ring or the share limit's deadline raises it at once.  An
enqueue from another CPU rings the doorbell as before, because the
cost of making a cross-CPU wake wait falls on the sparse sender (+77 %
enqueue on the 128-core Ampere).  *Contract*: an
item from the worker's CPU waits at most the deadline.  *Cost*: one
callout arm per wait episode on the worker's own CPU, and the deadline
itself for any item whose producer does not sleep.  *When to use*: a
receiver or producer pinned to its worker's CPU, where the doorbell
otherwise preempts the producer once per item; a same-CPU burst is then
served once, when its producer is done (+18 to +69 % measured), with
the idle latency and the fairness columns unchanged.  *Value*: 100 us.
On a co-located bulk receiver the deadline, not the producer, ends 98
to 99.8 % of the episodes, so the knob is the batch size; 50 us gave
back 30 to 65 % of the gain on the three test machines and improved no
latency row (WAKE.md S12.4).  `cap_prio`'s default, 224, is one
decision for both this wait and the share limit's wait between passes
(SCHED.md S6.3, WAKE.md S12.3).

**Both knobs at once.**  A same-CPU producer can satisfy both regimes,
and the doorbell decides the wait first: the worker becomes runnable
now and runs when that producer sleeps, which is sooner than the next
tick and costs no timer interrupt.  Cross-CPU producers still take the
tick's skip.  The reverse order, which the first build had, made the
two mutually exclusive and let the machine decide which one a
co-located receiver got (WAKE.md S12.1).

### Per-worker counters, `kern.kwq.<class>.cpu.<N>.`

| node | meaning |
|---|---|
| `rounds` | DRR rounds completed |
| `passes` | handler invocations |
| `wakeups` | times the worker was woken from an idle sleep; a sleep that ended by its timeout counts in `ticks` instead |
| `yields` | end-of-round yields that had a taker |
| `tick_yields` | yields forced because a pass outlived a clock tick |
| `handbacks` | rounds ended early because a higher class wanted the CPU |
| `switched` | passes during which the worker left the CPU (preempted or blocked); NET charges them wall time |
| `cap_demotions` | periods in which the share limit was reached with other threads runnable |
| `cap_yields` | hand-overs at pass end under the share limit |
| `cap_cuts` | budget calls answered 0 by the share limit, cutting a pass short (SCHED.md S6.3) |
| `cap_restores` | period deadlines that raised a waiting worker back to its class priority |
| `cap_ns` | wall time given away under the share limit |
| `cap_runon` | passes that ended over the allowance with nothing else runnable: the limit did not bind |
| `slice_yields` | yields at ULE's own priority for the worker because ULE ended its slice; limit off only (SCHED.md S6.3) |
| `prio_violations` | passes that began below the class priority or outside its scheduling class: the tripwire of the priority promise, 0 on a conforming build; a module built with `KWQ_HARD_INVARIANTS` asserts it as well, a panic on an INVARIANTS kernel (KWQ.md S3) |
| `ticks` | tick-regime sleeps that ended by their timeout (`tick_us` > 0).  A park in the idle class that outlasts the timeout returns as one too and is counted and judged as a tick, not in `wakeups` (WAKE.md S12.1) |
| `ticks_empty` | of those, the ones that found nothing queued |
| `ticks_single` | of those, the ones that served one element: the regime's exit condition |
| `tick_enters`, `tick_exits` | regime episodes entered and left |
| `tick_ns` | wall time spent in the tick regime |
| `tick_barren` | episodes in which no tick's round served more than one element (the round that entered the regime does not count), each doubling the entry hold-off |
| `wakes_low` | wakes placed in the idle class at `cap_prio` instead of preempting a same-CPU producer (`wake_delay_us` > 0): one per wait episode |
| `wake_low_ns` | total length of those wait episodes; divided by `wakes_low`, the mean wait |
| `wake_self` | wait episodes the worker ended by getting the CPU on its own, the producer having slept |
| `wake_promotions_deadline` | wait episodes the deadline callout ended |
| `wake_promotions_urgent` | wait episodes cut short by an urgent ring or nudge (`kwq_enqueue_urgent()`, a notifier, a drain, a cancel), an enqueue from another CPU, the share limit's deadline or an unload |
| `wakes_urgent` | wakes of a sleeping worker by an urgent item or a notifier added to a list already placed with it (the urgent nudge), counted in the queue's `doorbells` too |
| `tick_urgent_deferred` | urgent items that found the worker asleep in the tick regime and waited up to `tick_urgent_us` instead of waking it |
| `tick_urgent_wakes` | wakes by the `tick_urgent_us` bound, one per window of deferred urgent items |
| `busy_ns` | time charged to passes: CPU time, wall time for NET |
| `idle_ns` | wall time asleep with nothing queued |
| `round` | current round number (gauge) |
| `nactive` | entries on the DRR ring now (gauge) |
| `nnew` | entries on the new list now (gauge) |
| `budget_calls` | `kwq_budget_left()` calls on this worker |
| `budget_reads` | of those, the ones that read the clock |
| `steal_tries` | bulk: steal attempts, with nothing of its own to do and under place=2 the pulls at a pass end; a mailbox or chain victim tried first counts one more |
| `steals` | bulk: of those, the ones that moved a batch to this CPU |
| `stolen` | bulk: items in those batches |
| `steal_wakes` | bulk: wakes sent to this worker so that it steals |
| `pulls`, `donor_flags`, `starved` | bulk, place=2: batches pulled from a donor at a pass end; times this CPU became a donor, the starvation watchdog's flag included; times the starvation watchdog flagged it |
| `directed`, `chained` | bulk, place=2: batches taken from the victim a wake named; from the victim of the last batch again |
| `handoffs` | bulk, place=1: batches this worker handed off at a pass end |

### Per-queue nodes, `kern.kwq.<class>.queue.<queue>.`

| node | access | meaning |
|---|---|---|
| `weight` | RD | DRR weight as created, 1 to 8 |
| `limit` | **RW** | per-CPU item bound in effect, writable 1 to `INT_MAX`, so a depth can be tuned or swept without recreating the queue (`2147483647` for `KWQ_LIMIT_NONE`) |
| `flags` | RD | `KWQ_F_*` as created |
| `state` | RD | `inactive`, `active`, `draining`, `drained` |
| `reset` | WR | write 1 to zero `maxlat_ns` and `maxdepth` on every CPU |

### Per-queue, per-CPU counters, `....queue.<queue>.cpu.<N>.`

| node | meaning |
|---|---|
| `depth` | elements queued now, pending signals included (gauge) |
| `state` | 0 idle, 1 waking, 2 running, 3 parked (gauge) |
| `items` | elements handed to the handler, notifiers included |
| `signals` | of those, notifiers, counted as the handler consumes them |
| `passes` | handler invocations for this queue on this CPU |
| `cycles` | wall-clock `cpu_ticks()` spent in those passes, not CPU time: under interrupt load it exceeds the worker's `busy_ns` share, which is what makes it useful for latency diagnosis |
| `rejected` | enqueues refused: `ENOBUFS` at the bound, `ENXIO` after drain began |
| `coalesced` | `kwq_notify()` calls that found the notifier pending |
| `requeued` | items handed back with `kwq_requeue()` |
| `maxdepth` | high-water mark of `depth` at pass start since `reset` |
| `maxlat_ns` | longest doorbell-to-first-pass latency since `reset` |
| `overruns` | passes that exceeded budget by more than one quantum x weight.  A handler whose single item costs more than a quantum overruns however carefully it checks, because of the progress rule: such a client wants a class with a larger quantum, or smaller items |
| `overruns_switched` | of those, the passes during which the worker left the CPU: `overruns - overruns_switched` is the handler's own excess, the remainder the system's (SCHED.md S6.3) |
| `doorbells` | wakes performed for this (queue, CPU): doorbells that found the worker asleep, idle-class parks and urgent nudges; a ring that finds the worker awake is not counted |
| `doorbells_skipped` | placements of this list with the worker that woke nothing because the worker slept in the tick regime (`tick_us` > 0), urgent items held back by `tick_urgent_us` included; appends to a list already placed count in neither column |
| `parks` | rounds the queue was skipped for owing time |
| `boosts` | passes served from the new list |
| `grace` | doorbells sent to the ring instead of the new list by the anti-gaming rule, the queue having returned before one ring's worth of service went to the others (SCHED.md S4.6) |
| `debts` | doorbells sent to the ring because the queue went idle owing time |
| `switched` | passes of this queue during which the worker left the CPU (preempted or blocked); NET charges them wall time |
| `glitches` | passes whose CPU-time delta was negative or over 1 s |
| `steals_in`, `stolen_in` | bulk: batches and items this CPU's worker took from other CPUs' lists of the queue |
| `steals_out`, `stolen_out` | bulk: batches and items other CPUs' workers took from this list.  Summed over the CPUs, in equals out |
| `scattered` | bulk: items `kwq_scatter()` put on this list |
| `cost_ns` | bulk: an item's cost on this list, ns, averaged over passes (1/8 per pass), glitched passes left out; kept under place=2 for stealable queues, under place=1 for every BULK queue |

Counters are 64-bit and never reset except through `reset`.  A non-zero
`glitches` means this machine's `cpu_ticks()` is not monotonic and every
CPU-time figure on it is suspect.

## 12. DTrace

Provider `kwq`; `dtrace -l -P kwq` lists the probes.  Queue probes carry
a translated `kwqinfo_t` as `args[0]` with the members `kwq_name`,
`kwq_class`, `kwq_weight`, `kwq_limit`, `kwq_flags` and `kwq_addr`.
`kwq_class` renders as the string `"net"`, `"bulk"` or `"blocking"`;
`kwq_addr` is the queue's kernel address, for correlating a probe with
`lockstat` output or an `fbt` stack.  The
translator is installed as `/usr/lib/dtrace/kwq.d`; an uninstalled tree
works with `-L path/to/kwq/kwq`.  It needs the module's CTF, which the
default build produces.

| probe | fires | arguments after `args[0]` |
|---|---|---|
| `create`, `activate`, `drain-start`, `drain-end`, `destroy` | lifecycle | none |
| `enqueue` | each accepted `kwq_enqueue()` and `kwq_enqueue_urgent()`; once per `kwq_enqueue_list()` | `arg1` cpu, `arg2` depth after, `arg3` 1 if it rang the doorbell, whatever the doorbell then did, or its urgent nudge woke the worker |
| `reject` | enqueue or notify refused with `ENOBUFS` or `ENXIO`.  **Not** for `EINVAL`, which is detected before the list lock and fires nothing | `arg1` cpu, `arg2` errno |
| `pass-start` | list swapped, handler about to run | `arg1` cpu, `arg2` items, `arg3` doorbell-to-pass ns (0 on later passes of one backlog) |
| `pass-end` | handler returned | `arg1` cpu, `arg2` items, `arg3` ns charged (CPU time, NET wall time), `arg4` items requeued |
| `overrun` | a pass exceeded its budget by more than one quantum x weight | `arg1` cpu, `arg2` ns over |
| `park` | a queue skipped for owing time | `arg1` cpu, `arg2` deficit ns, zero or negative |
| `budget-hit` | `kwq_budget_left()` answered 0 | `arg1` cpu |
| `round-end` | worker finished a round | untranslated: `arg0` class, `arg1` cpu, `arg2` ring entries, `arg3` wall ns since the last round end |
| `yield` | worker yielded | untranslated: `arg0` class, `arg1` cpu, `arg2` 0 round end, 1 tick guard, 2 share limit, 3 slice (ULE ended the slice, limit off) |
| `cap` | the share limit (SCHED.md S6.3) | untranslated: `arg0` class, `arg1` cpu, `arg2` event (0 a pass begins under the limit, 1 allowance spent and CPU given away, 2 given away again, 3 period ended while over, 4 deadline callout ran), `arg3` ns into the period, `arg4` ns used of it, `arg5` the pass's budget ns (event 0) |
| `idle` | worker about to sleep | untranslated: `arg0` class, `arg1` cpu, `arg2` busy ns since it woke |
| `doorbell` | a (queue, CPU) placed with its worker, or an urgent nudge that woke the worker | untranslated: `arg0` class, `arg1` cpu, `arg2` worker state when rung: 0 awake (a wait in the idle class it ends included), 2 asleep and woken, 4 asleep and made runnable in the idle class (`KWQ_DB_LOWPRI`), 6 asleep with the tick armed: placed, not woken (`KWQ_DB_TICKED`); 1 (polling) is defined and never fires |
| `tick` | a tick-regime sleep ended by its timeout (`tick_us` > 0, KWQ.md S16), as counted in `ticks` | untranslated: `arg0` class, `arg1` cpu, `arg2` elements served since the worker's previous sleep (the previous tick's round, or at an episode's first tick the round that entered the regime) |
| `wake` | a worker waiting in the idle class promoted to the class priority (`wake_delay_us` > 0) | untranslated: `arg0` class, `arg1` cpu, `arg2` reason (`KWQ_WAKE_DEADLINE` 2, `KWQ_WAKE_URGENT` 4: an urgent ring or nudge, an enqueue from another CPU, the share limit's deadline, an unload; `KWQ_WAKE_SELF` 5: the worker got the CPU), `arg3` ns since the wait began |
| `steal` | a batch moved between two CPUs' lists of a stealable queue, by any path | `arg1` from cpu (a hand-off: the handing CPU), `arg2` to cpu, `arg3` items, `arg4` how (`KWQ_STEAL_SCAN` 0, `DONOR` 1, `MAILBOX` 2, `CHAIN` 3, `PULL` 4, `HANDOFF` 5), `arg5` items left on the list it came from |
| `steal-wake` | a sleeping BULK worker woken to steal (place 0 and 2) | untranslated: `arg0` the CPU to steal from, `arg1` the woken CPU or -1 (none in reach slept), `arg2` why (`KWQ_SWAKE_ENQUEUE` 0, `HELPER` 1, `DONOR` 2, `WATCHDOG` 3), `arg3` 1 if the mailbox was filled |
| `steal-miss` | a steal attempt that moved nothing | untranslated: `arg0` thief cpu, `arg1` how (as `steal`), `arg2` victim cpu or -1, `arg3` why (`KWQ_SMISS_NONE` 0, `STALE` 1, `EMPTY` 2, `LONE` 3, `INACTIVE` 4) |
| `donor` | place=2: a CPU's donor or starved state changed | untranslated: `arg0` cpu, `arg1` donor, `arg2` starved, `arg3` stealable backlog ns |
| `place` | a `KWQ_CPU_ANY` item placed or a fixed CPU's item spilled | `arg1` producer cpu, `arg2` chosen cpu, `arg3` step (`KWQ_PICK_TODAY` 0; P8 `NEAR` 1, `FAR` 2, `SELF` 3, `NONDONOR` 4, `PROBE` 5; gossip 16 + `KP_STEP_*`), `arg4` 1 for a spill |
| `worker-block`, `worker-spawn`, `worker-exit` | defined, never fire yet (P7) | untranslated: `arg0` cpu, `arg1` workers, `arg2` runnable |

`round-end`, `yield`, `cap`, `idle`, `doorbell`, `tick` and `wake` are
worker probes rather than queue probes: they carry no `kwqinfo_t`, so
their arguments start at `arg0` with the class id; `steal-wake`,
`steal-miss` and `donor` carry none either and start with a CPU.  The
codes are D inlines in kwq.d.  Twenty-seven probes in all.

Measured cost on a 128-core Neoverse-N1: probes compiled in but disabled
cost 0.6 % of a saturated worker's throughput; an enabled probe costs
about 140 ns per firing with a plain aggregation and 180 ns with the
translator and a string key.  Pass-level probes fire a few times per
pass and are safe to leave running; `enqueue` fires per item and costs
more than kwq's own per-item work.

## 13. DDB and other tools

```
db> show kwq
```

Prints every queue with its class, state, weight, limit and flags, then
one line per CPU it has been used on with that list's state, depth,
pending notifiers, deficit, passes, items and overruns (a queue never
used on any CPU prints "(idle on every CPU)"); then every worker with
`asleep`, `in round` or `between rounds`, its round number, its ring and
new-list lengths, its pass count and the queue it is serving.

Other providers see kwq without any kwq-specific support:

| tool | what it shows |
|---|---|
| `top -H`, `ps -H` | the worker threads by name, `kwq_net/<N>` |
| `lockstat` | per-queue list locks by name, `kwq <queue>` |
| `dtrace -n 'profile-997 /execname == "kernel"/ ...'` | where a worker's CPU time goes |
| `fbt::kwq_enqueue:return` | enqueue errors with the producer's stack |
| `sched:::on-cpu`, `off-cpu` | worker scheduling, keyed by `curthread->td_name` |

## 14. Build and install

```sh
make -C kwq                 # kwq.ko, kwq_test.ko and kwq_crypto.ko
make -C kwq install         # modules to /boot/modules, kwq.d to /usr/lib/dtrace
make -C kwq uninstall       # removes them and refreshes linker.hints
```

Defaults of the module build: `WITH_CTF=1` so the DTrace translator
works, `DEBUG_FLAGS=-g` for symbols, `INSTALL_NODEBUG` set so no
separate debug file is installed, and SDT probe sites compiled in.
`make KWQ_NO_SDT=1` compiles every probe site out, which is how the
disabled-probe cost was measured.  `DTRACEDIR` overrides where `kwq.d`
goes.

A client builds against the header with
`CFLAGS+= -I${.CURDIR}/path/to/kwq/kwq` and declares
`MODULE_DEPEND(client, kwq, 1, 1, 1)`.  `kwq.ko` must be loaded first,
or installed so the dependency resolves.

## 15. Implementation status

| feature | status |
|---|---|
| NET and BULK classes, one worker per CPU | implemented, measured |
| create, activate, enqueue, enqueue_list, drain, destroy | implemented |
| notifiers with coalescing and cancel | implemented |
| DRR in CPU time: quantum, weights, carry, parking, penalty cap | implemented |
| new list with boost, grace rule, debt rule, progress rule | implemented |
| estimated budget check, clock glitch guard | implemented |
| yield at round end and tick, hand-back between classes, share limit | implemented |
| sysctl tree, 27 DTrace probes, translator, `show kwq` | implemented |
| writable per-queue `limit` | implemented |
| `kwq_cpu_for_hash()` remap for sparse CPU ids | implemented; the sparse path itself is **untested**, no such machine being available |
| notifiers on the item list in arrival order, budgeted, requeueable (tagged link, 2026-10-08) | implemented, tested by the eight `mix` cases |
| `KWQ_F_STEALABLE`: stealing, the pass cap, the steal wake (P6, SCHED.md S16) | implemented; the `steal`, `steal_off`, `stealdrain` and `stealdrain_discard` cases |
| `kwq_scatter()` with `struct kwq_group` (P6) | implemented; the `scatter` case |
| `KWQ_BLOCKING` | refused at create with a log line; no workers of that class exist (P7) |
| `KWQ_F_RESERVE`, `nreserve != 0` | asserted at create, not cleanly refused (P7) |
| cache-topology `KWQ_CPU_ANY`, an idle worker first (P6) | implemented |
| BULK placement `kern.kwq.bulk.place`: P8 with the chain, the default on one NUMA domain, and gossip (P6g, SCHED.md S17) | implemented; the `place_switch`, `place_stale`, `torus`, `island` and `starved` cases; measured on lab and a07 (BULK.md S8) |
| `KWQ_F_PROMPT` queues; items that stop part way, `kwq_item_begin()`, `kwq_item_stop()` and `item_min_us` (SCHED.md S4.8, S4.9) | implemented; no kwq_test case, exercised by kwq-geli's `read_queue` and `item_min` cases (`../../kwq-geli/tests`) |
| grace rule in service, `GRACE_SERVICE` one ring's worth (replaces `GRACE_ROUNDS`) | implemented |
| tick regime (`tick_us`), its counters and the `tick` probe | implemented, off by default |
| idle-class wait for same-CPU producers (`wake_delay_us`), its counters and the `wake` probe; it takes precedence over the tick regime for a same-CPU producer | implemented, off by default |
| `kwq_enqueue_urgent()`, the urgent nudge and `wakes_urgent`; if_pair's `net.link.pair.urgent_bytes`, which never marks a pure TCP ACK or window update | implemented; `urgent_bytes` 0 (off) by default |
| `tick_urgent_us`, `tick_urgent_deferred`, `tick_urgent_wakes` | implemented, off by default |
| priority assertion and `prio_violations`, `overruns_switched` | implemented; counted by default, asserted with `KWQ_HARD_INVARIANTS` |
| split charging of a NET pass | candidate only, pending a battery that produces interrupt threads or lock blocks (SCHED.md S6.3) |
| `kwq(9)` and `dtrace_kwq(4)` man pages | P8 |

Clients: `if_pair(4)` runs on kwq and is measured against a frozen copy
of its taskqueue predecessor (`../../extras/if_pair_tq`).  epair is P5.
The BULK proof (P6) is `../../kwq-geli`, GELI ported to
BULK queues and measured against the stock module, with
`kwq_crypto.ko`, a dummy BULK client benchmarking AES-XTS, beside it.
