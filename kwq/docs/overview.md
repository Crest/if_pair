# kwq(9): an overview, with a tutorial

kwq is a kernel work queue service for FreeBSD.  A subsystem that today
creates its own `taskqueue(9)` with a thread per CPU instead creates one
*kwq queue*, and the work of every such client on a CPU is served by one
shared worker thread that divides its time between them by deficit round
robin in CPU time.  The client keeps what it had: deferred execution on
a CPU of its choosing, batches, and a bound on how much it may queue.
It gains what a private pool cannot give it: fairness against the other
clients on that CPU, a bounded hand-back to lower priority work, and one
set of counters and DTrace probes that answer "who is slow, who is
dropping, who is monopolising a CPU" for every client at once.

This document introduces the model and ends with a tutorial that builds
a working client module.  The companions are
[cheatsheet.md](cheatsheet.md) for patterns and pitfalls,
[reference.md](reference.md) for the exact KPI, sysctl and probe
surfaces, and [whitepaper.md](whitepaper.md) for why the design is
shaped this way.  The normative specifications behind all of them are
`../KWQ.md` (objects, contracts) and `../SCHED.md` (the scheduler).

Contents: 1 the problem; 2 the objects; 3 the life of an item; 4 the
five rules; 5 what kwq is not; 6 tutorial; 7 where to go next.

## 1. The problem kwq exists to solve

FreeBSD has no shared deferred-work service with a policy.  It has
excellent mechanisms (`taskqueue(9)`, ithreads, netisr workstreams,
`epoch(9)` callbacks, per-CPU callout wheels), and every subsystem that
needs per-CPU deferred work builds its own pool out of them.  On a
128-core machine, if_pair, if_epair, if_wg, GELI and iflib each create
on the order of one thread per CPU.  Three consequences follow, and all
three were measured on this project's hardware before kwq was written
(`../../NOTES.md`):

- **No arbitration between pools.**  Two pools on one CPU at the same
  priority interleave by whatever ULE decides; neither can be told to
  take less.  There is no unit of account, so there is nothing to be
  fair with.
- **Starvation of everything below.**  Pools run at `PI_NET` or
  `PI_SOFT` because that is what their work is worth in isolation.
  Saturate 35 of them on a 128-core machine and the per-CPU callout
  threads get about 30 % of a CPU: TCP timers stall machine-wide, ssh
  freezes, and a benchmark aborts on its own control connection
  (`../../samples/starve.log`).  The fix, a bounded demote-yield, is a policy
  decision that belongs in one place, not in five drivers.
- **No common observability.**  Each pool counts what its author
  thought to count.  There is no answer to "which deferred-work client
  on CPU 7 is responsible for this latency" that spans subsystems.

kwq is the one place: one service, a small number of fixed *classes*, a
queue per client, and a scheduler whose unit of account is CPU time.

## 2. The objects

    kern.kwq.net                 the NET class
      .cpu.<N>                     one worker thread per CPU
      .queue.pair0a                one client's queue
        .cpu.<N>                     that queue's list on CPU N

                 CPU 3                              CPU 7
    +---------------------------+     +---------------------------+
    | worker kwq_net/3          |     | worker kwq_net/7          |
    |  ring: pair0a, wg_crypto  |     |  ring: pair0a             |
    |  new:  netisr_ip          |     |  new:  -                  |
    +---------------------------+     +---------------------------+
         ^          ^                      ^
         |          |                      |
    [pair0a:3]  [wg_crypto:3]          [pair0a:7]      per-CPU FIFO lists
         ^                                  ^
         |  kwq_enqueue(q, 3, item)         |  kwq_enqueue(q, 7, item)
      producers on any CPU               producers on any CPU

**Service.**  `kwq.ko`.  At load it creates one worker thread per CPU
for each implemented class, NET and BULK, and nothing else; queues come
and go under it.

**Class.**  `KWQ_NET`, `KWQ_BULK`, `KWQ_BLOCKING`: a fixed set, not a
client-chosen priority.  A class fixes the worker priority, whether the
handler may sleep, and the default quantum.  NET is interrupt-class work
that never sleeps (packet input, 200 us quantum); BULK is lower-priority
work that never sleeps (crypto, compression, 1 ms quantum); BLOCKING is
for work that may sleep and is not available yet.  The class is a
property of the work, so the arms race in which every driver picks the
top priority cannot start.

**Queue.**  What a client creates: a name, a class, a weight, a bound,
one handler function and one context pointer.  A queue is not a thread
and not a list; it is the unit of accounting, naming and observability.
Its name appears verbatim in sysctl, in the lock name and in DTrace, so
`pair0a` and `wg_crypto` are the names an operator sees.

**Per-CPU list.**  Each queue has, on every CPU, one intrusive FIFO list
holding its items and its pending notifiers in arrival order.  The list is the unit of
ordering: items on one (queue, CPU) list are handed to the handler in
the order they were enqueued, and nothing is promised between CPUs.  The
bound (`limit`, default 4096) is per list, so a hot CPU can refuse while
the others are empty, which is a steering symptom with its own counter.

**Item.**  The client's own object with a `struct kwq_item` embedded.
kwq allocates nothing on the data path and owns nothing: an item is
either not queued, on exactly one list, or owned by the handler that
received it.  For packets there is not even an extra field, since an
mbuf's `m_nextpkt` link is layout-identical to a `kwq_item`.

**Handler.**  One function per queue, called with a whole list:
`fn(q, head, n, ctx)`.  It owns those `n` items and must consume them,
by processing, freeing, handing on or giving back.  Pending notifiers
are among them at their arrival positions.  A negative `n` means
"release these without running them", which is how a drain hands back a
backlog.

**Worker, pass, round.**  The worker for (class, CPU) picks a queue,
swaps that queue's list out under the list lock, and calls the handler
once: that is a *pass*.  Serving every queue that has work once is a
*round*.  The worker charges each pass the CPU time it used, which is
what the scheduler divides.

## 3. The life of an item

1. A producer, on any CPU and in almost any context, picks the target
   CPU (normally from a flow hash) and calls `kwq_enqueue()`.
2. The item is appended to that (queue, CPU) list under its lock.  If
   the list was idle, the same lock hold rings the doorbell and wakes
   the worker; if it was already queued or being served, the enqueue is
   an append and nothing is woken.  One wakeup per burst, not per item.
3. The worker wakes, refills the queue's deficit by `quantum x weight`,
   swaps the list out, and calls the handler with everything that
   accumulated.
4. The handler processes items, checking `kwq_budget_left()` as it goes.
   When the budget reaches 0 it hands the untouched remainder back with
   `kwq_requeue()` and returns; the remainder is the first work of the
   queue's next pass, still in order.
5. The pass is charged to the queue's deficit.  A queue that still has
   work goes to the ring tail; one whose list is empty goes idle.
6. At the end of a round the worker yields once, briefly, to whatever
   else wants the CPU, then carries on.

At teardown the client calls `kwq_drain()`, which refuses further
enqueues, waits for passes in progress and disposes of the backlog, and
then `kwq_destroy()`.

## 4. The five rules a client must know

1. **The CPU is the ordering domain.**  Items that must stay in order
   relative to each other go to the same CPU; use `kwq_cpu_for_hash()`
   on a flow hash so one flow always lands on one CPU.  Never use
   `curcpu` for work that has an ordering requirement: the producer
   migrates, the work does not follow, and the order is gone.
2. **Check the budget, and process at least one item.**  Call
   `kwq_budget_left()` after each item (or every few, for very cheap
   items), hand the rest back with `kwq_requeue()` when it returns 0,
   and return.  A handler that ignores it is not killed, but its queue
   is charged, parked and visible in `overruns`.
3. **A NET or BULK handler must not sleep.**  No `tsleep`, no sx locks,
   no `M_WAITOK`, no `copyin`.  This is asserted; it panics under
   INVARIANTS rather than deadlocking in production.
4. **Ownership is explicit.**  An item is yours until `kwq_enqueue()`
   returns 0; a rejected item is still yours, with its error to count; a
   handed-over item is the handler's.  Double enqueue is a bug, and
   `KWQ_ITEM_INIT()` before every enqueue is how you prove you are not
   doing it.
5. **Configuration sleeps, the data path does not.**  `kwq_create()`,
   `kwq_drain()` and `kwq_destroy()` need a sleepable context;
   `kwq_enqueue()` and `kwq_notify()` work from an ithread, a callout,
   netisr or another handler, and from an interrupt filter if the queue
   was created `KWQ_F_SPIN`.

## 5. What kwq is not

- **Not a timer.**  Nothing is scheduled for later; a queue runs when
  work is enqueued into it.
- **Not a thread pool for blocking work.**  That is the BLOCKING class,
  which is specified but not implemented; code that may sleep keeps its
  own pool for now and says so.
- **Not a lock or a barrier.**  There is no synchronous submission, and
  nothing that runs pending work on the caller's thread the way
  `taskqueue_drain(9)` can.  The only "wait for my work" primitive is
  `kwq_drain()`, from a sleepable context, at teardown.
- **Not a scheduler for the client's threads.**  kwq decides when its
  own workers run and how they divide their time; it does not touch the
  producer.
- **Not a queue with per-item priorities.**  Within a (queue, CPU) list
  the order is FIFO, full stop.  Differentiation is between queues, by
  class and weight.

## 6. Tutorial: a deferred-work client in 60 lines

We build `kwq_tutorial.ko`: a module whose sysctl accepts a number,
defers the "work" of accumulating it to a kwq queue, and reports the
total.  It is contrived on purpose, so that every part of the contract
is visible in a page of code.  It compiles against an uninstalled kwq
tree and runs on any machine with at least one CPU.

### 6.1 The item

The client's object carries the work and embeds a `kwq_item`.  It need
not be first; `__containerof()` recovers the object from the link.

```c
struct tut_work {
	struct kwq_item	tw_item;	/* kwq's link, cleared before each enqueue */
	uint64_t	tw_value;	/* the "work" */
};
```

### 6.2 The handler

The handler receives a whole list in FIFO order.  Three things matter:
walk with the next pointer saved *before* touching the item, honour the
budget, and dispose of every item.

```c
static uint64_t	tut_total;	/* written by the handler only */
static struct kwq *tut_q;

static void
tut_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx __unused)
{
	struct kwq_item *it, *next;
	struct tut_work *w;
	int left = n < 0 ? -n : n;

	for (it = head; it != NULL; it = next) {
		next = KWQ_ITEM_NEXT(q, it);	/* save it: we are about to free */
		w = __containerof(it, struct tut_work, tw_item);
		if (n > 0)			/* n < 0: a drain, do not run */
			tut_total += w->tw_value;
		free(w, M_TUT);
		left--;
		/*
		 * Progress rule: at least one item per pass, then the
		 * budget decides.  NULL tail means "the rest of the list
		 * this pass handed me"; kwq knows where it ends.
		 */
		if (n > 0 && next != NULL && kwq_budget_left(q) == 0) {
			kwq_requeue(q, next, NULL, left);
			return;
		}
	}
}
```

### 6.3 The producer

Our producer is a sysctl handler, which runs in a sleepable context, so
it may allocate with `M_WAITOK`.  A real data-path producer would
preallocate or use `M_NOWAIT`, because kwq itself never allocates.

```c
static int
tut_sysctl_submit(SYSCTL_HANDLER_ARGS)
{
	struct tut_work *w;
	uint64_t v = 0;
	int error, cpu;

	error = sysctl_handle_64(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	w = malloc(sizeof(*w), M_TUT, M_WAITOK | M_ZERO);
	w->tw_value = v;
	KWQ_ITEM_INIT(&w->tw_item);		/* prove it is not queued */

	/*
	 * Pick the CPU.  Work with no ordering requirement may go to
	 * curcpu; work that must stay ordered against other work goes
	 * to kwq_cpu_for_hash(<its flow's hash>) so one flow always
	 * lands on one CPU.
	 */
	cpu = kwq_cpu_for_hash((uint32_t)v);
	error = kwq_enqueue(tut_q, cpu, &w->tw_item);
	if (error != 0) {
		free(w, M_TUT);			/* still ours: ENOBUFS or ENXIO */
		return (error);
	}
	return (0);
}
```

### 6.4 Lifecycle

Create inactive, activate when the client is ready to accept work, drain
then destroy at unload.  `KWQ_F_DISCARD` is what makes the drain hand
the backlog to the handler with a negative count instead of running it.

```c
static int
tut_modevent(module_t mod __unused, int type, void *data __unused)
{
	struct kwq_params p;

	switch (type) {
	case MOD_LOAD:
		memset(&p, 0, sizeof(p));
		p.limit = 256;		/* items per CPU list; 0 = class default */
		p.weight = 1;		/* 1..8, relative share within the class */
		p.domain = -1;		/* NUMA domain for kwq's own memory */
		tut_q = kwq_create("tutorial", KWQ_NET,
		    KWQ_F_INACTIVE | KWQ_F_DISCARD, &p, tut_handler, NULL);
		if (tut_q == NULL)
			return (ENXIO);	/* kwq logged the reason */
		kwq_activate(tut_q);
		return (0);
	case MOD_UNLOAD:
		/*
		 * Good enough here, because this module owns nothing else.
		 * A client with cloned interfaces or other SYSUNINIT-torn-down
		 * objects must destroy its queues from a SYSUNINIT instead:
		 * MOD_UNLOAD runs before a file's SYSUNINITs, so a queue
		 * destroyed here would be gone while its clients still exist.
		 * That is the shape if_pair(4) uses.
		 */
		kwq_drain(tut_q);	/* no more enqueues, backlog disposed */
		kwq_destroy(tut_q);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t tut_mod = { "kwq_tutorial", tut_modevent, NULL };
DECLARE_MODULE(kwq_tutorial, tut_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(kwq_tutorial, 1);
MODULE_DEPEND(kwq_tutorial, kwq, 1, 1, 1);
```

The declarations the snippets above assume:

```c
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include "kwq.h"

static MALLOC_DEFINE(M_TUT, "kwq_tutorial", "kwq tutorial items");

SYSCTL_NODE(_kern, OID_AUTO, kwq_tutorial, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "kwq tutorial");
SYSCTL_PROC(_kern_kwq_tutorial, OID_AUTO, submit,
    CTLTYPE_U64 | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    tut_sysctl_submit, "QU", "write a number to defer adding it to total");
SYSCTL_U64(_kern_kwq_tutorial, OID_AUTO, total, CTLFLAG_RD, &tut_total, 0,
    "sum of the numbers the handler has processed");
```

### 6.5 Build and run

The `Makefile`, assuming the kwq tree is at `../kwq` relative to the
module:

```make
KMOD=	kwq_tutorial
SRCS=	kwq_tutorial.c
CFLAGS+=	-I${.CURDIR}/../kwq/kwq
.include <bsd.kmod.mk>
```

Build and load.  `kwq.ko` must be loaded first, or installed so that the
module dependency pulls it in:

```sh
make -C ../kwq            # builds kwq.ko and kwq_test.ko
make                      # builds kwq_tutorial.ko
kldload ../kwq/kwq/kwq.ko
kldload ./kwq_tutorial.ko
```

Submit work and read the result:

```sh
sysctl kern.kwq_tutorial.submit=40
sysctl kern.kwq_tutorial.submit=2
sysctl -n kern.kwq_tutorial.total        # 42
```

### 6.6 Watch it from outside

Everything kwq knows about the queue is already visible, without a line
of instrumentation in the client:

```sh
# the queue exists, with the parameters it was created with
sysctl kern.kwq.net.queue.tutorial.weight kern.kwq.net.queue.tutorial.limit \
       kern.kwq.net.queue.tutorial.state

# which CPUs it has actually been used on, and how much they did
sysctl kern.kwq.net.queue.tutorial | grep '\.items:' | grep -v ': 0$'

# items, passes and worst doorbell-to-handler latency on one CPU
sysctl kern.kwq.net.queue.tutorial.cpu.0.items \
       kern.kwq.net.queue.tutorial.cpu.0.passes \
       kern.kwq.net.queue.tutorial.cpu.0.maxlat_ns
```

With DTrace, the same questions and a few that counters cannot answer:

```sh
# every pass of our queue: how many items, how long the handler ran
dtrace -n 'kwq:::pass-end /args[0]->kwq_name == "tutorial"/ {
	printf("cpu %d items %d ns %d requeued %d", arg1, arg2, arg3, arg4); }'

# the age of a burst when its first pass began, per queue, as a histogram
dtrace -n 'kwq:::pass-start /arg3 != 0/ { @[args[0]->kwq_name] = quantize(arg3 / 1000); }'
```

In DDB, `show kwq` prints every queue, its per-CPU state and deficit,
and every worker with its ring.

### 6.7 Two experiments, with the numbers they produced

A single sysctl write is one item, and the handler above is far faster
than a shell, so nothing interesting happens until several writers
compete.  Both experiments below were run on a 4-vCPU test guest; your
counts will differ, their shape will not.

**Experiment 1: the doorbell and the grace rule.**  Add `DELAY(50)` to
the handler so an item costs 50 us, and submit from six shells at once:

```sh
for i in 1 2 3 4 5 6; do
	( for j in $(seq 1 60); do sysctl kern.kwq_tutorial.submit=1 >/dev/null; done ) &
done; wait
sysctl kern.kwq.net.queue.tutorial.cpu.1
```

360 items over 352 passes, `maxdepth` 3, `requeued` 0, `overruns` 0: the
handler keeps up, so almost every pass is one item and the budget is
never in play.  The interesting counters are `boosts` 1 and `grace` 325.
The queue goes idle and comes back 325 times, and the anti-gaming rule
sends it to the ring rather than the new list every time but the first.
That is section 5.4 of the white paper, visible in a tutorial.

**Experiment 2: the budget, the requeue and the bound.**  Make an item
cost more than the whole quantum (`DELAY(2000)`, i.e. 2 ms against the
200 us NET quantum), send everything to one CPU (`cpu = 1` instead of
the hash, which also shows that the bound is per CPU), set
`p.limit = 2`, and submit 120 times from four shells:

```sh
for i in 1 2 3 4; do
	( for j in $(seq 1 30); do sysctl kern.kwq_tutorial.submit=1 \
	    >/dev/null 2>&1 || echo refused; done ) &
done; wait
```

51 of the 120 writes failed with "No buffer space available", and
`rejected` is exactly 51: kwq refuses and hands the item back, it does
not grow.  `requeued` is 192, the budget check doing its job.  Two
results are worth understanding rather than filing as bugs:

- `overruns` is 43, although the handler checks its budget after every
  item.  An item that costs 2 ms cannot help overrunning a 200 us
  quantum: the progress rule runs one item per pass whatever the
  budget says.  Overrun accounting measures what the queue owes, and a
  client whose single items dwarf the quantum belongs in a class with a
  larger one, or should split its work.
- `maxdepth` reaches 11 with `limit` 2, because `kwq_requeue()` is
  exempt from the bound by design: already-admitted items go back, and
  the bound governs admission only.

Then delete the `kwq_budget_left()` check and repeat: `requeued` falls to
0, `overruns` tracks `passes`, `parks` grows, and the `kwq:::overrun`
probe fires with the nanoseconds over.

## 7. Where to go next

- [cheatsheet.md](cheatsheet.md): the dozen patterns real clients use,
  each with what to do, what it costs and what goes wrong.
- [reference.md](reference.md): every function, flag, error, sysctl and
  probe, with contexts and ranges.
- [whitepaper.md](whitepaper.md): why DRR in CPU time, why per-CPU
  lists, what was taken from fq_codel, squeues, cmwq and libdispatch,
  what was rejected, and the measurements behind each choice.
- `../KWQ.md` and `../SCHED.md`: the normative design and scheduler
  specifications.  Where this documentation and those differ, they win.
