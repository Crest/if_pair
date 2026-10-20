# kwq(9) reference

The complete surfaces of the kwq kernel work queue service: the KPI, the
types and flags, the error and context rules, the sysctl tree, the
DTrace provider, the DDB command and the build.  Names, ranges and
defaults are exact as of 2026-10-01 and were read from the code, not
from the design documents.  Where this and `../KWQ.md` or `../SCHED.md`
differ, those are normative.

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
| `KWQ_F_STEALABLE` | 0x02 | BULK only: idle workers may take batches (accepted, inert until P6); no `kwq_notify()` on such a queue |
| `KWQ_F_DISCARD` | 0x04 | `kwq_drain()` hands the backlog to the handler with `n < 0` instead of running it |
| `KWQ_F_RESERVE` | 0x08 | BLOCKING only; asserted at create today |
| `KWQ_F_VNET` | 0x10 | the worker sets the creator's vnet around the handler |
| `KWQ_F_SPIN` | 0x20 | per-CPU list locks are spin mutexes: producers may be interrupt filters or hold spin locks |
| `KWQ_F_ALL` | 0x3f | the mask of valid flags |

| constant | value | meaning |
|---|---|---|
| `KWQ_CPU_ANY` | -1 | BULK only: kwq takes the caller's CPU if that list is empty, otherwise the shallowest of at most four other CPUs probed in the caller's NUMA domain.  Bounded on purpose: scanning a whole 128-CPU domain would read 128 cold cache lines per enqueue |
| `KWQ_NAMELEN` | 32 | buffer size; a name is 1 to 31 characters |
| `KWQ_LIMIT_NONE` | `UINT_MAX` | unbounded; the effective bound is `INT_MAX` so a batch's `n` fits an `int` |

## 4. Functions

### kwq_name_valid

```c
bool kwq_name_valid(const char *name);
```

True if `name` is 1 to 31 characters, each from `[A-Za-z0-9_-]`.

**Not exported to modules:** it is declared in the header but absent
from the module's `EXPORT_SYMS`, so a client cannot link it today.  A
client that derives a queue name from user-controlled text must apply
the rule itself, or rely on `kwq_create()` refusing the name with a line
in the kernel log.  (Either the export list or the declaration should
change; the declaration is the useful half.)

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

### kwq_scatter

```c
int kwq_scatter(struct kwq *q, struct kwq_item *list, int n,
    void (*done)(void *), void *arg);
```

Fan a list of independent items across the CPUs of a BULK queue and call
`done(arg)` once when all have been processed.  Returns `EOPNOTSUPP`
today; specified for P6.

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
Returns 0 in exactly three cases: a higher class is waiting for this
CPU, the budget is spent, or the clock read looked impossible (negative,
or over one second).  A positive value is an *estimate*: the clock is
read at least every `budget_check_every` calls, and early whenever the
estimate comes within a tenth of a quantum of the end, so the answer to
"is there time for another item" is reliable near the boundary while the
number itself may be a few microseconds out mid-pass.  Never an error.

The tick guard that bounds a worker's CPU occupancy is *not* visible
here: it runs between passes, in the worker, and does not affect what a
handler's budget query returns.  See the cadence guidance in
[cheatsheet.md](cheatsheet.md) section 5.

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

## 6. Errors

| call | error | cause and what the caller does |
|---|---|---|
| `kwq_create` | `NULL` | **logged, and a legitimate runtime failure:** an invalid name, a name already used in the class, or `KWQ_BLOCKING`.  **Asserted, i.e. a programming error that panics under INVARIANTS and returns `NULL` otherwise:** a NULL name or handler, a bad class, undefined flags, `KWQ_F_RESERVE`, `KWQ_F_STEALABLE` on a non-BULK queue, `weight > 8`, `nreserve != 0`.  No `limit` value is refused: 0 means the class default and anything above `INT_MAX` is clamped |
| `kwq_enqueue`, `kwq_enqueue_list` | `ENOBUFS` | the chosen CPU's list holds `limit` items.  Item still the caller's: drop and count, or apply upstream backpressure |
| | `ENXIO` | `kwq_drain()` has begun, or the queue is still `KWQ_F_INACTIVE` |
| | `EINVAL` | `KWQ_CPU_ANY` on a non-BULK queue |
| `kwq_scatter` | `EOPNOTSUPP` | not implemented before P6 |
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
| interrupt filter, or holding a spin mutex, or inside `critical_enter()` | the same calls, **on a `KWQ_F_SPIN` queue only** | the same calls on a sleep-mutex queue (WITNESS panics); everything else |
| NMI, `SCHEDULER_STOPPED()`, KDB | nothing | everything.  After a panic the queue mutexes degrade to no-ops, so a `kwq_drain()` on the shutdown path needs no special case, but no new work may be submitted |

Client locks may be held across `kwq_enqueue()`: the per-CPU list locks
are leaves.

## 8. What a handler may do

| operation | NET / BULK | notes |
|---|---|---|
| `kwq_enqueue` to another queue, or this one | yes | to this queue it is new work, behind what is already queued |
| `kwq_notify` to any queue, including its own notifier | yes | the way to say "still not empty" |
| `kwq_requeue` the pass's remainder | yes | prepends, FIFO kept, notifiers among it; never an element whose successor the handler has already fetched |
| `kwq_budget_left` | yes | 0 means requeue and return |
| sleep mutex, rwlock, rmlock | yes, briefly | |
| spin mutex, `critical_enter`, `sched_pin` | yes, release before return | while held, only `KWQ_F_SPIN` queues may be enqueued to |
| `M_NOWAIT` allocation | yes, handle `NULL` | |
| `kwq_scatter` to a BULK queue | yes | the completion callback runs later, on some BULK worker |
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
| `quantum_us` default | 200 | 1000 | 5000 |
| `limit` default | 4096 | 4096 | 4096 |
| stealing | never | `KWQ_F_STEALABLE` (P6) | n/a |
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

### Class knobs

All per class.  RWTUN nodes are also read from `loader.conf` when the
module is preloaded.  Out-of-range values are refused with `EINVAL`.

| node | flags | range | default net / bulk / blocking | effect |
|---|---|---|---|---|
| `quantum_us` | RWTUN | 10 .. 1000000 | 200 / 1000 / 5000 | CPU time a weight-1 queue may use per round.  Larger: fewer hand-offs per item, longer worst-case wait for others |
| `limit` | RWTUN | any | 4096 | default per-CPU bound for queues created with `limit = 0`; applies to queues created afterwards |
| `penalty_rounds` | RWTUN | 1 .. 1024 | 32 | how many quanta of debt a queue may accumulate, i.e. the longest parking |
| `budget_check_every` | RWTUN | 1 .. 64 | 8 | how often `kwq_budget_left()` reads the clock; 1 is exact and expensive |
| `cap_permille` | RWTUN | 1 .. 1000 | 500 | share limit: the share of its CPU the worker keeps at kernel priority per period while other threads are runnable, in 1/1000; the rest of the period it is an idle-priority thread.  Equal share by default, 1000 = off (SCHED.md S6.3) |
| `cap_period_us` | RWTUN | 100 .. 1000000 | 1000 | the share limit's period, the longest the CPU is given away at a time |
| `yield_prio` | RW | class priority .. 223 | 56 (`PUSER`) | priority of the end-of-round yield.  223 lets every user thread run a full slice, which is the setting for the case where an interactive process turns CPU-bound and its latency spikes with `yields` normal and no kwq counter explaining them.  The one knob that is not a loader tunable |
| `priority` | RDTUN | 0 .. 223, net <= bulk <= blocking | 1 (net), 2 (bulk), 56 (blocking) | the class's worker priority, settable only before the module loads (loader.conf or kenv(1)); a setting outside the range or out of order resets all three to the defaults with a console message.  Every knob exists for `blocking` too, although the class has no workers |

Service-wide (`kern.kwq.`): `clock` (RD) names the scheduler's tick source
and its rate, `tsc`, `cntvct`, `timebase`, or the kernel's `cpu_ticks` as
the fallback; `clock_fast` (RDTUN, default 1) set to 0 at load keeps
`cpu_ticks()`, for A/B measurements (SCHED.md S15.2).

The anti-gaming grace window (one round) is compile time.

### Per-worker counters, `kern.kwq.<class>.cpu.<N>.`

| node | meaning |
|---|---|
| `rounds` | DRR rounds completed |
| `passes` | handler invocations |
| `wakeups` | times the worker was woken from an idle sleep |
| `yields` | end-of-round yields that had a taker |
| `tick_yields` | yields forced because a pass outlived a clock tick |
| `handbacks` | rounds ended early because a higher class wanted the CPU |
| `switched` | passes during which the worker left the CPU (preempted or blocked); NET charges them wall time |
| `cap_demotions` | periods in which the share limit was reached with other threads runnable |
| `cap_yields` | times the worker gave the CPU away under the share limit |
| `cap_restores` | period deadlines that raised a waiting worker back to its class priority |
| `cap_ns` | wall time given away under the share limit |
| `cap_runon` | passes that ended over the allowance with nothing else runnable: the limit did not bind |
| `busy_ns` | CPU time inside handlers |
| `idle_ns` | wall time asleep with nothing queued |
| `round` | current round number (gauge) |
| `nactive` | entries on the DRR ring now (gauge) |
| `nnew` | entries on the new list now (gauge) |
| `budget_calls` | `kwq_budget_left()` calls on this worker |
| `budget_reads` | of those, the ones that read the clock |

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
| `parks` | rounds the queue was skipped for owing time |
| `boosts` | passes served from the new list |
| `grace` | doorbells sent to the ring instead of the new list by the anti-gaming rule |
| `debts` | doorbells sent to the ring because the queue went idle owing time |
| `glitches` | passes whose CPU-time delta was negative or over 1 s |

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
| `enqueue` | each accepted `kwq_enqueue()`; once per `kwq_enqueue_list()` | `arg1` cpu, `arg2` depth after, `arg3` 1 if it woke the worker |
| `reject` | enqueue or notify refused with `ENOBUFS` or `ENXIO`.  **Not** for `EINVAL`, which is detected before the list lock and fires nothing | `arg1` cpu, `arg2` errno |
| `pass-start` | list swapped, handler about to run | `arg1` cpu, `arg2` items, `arg3` doorbell-to-pass ns (0 on later passes of one backlog) |
| `pass-end` | handler returned | `arg1` cpu, `arg2` items, `arg3` CPU time ns, `arg4` items requeued |
| `overrun` | a pass exceeded its budget by more than one quantum x weight | `arg1` cpu, `arg2` ns over |
| `park` | a queue skipped for owing time | `arg1` cpu, `arg2` deficit ns, zero or negative |
| `budget-hit` | `kwq_budget_left()` answered 0 | `arg1` cpu |
| `round-end` | worker finished a round | untranslated: `arg0` class, `arg1` cpu, `arg2` ring entries, `arg3` wall ns since the last round end |
| `yield` | worker yielded | untranslated: `arg0` class, `arg1` cpu, `arg2` 0 round end, 1 tick guard, 2 share limit |
| `cap` | the share limit (SCHED.md S6.3) | untranslated: `arg0` class, `arg1` cpu, `arg2` event (0 a pass begins under the limit, 1 allowance spent and CPU given away, 2 given away again, 3 period ended while over, 4 deadline callout ran), `arg3` ns into the period, `arg4` ns used of it, `arg5` the pass's budget ns (event 0) |
| `idle` | worker about to sleep | untranslated: `arg0` class, `arg1` cpu, `arg2` busy ns since it woke |
| `doorbell` | a (queue, CPU) placed with its worker | untranslated: `arg0` class, `arg1` cpu, `arg2` worker state when rung: 0 awake, 1 polling, 2 asleep |
| `steal` | defined, never fires yet (P6) | `arg1` from cpu, `arg2` to cpu, `arg3` items |
| `worker-block`, `worker-spawn`, `worker-exit` | defined, never fire yet (P7) | untranslated: `arg0` cpu, `arg1` workers, `arg2` runnable |

`round-end`, `yield`, `cap`, `idle` and `doorbell` are worker probes rather than
queue probes: they carry no `kwqinfo_t`, so their arguments start at
`arg0` with the class id.  Twenty-one probes in total.

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
make -C kwq                 # kwq.ko and kwq_test.ko
make -C kwq install         # modules to /boot/modules, kwq.d to /usr/lib/dtrace
make -C kwq uninstall       # removes both and refreshes linker.hints
```

Defaults of the module build: `WITH_CTF=1` so the DTrace translator
works, `DEBUG_FLAGS=-g` for symbols, `INSTALL_NODEBUG` set so no
separate debug file is installed, and SDT probe sites compiled in.
`make KWQ_NO_SDT=1` compiles the probe sites out, which is how the
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
| sysctl tree, 19 DTrace probes, translator, `show kwq` | implemented |
| writable per-queue `limit` | implemented |
| `kwq_cpu_for_hash()` remap for sparse CPU ids | implemented; the sparse path itself is **untested**, no such machine being available |
| notifiers on the item list in arrival order, budgeted, requeueable (tagged link, 2026-10-08) | implemented, tested by the eight `mix` cases |
| `KWQ_F_STEALABLE` | accepted, inert: no worker steals (P6) |
| `kwq_scatter()` | `EOPNOTSUPP` (P6) |
| `KWQ_BLOCKING` | refused at create with a log line; no workers of that class exist (P7) |
| `KWQ_F_RESERVE`, `nreserve != 0` | asserted at create, not cleanly refused (P7) |
| cache-domain refinement of `KWQ_CPU_ANY` | NUMA domain only (P6) |
| `kwq(9)` and `dtrace_kwq(4)` man pages | P8 |

Clients: `if_pair(4)` runs on kwq and is measured against a frozen copy
of its taskqueue predecessor (`../../extras/if_pair_tq`).  epair is P5,
`if_wg` is the BULK proof in P6.
