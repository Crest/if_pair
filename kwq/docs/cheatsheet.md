# kwq(9) cheat sheets: patterns, recommendations and warnings

Working patterns for kwq clients, each with what it is for, the code,
what it costs and what goes wrong.  The warnings are mistakes that were
made and measured during development, not hypotheticals.  So are most of
the patterns: 1, 4 to 8 and 10 to 12 are what `if_pair(4)`, the only
in-tree client, actually does.  Patterns 2, 3 and 9, for batched
enqueue, notifiers and interrupt filters, follow the KPI and the
specification but have no client behind them yet, so treat their details
as untested where this says so.  Exact signatures and
ranges are in [reference.md](reference.md); the reasoning is in
[whitepaper.md](whitepaper.md); the normative contracts are `../KWQ.md`
sections 3 and 6 and `../SCHED.md`, which win wherever this disagrees
with them.

Contents: 1 deferred input; 2 handing over a list; 3 signals instead of
items; 4 choosing the CPU; 5 the budget; 6 teardown; 7 mbufs; 8 vnets;
9 interrupt filters; 10 bounds and backpressure; 11 classes and weights;
12 diagnosis recipes; 13 anti-patterns.

---

## 1. Deferred input: the standard shape

**For:** a pseudo-interface, a protocol input path, anything that takes
one unit of work from an upstream caller and must not process it inline.

```c
/* producer: one item per call, no batch to offer */
cpu = kwq_cpu_for_hash(flow_hash);
KWQ_ITEM_INIT(KWQ_MBUF_ITEM(m));
if (kwq_enqueue(sc->sc_q, cpu, KWQ_MBUF_ITEM(m)) != 0) {
	m_freem(m);
	if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
	return (ENOBUFS);
}

/* handler: the whole list, budget per item, remainder back */
static void
my_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
{
	struct my_softc *sc = ctx;
	struct kwq_item *it, *next;
	struct mbuf *m;
	int left = n;

	if (n < 0) {				/* drain: release, do not run */
		for (it = head; it != NULL; it = next) {
			next = KWQ_ITEM_NEXT(q, it);
			m_freem(KWQ_ITEM_MBUF(it));
		}
		return;
	}
	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);
		m = KWQ_ITEM_MBUF(it);
		m->m_nextpkt = NULL;		/* the stack expects it clear */
		my_input(sc->sc_ifp, m);
		left--;
		if (next != NULL && kwq_budget_left(q) == 0) {
			kwq_requeue(q, next, NULL, left);
			return;
		}
	}
}
```

**Recommended:** enqueue per unit as it arrives.  The doorbell makes a
staging buffer pointless: the first enqueue into an idle list wakes the
worker, every later one while the list is queued or being served is a
plain append, and the batch a pass receives is exactly what accumulated
during the previous pass.  No flush timer, no added latency when idle.

**Recommended:** if the handler dereferences an object with a
refcount, take the reference for the whole pass rather than per item.
The real client wraps each pass in `if_ref(ifp)` and `if_rele(ifp)`.
Acquiring and releasing a refcount is not sleeping, so this is legal in
a NET handler, and it is cheaper once per pass than once per packet.

**Warning:** do not process inline "just this once" when the queue looks
empty.  For TCP between two local sockets the input path loops back into
the sender's own inpcb lock: on an INVARIANTS kernel that is "recursed
on non-recursive mutex", and on a production kernel the mutex silently
recurses and a later walk off the end of an mbuf chain panics.  The
queue is the reentrancy break.

## 2. Handing over a list you already have

**For:** a producer that genuinely holds several items at once, for
example a software TSO split or a driver's receive batch.

```c
/* head..tail already linked through the kwq_item links, n items */
error = kwq_enqueue_list(q, cpu, head, tail, n);
/* all-or-nothing: on error nothing was queued and every item is still yours */
```

**Recommended:** use this whenever the batch exists for free.  It is one
lock hold, one doorbell and one limit check for the whole list, and it
is the single biggest performance lever in the KPI: on a 128-core
Neoverse-N1, four items per call cut the per-item cost from about 1350
cycles to 104, a 13-fold throughput increase with no change to the
handler.

**Warning:** do not invent a batch by staging items in the producer.
You pay a flush timer (latency for the last item of every burst), a
staging area per (producing CPU, target CPU) because the target is
chosen per item, and a second copy of the state machine kwq already
runs.

**Warning:** `n` must be the true length and `tail` the true last item,
both linked through `kwq_item` links.  Nothing recounts them.

## 3. Signals instead of items: the notifier

**For:** "something changed, come and look", where the client keeps its
own queue or state and does not want to allocate an item per event.
Equivalent to `taskqueue_enqueue(9)` of a single task, and idempotent.

```c
struct kwq_notifier nf;			/* client-owned, lives as long as the queue */

kwq_notifier_init(&nf, cpu);		/* fixed CPU for its lifetime */

/* producer, any context: cannot fail */
if (kwq_notify(q, &nf))
	/* was idle, now queued */;
else
	/* already pending (coalesced), or the queue is draining */;

/* handler: a notifier arrives in the batch, at its arrival position */
static void
my_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
{
	struct kwq_item *it, *next;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);	/* fetch first: this consumes a notifier */
		if (it == &my_nf.kn_item) {
			state = atomic_load_64(&shared_state);	/* read the LEVEL */
			do_work_for(state);
			if (still_more_to_do)
				kwq_notify(q, &my_nf);	/* re-arm: legal, coalesces */
			continue;
		}
		/* ... an item ... */
	}
}
```

**Recommended:** walk the list even when only the signal matters.
Fetching an element's successor with `KWQ_ITEM_NEXT(q, it)` is what
consumes a notifier; a handler that returns without walking leaves it
pending, and it never fires again.  kwq settles an unwalked head or tail
on your behalf, which covers a lone signal, and asserts under
INVARIANTS so a test shows the mistake at once; a notifier in the middle
of a batch is yours to reach.

**Recommended:** treat a notifier as level-triggered, not
edge-triggered.  Read the current state in the handler rather than
assuming one event per run; a burst of signals legitimately collapses
into one run, and the `coalesced` counter says how often.  Re-arming
from inside the handler is the supported way to say "my list is still
not empty" and guarantees exactly one further run.

**Warning:** a notifier is bound to one CPU for its lifetime, so one
notifier cannot signal several CPUs.  Use one notifier per CPU you need
to signal, or use items.

**Warning:** `kwq_notify_cancel()` may sleep: teardown only, and it is
not needed after `kwq_drain()`, which leaves every notifier idle.

**Recommended:** mixing notifiers and items on one queue is fine since
2026-10-08: a signal is delivered at its arrival position among the
items and is budgeted like one, so a handler stops before it and hands
it back when the budget is gone.  The `mix` ATF cases hold this.  What
remains true is that a signal's handler costs what it costs: a notifier
handler that spends a quantum still delays the items behind it by that
quantum, once, where before the change it starved them to one item per
pass.  In the handler, call `KWQ_ITEM_NEXT(q, it)` before acting on an
element, as the walk above does; for a notifier that call is what makes
re-arming safe.

## 4. Choosing the CPU

| situation | use | why |
|---|---|---|
| work belongs to a flow, connection or interface queue | `kwq_cpu_for_hash(hash)` | same hash always lands on the same CPU, so order is kept; the same mapping TCP uses for per-CPU timers, so a connection's worker and its timers share a CPU |
| work was produced by an interrupt on a known CPU and has no other affinity | that CPU's id | the data is already in that CPU's cache |
| work is independent of all other work (parallel chunks) | `KWQ_CPU_ANY` (BULK queues only) | the caller's CPU if its list is empty, else the shallowest of at most four probed CPUs in the caller's NUMA domain |
| the item has no usable hash (a packet too short to parse, an unknown family) but its peers do | a fixed CPU chosen once per queue at create time | what the real client does: one unhashable stream stays on one list instead of following whichever CPU its producer runs on |
| nothing of the above and order does not matter | `curcpu` | cheapest, cache-warm |

**Recommended:** derive the hash once, store it where the stack can see
it (for packets, `m_pkthdr.flowid` with `M_HASHTYPE_SET()`), and reuse
it, so that the peer and any further hop inherit the same steering.

**Warning:** `curcpu` is wrong for anything with an ordering
requirement.  A producer thread migrates between CPUs, its work follows
it, and two items of one flow end up on two lists with two workers.
There is no ordering between CPUs, by design.

**Warning:** a hash that is skewed for your traffic is a steering bug
that shows up as `ENOBUFS` on one CPU with the others idle.  Look at
`sysctl kern.kwq.<class>.queue.<name> | grep '\.items:'` before blaming
the service.

## 5. The budget

```c
if (next != NULL && kwq_budget_left(q) == 0) {
	kwq_requeue(q, next, NULL, left);	/* NULL tail: the rest of this pass */
	return;
}
```

**Recommended cadence:** per item when an item costs about a microsecond
or more; every 8 to 16 items when items are much cheaper.  The call is
cheap by construction: it reads the clock only every
`budget_check_every` calls (default 8) and whenever the estimate comes
within a tenth of a quantum of the end, and extrapolates in between.

**Recommended:** stop when the remaining budget is less than the worst
case for one item if you know that figure and want to avoid overshoot
entirely.  Either way is within the contract.

**Warning:** always process at least one item before checking.  The
progress rule guarantees a queue in debt still makes progress; a handler
that checks first and returns without doing anything can spin.

**Recommended:** treat `kwq_requeue()` as a list operation, not a
return.  Running an epilogue after it is legal and usually necessary:
the real client requeues, breaks out of its loop, then restores the vnet
and releases its interface reference.  Only the requeued items are off
limits afterwards.

**Warning:** pass `NULL` as the tail.  Walking the remainder to find the
tail costs time proportional to the backlog, is charged to your queue
*after* the budget check, and with a deep backlog it spirals: in
development this put a 16,000-item backlog on one CPU while fifteen
other workers idled, and the same layout ran 1.7 M packets per second
instead of 9.9 M.

**Warning:** going a few microseconds over budget is normal and costs
exactly those microseconds, carried into the next pass.  Being parked
for a round needs a pass that overruns by a *whole quantum*.  Do not
build elaborate avoidance for the former.

**Warning:** if one of your items can cost more than a quantum on its
own, no amount of checking prevents an overrun: the progress rule runs
one item per pass regardless.  Measured on a 200 us NET quantum with
2 ms items, every pass overruns.  Such a client belongs in a class with
a larger quantum, or should split its items.

## 6. Teardown

```c
/* 1. stop producers reaching the queue (driver-specific) */
clear_running_flag(sc);
peer_clear_running_flag(sc);
NET_EPOCH_WAIT();		/* for epoch-protected producers */

/* 2. now kwq's side, from a sleepable context */
kwq_drain(q);			/* refuses enqueues, waits for passes, disposes backlog */
detach_things_the_handler_touches(sc);
kwq_destroy(q);			/* asserts drained */
free(sc);
```

**Recommended:** quiesce your producers first, then drain.  `kwq_drain()`
guarantees no pass is running and no item is queued when it returns, but
it cannot stop a producer that is about to call `kwq_enqueue()`; such a
producer merely gets `ENXIO`, which may not be what your counters
expect.

**Recommended:** create with `KWQ_F_DISCARD` if a backlog at teardown
must be freed rather than processed, and handle `n < 0` in the handler.
Without the flag the drain *runs* the backlog, which means the handler
touches objects you may be in the middle of detaching.

**Warning:** `kwq_drain()` and `kwq_destroy()` sleep and must not be
called from a handler; waiting for your own queue from inside it
deadlocks and is asserted.

**Warning:** the handler can still be called during `kwq_drain()`.
Anything it dereferences must stay valid until the drain returns.  If
the drain hands items back with `n < 0`, do not touch the client object
in that branch at all; free the items and return.

**Warning:** for a driver, drain *before* you tear down the interrupt or
release the device's resources, not after: the drain's final pass may
touch hardware.  A driver that must stop the hardware first instead
creates the queue `KWQ_F_DISCARD`, so the backlog is freed rather than
processed.

**Warning, for notifier clients:** empty or abandon your own list before
draining.  The drain does not re-arm: a `kwq_notify()` from the
handler's last run, meaning "there is still work", returns `false`,
counts in `rejected` and fires `kwq:::reject` with `ENXIO`.  The
remainder then sits in your list with nothing left to signal for it.

**Warning:** a queue you created but never activated must *still* be
drained before it is destroyed; the assertion does not care that it
never accepted work.  The error path of a client whose own setup fails
after `kwq_create()` therefore reads:

```c
q = kwq_create(...);
if (q == NULL)
	return (ENXIO);
if (my_next_setup_step() != 0) {
	kwq_drain(q);		/* inactive: nothing to hand back */
	kwq_destroy(q);		/* asserts drained, even now */
	return (ENXIO);
}
kwq_activate(q);
```

## 7. When the items are mbufs

```c
KWQ_ITEM_INIT(KWQ_MBUF_ITEM(m));			/* before enqueue */
error = kwq_enqueue(q, cpu, KWQ_MBUF_ITEM(m));
...
m = KWQ_ITEM_MBUF(it);					/* in the handler */
m->m_nextpkt = NULL;					/* before the stack sees it */
```

**Recommended:** use the mbuf's own `m_nextpkt` link, which is
layout-identical to a `kwq_item`, so a packet needs no extra field and
no wrapper allocation.  A client that already holds an `mbufq` can hand
the whole thing over with `kwq_enqueue_list()` by passing its first and
last packet and its length.

**Warning:** clear `m_nextpkt` before handing a packet to the stack.
`ip_input()` and friends expect it NULL; a leftover link makes them
process your remaining backlog from inside one call, out of kwq's
accounting and past its budget.

**Warning:** one mbuf with one packet header is one item, including a
TSO chain.  Never enqueue a packet that still has followers on its
`m_nextpkt`.  `KWQ_ITEM_INIT()` clears that link, so the followers are
simply lost; skip the `KWQ_ITEM_INIT()` and kwq asserts instead, because
the item looks already queued.  To hand over a chain, use
`kwq_enqueue_list()` with its true head, tail and length.

## 8. vnets

| the work belongs to | do |
|---|---|
| a vnet, for the queue's whole life (per-vnet housekeeping created in a `VNET_SYSINIT`) | create with `KWQ_F_VNET`; the worker sets the vnet around the handler |
| an object that can move between vnets (an interface, a socket, a packet with an `rcvif`) | leave the flag off; set the vnet in the handler from the object, per pass |

**Recommended, for the movable case:**

```c
CURVNET_SET(if_getvnet(ifp));
/* ... the pass ... */
CURVNET_RESTORE();
```

**Warning:** `KWQ_F_VNET` captures `curvnet` at create time and kwq holds
no reference to it, so such a queue must be drained and destroyed before
its vnet dies.  For an interface that can be moved with `if_vmove()` the
cached vnet is simply wrong after the move; the interface itself is the
authority.

## 9. Enqueueing from an interrupt filter

**For:** a driver whose primary interrupt handler (not its ithread) must
defer work.

```c
q = kwq_create("mydrv", KWQ_NET, KWQ_F_SPIN, &p, handler, sc);
/* now kwq_enqueue()/kwq_notify() are legal from a filter or under a spin mutex */
```

**Recommended:** use `KWQ_F_SPIN` only if you really enqueue from a
filter or while holding a spin mutex.  It makes the per-CPU list locks
spin mutexes for every producer of that queue, which is the same trade
`taskqueue_create_fast(9)` makes.

**Warning:** enqueueing to a sleep-mutex queue from a filter is a
WITNESS panic (a blockable lock in filter context), not a subtle
slowdown.  Conversely, a `KWQ_F_SPIN` queue's producers must keep their
hold short: they are spinning, and the handler is not affected.

## 10. Bounds and backpressure

**Recommended:** pick `limit` from your own flow control, not from fear.
The bound is per (queue, CPU) list, so the memory it implies is
`limit x ncpu x item size` in the worst case.  Measurements on this
project's traffic put the knee at 512 items per list and drop-free
behaviour from 1024; the default is 4096.

**Recommended:** treat `ENOBUFS` as a signal, not an error to hide.
Count it where your subsystem already counts drops, and consider it
upstream backpressure if you have a path for it.

**Warning:** `KWQ_LIMIT_NONE` exists for closed, self-limiting producers
(a reclamation path whose item count is bounded by something else).  A
queue that never refuses is a queue that grows without bound; if you
cannot name the thing that bounds it, you want a number.

**Warning:** the bound is per CPU, so `limit = 4096` on a 128-CPU
machine is half a million items in the worst case.  See the memory
arithmetic in [reference.md](reference.md).

## 11. Classes and weights

| work | class |
|---|---|
| packet input, interface queues, anything that was `PI_NET` and never sleeps | `KWQ_NET` |
| crypto, compression, checksumming, background rebuilds; never sleeps | `KWQ_BULK` |
| anything that may sleep (I/O, `M_WAITOK`, sx locks) | `KWQ_BLOCKING`, not implemented: keep your own pool and document it |

**Recommended:** leave `weight` at 1 unless you can name the queue you
want to be served ahead of and say why.  Weights are bounded (1 to 8)
and act within a class only; they are a share, not a priority, and they
cannot starve anyone.

**Warning:** the class is a property of the work, not of the client's
importance.  Putting bulk work in NET to make it faster is the
tragedy-of-the-commons move that this design exists to prevent, and it
is visible in `kern.kwq.net.queue.*` to anyone looking.

## 12. Diagnosis recipes

```sh
# is my handler too slow?  overruns ~ passes means it ignores the budget
sysctl kern.kwq.net.queue.myq | grep -E 'overruns|parks|requeued' | grep -v ': 0$'

# am I batching?  items per pass over 5 s
p0=$(sysctl -n kern.kwq.net.cpu.3.passes); i0=$(sysctl -n kern.kwq.net.queue.myq.cpu.3.items)
sleep 5
p1=$(sysctl -n kern.kwq.net.cpu.3.passes); i1=$(sysctl -n kern.kwq.net.queue.myq.cpu.3.items)
echo "$(( (i1 - i0) / (p1 - p0 + 1) )) items per pass"

# how busy is the worker, and does anything else want the CPU?
sysctl kern.kwq.net.cpu.3.busy_ns kern.kwq.net.cpu.3.idle_ns \
       kern.kwq.net.cpu.3.yields kern.kwq.net.cpu.3.tick_yields

# who is dropping, on which CPU, with which error
dtrace -n 'kwq:::reject { @[args[0]->kwq_name, arg1, arg2] = count(); }'

# queueing delay per client, as a histogram of microseconds
dtrace -n 'kwq:::pass-start /arg3 != 0/ { @[args[0]->kwq_name] = quantize(arg3 / 1000); }'

# who overruns and from where
dtrace -n 'kwq:::overrun /args[0]->kwq_name == "myq"/ { @[stack()] = count(); }'

# the whole picture at once
sysctl kern.kwq.net.queue.myq
```
```
db> show kwq
```

**Warning:** the queueing-delay recipe above is deliberately silent on a
continuously backlogged queue, and that is not a fault.  `pass-start`
reports the age of a burst only on the first pass after a doorbell; on
later passes of the same backlog `arg3` is 0 and the predicate skips
them.  Verified in the guest: against two saturated queues the
histogram stayed empty while the probe fired 8,471 times, and against a
light 1 kHz queue next to a flood it filled in at once, 64 to 128 us
for most bursts.  If you want delay for a saturated queue, read
`maxlat_ns` with `reset`, or drop the predicate and distinguish first
from later passes yourself.

**Recommended:** reach for the pass-level probes (`pass-start`,
`pass-end`, `round-end`) before the per-item `enqueue` probe.  Disabled
probes cost 0.6 % of a saturated worker; an enabled pass-level probe is
about 140 ns per firing a few times per pass, while `enqueue` fires per
item and costs more than kwq's own per-item work.

**Warning:** a non-zero `glitches` on any queue means this machine's
`cpu_ticks()` is not monotonic (seen on Ampere Altra).  Every CPU-time
figure the kernel reports there, kwq's included, is then suspect; use
wall-clock figures for conclusions.

## 13. Anti-patterns

Each of these was either hit during development or is asserted in the
code.

**Sleeping in a NET or BULK handler.**  Asserted;
`THREAD_NO_SLEEPING()` is in force for the pass.  There is no spare
worker to take over, by design: the alternative, tolerating blocking
with extra threads, is what makes a pool's thread count unbounded.

**Reading a live tunable inside the handler.**  A queue keeps working
off its backlog after the code that filled it has moved on.  In
development a test harness read its own "how many producers" knob in the
handler; the next run changed the knob while the previous queue was
still draining, the handler indexed past its array, and the machine
panicked eight times before the pattern was recognised.  Snapshot
anything the handler needs into the queue's context at create time, or
into the items.

**Double enqueue.**  An item on a list that is enqueued again corrupts
the list.  `KWQ_ITEM_INIT()` before every enqueue, and a `KASSERT` on
the link being NULL inside kwq, are the two halves of the proof.

**Walking the remainder to find the tail for `kwq_requeue()`.**  See
pattern 5: pass `NULL`.

**Requeueing the same item forever.**  A handler that gives back the
item it was about to process, every pass, burns one pass per round and
starves the rest of its own queue behind it.  The signature is
`requeued` and `passes` growing while `items` does not.  This is the
distinct twin of the spin above: there the handler does nothing, here it
does nothing *useful*.

**Using the queue as a lock.**  There is no synchronous submission and
no barrier.  If you need "my work has finished", that is `kwq_drain()`
from a sleepable context at teardown, or a completion callback of your
own design.

**One wakeup per item.**  If your `wakeups` counter tracks your `items`
counter, your producer is enqueueing into a list that has already been
swapped out, item by item, with the worker going to sleep in between.
That is usually a sign the handler is far faster than the producer, and
it is the one case where a little producer-side batching helps.

**Creating a queue per flow or per connection.**  A queue costs a name,
a sysctl subtree and one `kwq_cpu` structure per CPU, and it is a unit
of *policy*.  Flows belong on the CPU axis of one queue, not on queues
of their own.

**Expecting order between CPUs.**  There is none, ever.  If two items
must be ordered, they must go to the same CPU.

**Destroying without draining.**  Asserted.  `kwq_destroy()` on a queue
with a pass in flight would free the structure the worker is using.
