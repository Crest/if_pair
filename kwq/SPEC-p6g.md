# P6g: BULK placement by load gossip on a torus of cores (plan for review)

Status: ACCEPTED 2026-11-05 (crest's goal: execute the plan), with the
decisions of section 9 taken as proposed (D1-D7); D8, a07, is asked at
P6g.3.  PLAN.txt's P6g entry.  Branch kwq-bulk, worktree
~/if_pair-scratch/wt/bulk.  Proposed as "P6b"; renamed because P6a-P6f
are P6's own steps in PLAN.txt.

The idea, from the discussion of 2026-11-04 (crest's questions on the
design space of stealing): every CPU publishes a load figure; the cores
form a 2D torus, each with up to four neighbours; each core keeps a small
table of timestamped (core, load) entries for the best nearly idle cores
it knows and pulls its neighbours' tables to refresh it.  Producers
place `KWQ_CPU_ANY` items from their own core's table, and an overloaded
worker hands work to a core from its table.  Idle workers search
nothing: they sleep.

The plan has four stages, each ending at a gate reviewed with crest:

| stage | what | gate |
|---|---|---|
| P6g.0 | the SMT order of today's code and the kernel's SMT yield, on lab | numbers feed the simulator |
| P6g.1 | a simulator running the real placement code, the scenario matrix, the bounds | G1: does the scheme meet section 1, against the alternatives? |
| P6g.2 | the kernel implementation behind a knob, ATF cases, on the guest | ATF clean on GENERIC-DEBUG |
| P6g.3 | A/B on lab, then a07 (permission), against the simulator's predictions | G2: the default |
| P6g.4 | documentation | post-merge doc review |

## 0. The problem, measured

| finding | evidence |
|---|---|
| The idle path's steal attempt reads (CPUs in reach) x (stealable queues) list depths, once before every sleep and after every wake (kwq_worker.c:1043) | lab, 128 kwq-geli providers on md(4), 4 KiB random reads: 550k IOPS and 28.2 us of CPU per request with stealing, 1.58M and 9.6 us without; 1.05 attempts and 0.18 steals per request |
| `kwq_pick_any()` compares this queue's depth on each CPU, not the CPU's BULK backlog (kwq.c:517) | with many queues of about one item each, an item lands on the caller's CPU when no worker sleeps nearby, however full it is |
| `kwq_bulk_idle` is one cpuset written by every BULK worker at every sleep and wake | code (kwq_worker.c:1234, 1254); not measured; a hot line at a07's 128 CPUs |
| The SMT sibling is the first ring of the topology order, so the first choice of placement and stealing | lab, userland, pinned: AES-256-XTS 8.9 GB/s on one thread, 9.8 on two siblings together, 17.6 on two cores; 1 MiB memcpy 43, 26 and 69 GB/s |
| Stealing paid only where the producer fixed the CPU | kwq_test `steal` (2000 items onto one CPU); a MAC-heavy provider fed on one CPU: 103 against 38 MB/s.  With `KWQ_CPU_ANY` it changed nothing at 16 providers (lab) and 64 (a07), and cost two thirds of the small-read rate at 128 |

## 0a. P6g.0 results (lab, 2026-11-05, c00fe2b)

Medians of three repetitions, A today's order, B the sibling last
(samples/p6g0_lab_2026-11-05.txt, script samples/p6g0_lab.sh):

| measurement | result |
|---|---|
| kernel SMT yield, kwq_crypto AES-XTS, not stealable | one thread 5.16 GB/s (4 KiB) and 5.48 (1 MiB); two siblings 6.39 and 6.66 (+24 %, +22 %); two cores 10.19 and 10.88.  A thread beside a busy sibling runs at 0.61 of its speed, not the 0.55 of userland openssl |
| line transfer (userland ping-pong, pinned) | one way 24 ns between siblings, 31-38 ns between cores of the L3; a write read back by another core 18 ns (sibling) and 39-46 ns (core) |
| K0, KWQ_CPU_ANY, 2 queues x 4 in flight, 1 MiB | A 41.9 GB/s, B 33.5 (-20 %).  A's sibling-first pick makes a request alternate between the two threads of one core: the worker that completes it resubmits to its sleeping sibling, so each core runs one request at a time and keeps it in its L2.  B sends it to the first idle thread of another core, which may have a busy sibling.  The ideal, 8 requests on 8 cores without moves, is 8 x 5.48 = 43.8 |
| K0, 4 or 8 queues x 4 in flight (every thread busy); 4 queues fed on CPU 2 (stealing) | A = B within 0.5 % |
| K0, 16 kwq-geli providers on md(4) | 4 KiB reads 1.505M against 1.513M IOPS, B 3.7 % more BULK CPU; 1 MiB writes 13.18 against 13.14 GB/s |

Consequences:

- **D4: the sibling-last order is not a fix** for today's code; the
  order stays.
- **The simulator** takes 0.61 as the crypto SMT factor, the line costs
  above for lab, and a cost for an item that runs on another core than
  the one that produced it (its data in that core's L2), calibrated so
  that P1 with A's and B's orders reproduces the -20 % (check V4, S3.6).
- **The placement order gains a step 0** (S2.5): a producer that is the
  CPU's own BULK worker, resubmitting from its handler, places on its
  own CPU when its own backlog is below L_local.  Today's sibling-first
  pick gets this case nearly right by accident; step 1 alone would move
  the request to another core.  The simulator runs it with and without
  step 0 (ablation).

## 1. Goals and acceptance criteria

Goals:

- **G1, cost:** placement costs a bounded number of remote line reads,
  independent of the number of queues and of CPUs: at most 2 on average,
  at most 7 in the worst case (2 candidates, 4 neighbours, the queue's
  depth on the target).
- **G2, idle path:** a worker going to sleep reads no remote line.
- **G3, no new shared writes on the per-item path:** every word added
  has one writer CPU, or one writer core for a core's own block.  The
  target's list lock, taken by every enqueue today, stays the only
  remote write.
- **G4, quality:** good placement on average, and bounded bad cases.
  The bounds matter more.
- **G5, out of the box:** no per-machine tuning, no pinning, default
  MSRs.
- **G6, exported KPIs only** (section 2.10).

Non-goals: NET and BLOCKING placement; queues created without
`KWQ_F_STEALABLE` and items with a fixed CPU on them (unchanged); the
NUMA placement of item memory; any change to ULE.

Acceptance criteria.  These are proposed values; crest decides them in
the review (decision D1).  The metrics M1-M11 are defined in section 3.4,
the oracle in 3.3.

Average case, every steady scenario, every topology:

| | criterion |
|---|---|
| A1 | mean wait at most max(1.2 x the oracle's, the oracle's + 10 us) (A1f, G1), and no worse than today's kwq (P1) in any steady scenario (A1t).  As first written, 1.2 x the oracle's failed every policy at waits of a few microseconds |
| A2 | at least 95 % of placements good (M2) |
| A3 | at most 2 remote line reads per placement on average; merges cost at most 0.2 % of a busy CPU |

Worst case, every scenario, every seed (p99.9 and max over the run):

| | criterion |
|---|---|
| W1 | wait p99.9 at most 2 x the oracle's p99.9 + 1 ms; max wait at most 3 x the oracle's max + 2 ms.  Under overload (rho >= 1), where waits grow for every policy: throughput at least 97 % of the oracle's, Jain's index across queues at least 0.95 |
| W2 | idle-while-waiting (M3) at most 2 % of the machine's capacity in steady states; after a step change, back below 2 % within 10 ms on 16 CPUs and 25 ms on 128, at p99 over seeds |
| W3 | a core that goes idle next to a backlog is used within 2 ms at p99; one h torus hops away within 2h + 1 ms at p99, and within h x Imax at max (M4) |
| W4 | at most max(8, the oracle's) items from distinct sources land on one core within one status-update window, at p99.9 (M6; W4o, G1: the oracle itself met the bound of 8 in 886 of 1460 runs, as M6 counts an idle core taking items one after another).  P6g.3 counts pile-ups in the kernel: items landing behind another source's queued item |
| W5 | in steady scenarios (G1: every stealing policy spreads a flash crowd in log2 halvings), at most 1 % of items move more than once, none more than 3 times; under uniform overload at most 1 hand-off per second per core (M7) |
| W6 | under uniform overload the scheme costs at most 0.5 % of CPU more than placement without stealing (P2) |
| W7 | after any idle period, no placement uses an expired entry, and at least 80 % of the placements in the first 10 ms are good |
| W8 | kernel: no item lost or doubled, no assertion or WITNESS report on the GENERIC-DEBUG guest, the knob safe to flip under load |

Kernel criteria (P6g.3), medians over at least three repetitions, same boot:

| | criterion |
|---|---|
| K1 | 128 providers, small reads, lab: IOPS within 3 % of stealing off (1.58M), CPU per request at most 5 % above it |
| K2 | 128 providers, bulk writes: no worse than today |
| K3 | 16 providers on lab and 64 on a07: no worse than today within the run-to-run range |
| K4 | pinned producers (kwq_test `steal`, the four-providers-on-one-CPU case with `cpu=N`, the MAC-heavy provider): spread at least today's, and MB/s at least 90 % of today's |
| K5 | the responsiveness probes (spin loop share unpinned and pinned, the 1 kHz timer's p99, fork+exec): no worse than today's kwq-geli within the run-to-run range |

## 2. The design

### 2.1 Nodes and the torus

- **Nodes are cores**: an SMT group of `smp_topo()`, or a single CPU on
  a machine without SMT.  Siblings are in the same node and read each
  other's words in the core's own block, so no gossip link goes to a
  sibling.
- **Shape**: R x C with R = floor(sqrt(ncores)) and C = ceil(ncores / R).
  The cores are laid out row-major in topology order, so a row tends to
  share an L3 and a column joins neighbouring L3s.  A partial last row
  wraps within the nodes that exist; duplicate neighbours collapse,
  hence "up to four".
- **Distance** in the ranking is topology distance (the rings of
  `kwq_topo_order`), not torus hops.
- **Sizes**: lab, 8 cores, 2 x 4, 3 hops across; a07, 128 cores, 11 x 12
  with a last row of 8, 11 hops; the guest, 4 CPUs, 2 x 2, 2 hops.
  Built once at module load:
  FreeBSD does not hot-plug CPUs.

### 2.2 Per-core blocks

Three blocks per core, each `KWQ_LINE` (128 bytes) aligned and allocated
in the core's domain (`malloc_domainset_aligned`, as kwq_cpu is).  The
layout is fixed by CTASSERTs.

| block | writers | write rate | readers | contents |
|---|---|---|---|---|
| status | each thread its own word; siblings share the L1, so the line does not travel between cores | on a change of load bucket only (2.3) | the 4 neighbour cores at merge; producers checking a candidate | one 64-bit word per thread (up to 8): time 32, load 12, flags 4, spare 16 |
| table | the core's threads, one at a time (try-flag in the control block) | at most once per merge, and only the words that changed | the 4 neighbour cores at merge; producers on this core | 4 entries of 64 bits: time 32, core 14, load 10, idle threads 2, hops 4, spare 2 |
| control | the core's threads | per merge | the core's threads only | try-flag, last merge time, interval, the merge's own counters |

- **Words**: written with `atomic_store_64`, read with `atomic_load_64`;
  both are single-copy atomic on amd64 and arm64.  No seqlock: entries
  are independent hints, and a table mixing old and new entries is
  harmless.
- **Time**: `sbinuptime() >> 12` (about 1 us), truncated to 32 bits.
  It wraps after 4096 s and is compared by signed difference.  A stamp
  that seems to lie more than 61 us in the future is one that wrapped
  (more than 2048 s old) and counts as expired; so does a merge time,
  which makes the merge due, and a pending fall's start, which lets the
  fall be published.  Without this a table left alone for half an hour
  would look fresh again (found by the simulator, whose clock starts
  near the wrap).  `cpu_ticks()` is not used: it jumps on a07.
- **Memory**: 384 bytes per core, 48 KB on 128 cores.  The counters go
  into one per-class sysctl table, not per queue (the memory audit's
  per-(queue, CPU) sysctl cost is not repeated).

### 2.3 The load figure and the status word

The BULK worker computes its thread's load:

- **Backlog**: the active lists' depths times the queue's cost per item
  on this CPU, an average of measured ns per item kept per (queue, CPU)
  by the worker, which is the only writer.  Each queue counts for at
  most one quantum (DRR serves it one quantum per round).
- **Divided by the share**: an average of BULK's fraction of wall time
  while it had work.  The share limit makes it 0.5 when other threads
  are runnable.
- **No term for other threads' CPU time.**  The first draft added a
  bucket from `pc_cp_time`; in the simulator it made every CPU of a
  machine running other work look equally loaded at about one quantum,
  and placement went blind: 128 kwq-geli providers on lab, 1.01M
  against 1.65M IOPS without it.  BULK runs at priority until its share
  is spent, so other threads delay an item only through the share,
  which the average above already carries.

The worker updates the word at wake, at the start and end of a pass and
before it sleeps, but writes it only when the quantized bucket changes:

- **Asymmetric hysteresis**: a rise is written at once, a fall only after
  it has held for 100 us.  Producers avoid a core as soon as it fills,
  and a core that empties for a moment does not attract a herd.  A
  worker that goes to sleep with a fall pending sleeps with a timeout
  of the rest of the hold, publishes, and sleeps again if it woke too
  early; otherwise an idle core keeps advertising its last load (the
  simulator's first run lost two idle cores to this).
- **No per-item flag**: the word does not carry "asleep".  Whether to
  wake a worker stays the doorbell's business (`kw_sleeping` under
  `kw_mtx`).  "Idle thread" in the ranking means load bucket 0, smoothed.
  The 4 KiB load, which sleeps and wakes per item, then writes its word
  rarely.

### 2.4 The merge (the forwarding)

1. **Read** the 4 neighbour cores' status and table blocks (8 lines).
2. **Collect candidates**: each neighbour core as a direct observation
   (hops 1, stamped now); their table entries as relays (hops + 1, the
   stamp copied, never renewed); the own table.
3. **Filter**: drop entries about the own core and entries older than
   T_exp; for each core keep the freshest stamp (ties: fewer hops); keep
   cores below the advertise mark L_adv.
4. **Rank**: more idle threads first, then lower load, then smaller
   topology distance, then fresher.  Keep 4.
5. **Write** only the words that changed.  A direct observation renews
   an unchanged entry only when the entry is older than T_exp / 4, to
   keep the lines quiet.

Interval, Trickle-style: between Imin and Imax.  It goes back to Imin
when the table or the own bucket changed, and doubles when the merge
changed nothing.  A failed check (S2.5) is Trickle's inconsistency: it
shortens the interval to Imin / 4.  Making the merge due at once was a
merge storm in the simulator (33000 merges/s per core in S3).
Tombstones, the freshest "busy" observations relayed beside the
entries so that a stale relay cannot bring a busy core back, were
tried and gained nothing; they stay an ablation.

Who merges, never on a timer:

| core state | who, and when |
|---|---|
| a worker busy | the worker, between passes, once the interval has passed |
| workers asleep, producers active | the producer, in the enqueue path, inside `critical_enter()`, when the table is older than the interval; the try-flag keeps the two siblings from merging at once |
| a worker waking after a sleep longer than Imax | the worker, before its first placement decision or hand-off |
| fully asleep | nobody: its status words stay true, and awake neighbours renew the entries about it |

### 2.5 Placement for `KWQ_CPU_ANY` (knob `place=1`)

The producer reads its own core's table, a local line.  Choices are
randomized among equal ranks, from a per-CPU generator.

0. **The own CPU, when the producer is its BULK worker** (a handler
   resubmitting work) and its backlog without this item is below
   L_local: the item runs at the next pass, without a wake, in a warm
   cache (P6g.0, section 0a).  Except when a large item's sibling is
   busy and the table holds a whole idle core within D_near: then the
   item goes there (step 1).  Without that exception two requests that
   once landed on one core run beside each other at the SMT rate for
   good (the simulator, V4: 26.8 GB/s against today's 41.9).
1. **An idle core in the table** within the near distance D_near (same
   L3): check its status block (1 read) and place on its idle thread.
2. **Else the idle sibling**, from the own core's status block (local).
3. **Else the own thread**, if its load is below L_local.
4. **Else the table's least loaded candidate**, checked.  At most 2
   candidates are checked; a check that fails is written into the own
   table as a direct observation, which repairs it for every queue at
   once.
5. **Else read the 4 neighbour cores' status blocks** and take the
   least loaded of them and the own core.

Before appending, the queue's own depth on the target is read
(`kc_depth`, in the line the enqueue locks anyway).  A list above the
queue's high mark sends the item to the next candidate.

Item size enters through the queue's cost per item: below C_small,
steps 2 and 3 move before step 1.  Locality wins for small items, a
whole core for large ones.  The simulator decides whether that split
pays (ablation).

### 2.6 Spill for queues with a fixed CPU

An item queued with a fixed CPU to a `KWQ_F_STEALABLE` queue is placed
as `KWQ_CPU_ANY` when that CPU's load is at or above the high mark H.
Queues created without `KWQ_F_STEALABLE` never spill.  This covers the
two cases where stealing measurably helped, before the items are
queued.

### 2.7 Hand-off, in place of stealing (`place=1`)

When a pass ends with the thread's backlog at or above H, the worker
looks for a table candidate with load at most L and checks it.  If one
is there:

1. It cuts the older half of its longest stealable live list, at most
   `steal_max` items, under its own list lock.
2. It appends the batch to the candidate's list of the same queue.
3. It rings the candidate's doorbell.

The rules around it:

- **At most one hand-off per pass.**
- **No ping-pong**: H > L + the batch's cost, so a batch cannot come
  straight back.
- **Drain fence**: the read side of `kwq_steal_rm`, as for a steal
  today.
- **Pass cap**: the rule "take one item when a helper sleeps nearby"
  becomes "take one item when the table holds an idle core within
  D_near".

Counters `handoffs_in` and `handoffs_out` per CPU, equal machine-wide;
probe `kwq:::handoff(q, from, to, n)`.

### 2.8 The knob

`kern.kwq.bulk.place`: 0 is today's code (`kwq_pick_any()`, the idle
mask, stealing); 1 is this scheme, without the idle mask and without
the idle-path steal; 2 is P8 (section 3.3), the sharded idle mask and
the donor line (G1: both go into the kernel, section 4).  It can be flipped at run time under load, for A/B
runs on one boot.  P6g.0 adds no knob of its own: it runs a build with
the sibling last in the ring order against a build without.

### 2.9 Constants to start from

The simulator sets the final values (scenario S18):

| constant | start | meaning |
|---|---|---|
| Imin, Imax | 1 ms, 8 ms | merge interval; 1 ms is BULK's quantum and the share limit's period |
| T_exp | 32 ms | entries not confirmed for this long are dropped |
| status fall hold | 100 us | a lower bucket is published after it held this long |
| H, L | 2 ms, 0.5 ms of backlog | hand-off and spill above H, to cores at or below L |
| L_adv | 0.5 ms | cores at or below it are advertised |
| L_local | 50 us | a producer's own thread is good enough below it: M2's tolerance for a good placement (0.2 ms in the first draft kept two 191 us requests in series on one thread beside idle cores) |
| C_small | 20 us | items cheaper than this prefer the near choices |
| D_near | the L3 | a near candidate's distance |
| table, neighbours | 4, 4 | |

### 2.10 Kernel interfaces

Used, all exported:

- `critical_enter()` / `critical_exit()`;
- `atomic_load_64`, `atomic_store_64`, `atomic_cmpset_int`;
- `sbinuptime()`;
- `smp_topo()`;
- `pcpu_find()` and `pc_cp_time`;
- `malloc_domainset_aligned()`;
- kwq's existing doorbell and wake paths.

An internal that would help: the runnable count of another CPU (ULE's
`tdq_load`), for the CPU-busy part of the figure.  The export to ask
for would be a read-only `sched_load(int cpu)` hint.

## 3. P6g.1: the simulator

### 3.1 Structure

- **Shared code**: the placement code lives in
  `kwq/kwq/kwq_place.{h,c}`, pure C with an environment header (clock,
  64-bit loads and stores, the try-flag), as `kwq_sched.c` is shared
  with `sim/sim.c` (PLAN P1a).  The simulator links the code the kernel
  runs: the merge, the ranking, the placement and the torus builder.
- **New program**: `kwq/sim/place/` (`placesim`), a discrete-event
  simulator with ns time, single-threaded and deterministic per seed.
  Runs are independent processes, run in parallel on the host;
  `make test` runs the regression suite (3.6).  `kwqsim` stays as it
  is.
- **Unit tests** of the shared code, built and run in the same
  directory: encoding, the merge rules, expiry and wrap, and the torus
  for every topology of 3.5.

### 3.2 The model

| part | model |
|---|---|
| topology | cores, SMT threads, L3 groups, sockets, absent CPUs; the T1-T5 machines of 3.5 |
| line costs | a remote line read or write costs by distance: same core 0, same L3, other L3, other socket.  Each line has a version and each reader remembers the version it saw, so the simulator counts real transfers and invalidations, not accesses.  lab's same-L3 cost calibrated (3.8); the others are stated assumptions |
| workers | one per thread: per-queue FIFO lists and DRR with a 1 ms quantum; `kwq_sched.c` itself per simulated CPU if its environment allows many instances, else a simplified DRR, checked against kwqsim's invariants |
| speed | an item's service time is its cost divided by the thread's speed: the SMT factor (sibling busy: 0.61 for crypto in the kernel, P6g.0; 0.30 for copies; swept 0.3-0.9) times BULK's share (competing threads under the share limit: 0.5; interrupt storms: 0 to 1 per scenario) |
| sleep and wake | a worker sleeps when empty; wake latency drawn from a distribution, shallow after short sleeps and deeper after long ones (calibrated, 3.8) |
| producers | per producer thread: open (Poisson, on/off bursts, periodic), closed (N in flight with think time, GEOM-like providers); `KWQ_CPU_ANY` or a fixed CPU; reads completing on other CPUs than they were issued on (md(4)'s threads) |
| item costs | 4 KiB (3 us), 1 MiB (300 us), mixtures, Pareto (alpha 1.5) cut at the pass budget, as kwq-geli resumes a bio |
| scheme costs | every remote read, write, merge, check and hand-off is charged as CPU time to the thread doing it, so overhead shows as lost throughput |
| limits | the per-(queue, CPU) limit of 4096 with refusal and a producer's retry (GEOM's ENOMEM pacing) |
| share limit | not a rate: BULK runs at full speed until it has used its share (0.5) of the period (1 ms) while other threads wanted the CPU, then waits for the next period (a fluid 50/50 split doubled every item's time and made the simulator steal four times too often) |
| contention | a line transfer or invalidation holds the line for its transfer time; another one waits for it.  This is what makes the idle mask expensive at a07's rates: every BULK worker writes it at every sleep and wake |
| idle-path steal | the scan runs as worker time outside a pass, not charged to the share limit, before the worker sleeps (kwq_steal()); a lone item is stolen only from a sleeping worker or one inside a pass (kwq_steal_lone_ok()) |
| locality | an item run on another core than its producer's pays a move cost, the time to pull its data from L3 (up to the L2's size), calibrated by V4 |
| other work | each request's non-BULK work (GEOM, md(4), the system calls of the load generator: 6.6 us per 4 KiB read on lab) runs as other threads' time on a CPU, sharing it with the BULK worker under the share limit; the next request of a closed-loop provider is issued from there |

### 3.3 Policies compared on the same traces

| policy | what |
|---|---|
| P0 oracle | each item to the thread with the least true start delay machine-wide, information free and instant (the reference for A1 and W1); also P0-L3, the same within the producer's L3, which separates information from locality |
| P1 today | `kwq_pick_any()` (idle mask, the queue's own depth, 4 probes), the idle-path steal with its scan cost, the steal wake, the pass cap |
| P2 no stealing | `kwq_pick_any()` only |
| P3 two choices | the producer's thread and two random cores of its L3, true loads at the cost of the reads |
| P4 group words | per L3 an array of its threads' published loads, 16 to a line, the least loaded taken (the first draft's one summary word per L3 herded onto the CPU it named: 265 us waits at rho 0.5) |
| P7 donor | today's placement (`kwq_pick_any()` and the idle mask); a CPU whose stealable backlog reaches H sets its bit in one donor line (written at the transitions only) and names its longest stealable list in a hint line; an idle worker reads the donor line once before it sleeps instead of scanning every list in reach; becoming a donor wakes the nearest sleeper.  crest's first idea of 2026-11-04, added after the first matrix runs |
| P8 shard | P7 with the idle mask cut into one line per group of 16 CPUs, written by that group's CPUs only.  A producer takes the nearest idle worker whose list of the queue is empty from its own group's line and up to two more, nearest first; without one, from one far group's line, a cursor walking the other groups that stays on a group while it has an idle worker; else its own CPU unless that is a donor, else the nearest CPU that is not.  An item for a fixed CPU spills when that CPU is a donor or its list holds `donor_us`.  A donor holds a stealable backlog of `donor_us` = 500 us or more, flagged by its worker at its decision points and by a per-CPU tick (1 ms, armed while the CPU holds stealable work) when the worker reached none since the last tick: a starved donor, whose lone items may be stolen too.  A lone item counts the pass's batch and the running item's remainder with it and may be taken by an idle thief, not by a puller (today's `kwq_steal_lone_ok()`).  Steal wakes at depths 2, 4, 8, ...; a thief that leaves the donor at `donor_us` or more wakes one more; a worker whose backlog falls below L pulls from a donor at the end of a pass; a steal takes no more than leaves the taker below `donor_us` (a puller: below L).  Spill, wakes, pull and the bound were added after the first matrix runs (4c16dd9, b813fde), the far probe and the tick after the worst-case set (f04df4f), the lone item after W2a (01ec314) |
| P5 this scheme | with ablations: no SMT ranking; no hand-off; no spill; fixed 1 ms interval; no check of candidates; no randomization; symmetric hysteresis; tables of 2 and 8; 8 neighbours; a random expander instead of the torus; a per-core "incoming" counter written by producers (the remedy if herding fails W4); a last-target hint per (queue, producer CPU) |

### 3.4 Metrics

| | metric |
|---|---|
| M1 | wait from enqueue to start: mean, p50, p99, p99.9, max; per item class |
| M2 | regret: the start delay chosen minus the least true start delay at that instant (counterfactual, on a sample of placements); good if within max(50 us, 0.1 x the item's cost) |
| M3 | idle-while-waiting: core time spent idle while an item waits longer than 100 us within the same L3, and anywhere |
| M4 | discovery: from a core going idle, or a backlog forming, to the first item placed or handed to it; per torus hop |
| M5 | reaction after a step: time until 95 % of placements are good and M3 is below 2 % |
| M6 | herding: most items landing on one core from distinct sources within one status-update window |
| M7 | movement: items moved 0, 1, 2, 3+ times; hand-offs per second; pairs handing back and forth |
| M8 | overhead: remote reads, writes and invalidations per placement and per second per CPU; merges per second; CPU share |
| M9 | information: age of the entries used; share of failed checks; table occupancy |
| M10 | locality: placements to the own thread, the sibling, the L3, another L3, another socket |
| M11 | overload: work completed per second against capacity; Jain's index across queues; refusals |

### 3.5 Scenarios

Topologies:

| | machine |
|---|---|
| T1 | the guest: 4 CPUs, flat |
| T2 | lab: 8 cores x 2 threads, one L3 |
| T3 | a07: 128 CPUs, flat, no SMT |
| T4 | synthetic: 2 sockets x 4 L3s x 8 cores x 2 threads (128 threads), no machine to validate it (decision D5) |
| T5 | odd shapes: 6, 12 and 127 threads, absent CPUs |

The scenarios, each on every topology it fits:

| | scenario | what it tests |
|---|---|---|
| S1 | uniform open load, rho 0.1 to 0.99, every item mix | the average case |
| S2 | closed-loop providers as geli_bench scale: 1 to 128 providers, 1 to 8 in flight, small and bulk | the measured kernel cases; the model's calibration (3.6) |
| S3 | a hot island ringed by idle cores: k = 1, 2, 4, 9, 16, 32 cores at 2, 4 and 8 x their capacity; inside the torus, across a wrap edge, across an L3 and a socket | how far and how fast the work spreads, and whether far idle cores are found |
| S4 | an idle island in a sea of load: k idle cores among saturated ones | herding on the few idle cores everyone sees |
| S5 | stripes and checkerboards aligned with the torus axes, and diagonals | the torus's own worst layouts |
| S6 | a dead band: a loaded region and an idle region separated by cores busy with non-BULK threads, whose BULK workers sleep and which have no producers | information crossing sleeping relays (bound B2) |
| S7 | load coming back after an idle period of 10 ms, 100 ms, 1 s and 10 s, as a step to rho 0.5, 0.9 or 1.5, everywhere or in one island | expired tables, the first milliseconds (W7) |
| S8 | load stopping in a region | how fast new idle cores are advertised |
| S9 | a moving hot spot, one core per 0.5, 1, 4 or 16 ms | staleness at speeds around the merge interval |
| S10 | on/off load with periods of 0.5 to 20 ms, in phase and out of phase across producers | aliasing with the Trickle intervals |
| S11 | flash crowds: 2000 items at once onto one CPU, fixed and `KWQ_CPU_ANY` (the kwq_test `steal` case); 128 producers bursting at the same instant | spill and hand-off speed; herding |
| S12 | a rate collapse after placement: a core's BULK share drops to 0.5, 0.1 or 0 for 1 to 100 ms while it holds a backlog | rescue by hand-off (bound B5) |
| S13 | non-BULK threads hogging half the cores, steady and churning | the share limit in the load figure |
| S14 | SMT: sibling factors 0.3, 0.55, 0.9; SMT ranking on and off; small and large items | the SMT rules of 2.5 |
| S15 | every core fully loaded, rho 1.0, 1.2 and 2.0; then one core goes idle; then the load falls to rho 0.9 | overhead, churn and fairness in overload (W5, W6); the needle; recovery |
| S16 | producers with a fixed CPU on stealable queues, every load | spill (K4) |
| S17 | random: topology, scenario mixture and constants drawn at random, thousands of seeds, the invariants and bounds checked on each | the protocol fuzzed |
| S18 | sweeps of Imin, Imax, T_exp, H, L, the hysteresis and the table size | the constants of 2.9, and cliffs near them |

Steady scenarios run at least 20 seeds, worst-case scenarios at least
100.  Every result is a distribution over seeds, and every worst case
keeps its seed so it can be replayed.

### 3.6 Bounds to prove and check, and the model's own validation

| | bound or check |
|---|---|
| B1 | discovery: an idle core h hops from a backlog is known there after at most the relays' merge intervals, at most h x Imax, and h x Imin while their tables change.  The simulator reports the margin |
| B2 | dead band: the relays of a dead band are BULK-idle themselves, so they are targets.  Work placed there wakes them and their merges resume.  Checked with S6 |
| B3 | herding: at most (tables holding the core) x (placements per source within the status-update window).  The fast rise of 2.3 makes the window the wake latency plus one status write.  Checked against W4 |
| B4 | no ping-pong: with H > L + a batch's cost, a batch cannot return before new arrivals lift the receiver above H.  A potential-function argument, checked with M7 |
| B5 | wait of a placed item: at most (H + one item) / the lowest share, plus one DRR round, while the share is above 0; with share 0, bounded by the rescue's discovery (B1) |
| V1 | the oracle on Poisson arrivals and exponential costs matches M/M/c's mean wait (Erlang C); random placement matches M/M/1 |
| V2 | conservation: items in = items out, no running item moved, `handoffs_in == handoffs_out`; DRR checked as in kwqsim |
| V3 | the measured kernel cases: P1 against P2 at 128 providers must show the measured direction and size (CPU per small read 2.9 x with stealing; bulk within 5 %), and no difference at 16 providers.  If the model cannot reproduce these, its cost model is wrong and its predictions do not count |
| V4 | P6g.0's K0: P1 with today's order against the sibling last, 2 queues x 4 in flight of 1 MiB on lab: A 41.9, B 33.5 GB/s (-20 %); no difference with every thread busy.  It calibrates the move cost; the 4- and 8-queue cases then check it |

### 3.7 Output

Each run writes TSV rows (scenario, topology, policy, seed, metric,
value).  A summary script turns them into JSON for an HTML report:

- distributions per scenario and policy;
- the worst cases with their seeds;
- the verdict on every criterion of section 1.

The report is published as a private artifact for gate G1.

### 3.8 Calibration on lab

Userland and read-only: no setting is changed and no module is loaded.

- **Line costs**: a ping-pong between two threads, measured on the same
  core and on two cores of the L3.
- **Wake latency**: from WAKE.md's measurements where they cover it,
  otherwise from the existing kwq counters in P6g.0's run.

a07's costs are left as assumptions until P6g.3 can check them.

### 3.9 Gate G1

A report to crest, with a push notification:

- the criteria met and missed, per policy;
- the ablations, showing which parts of the scheme earn their keep;
- the constants chosen;
- the worst cases found.

crest decides: on to the kernel, revise, or fall back to P4 group
words.

Outcome (2026-11-05, decisions D9-D13): the report
(https://claude.ai/artifact/U246kQmWvgBj6TYDJYyDYz, kwq-bulk cbabcd3)
recommended P8.  crest chose to implement both P8 and the gossip
scheme in the kernel and compare them on kwq-geli; with the kernel's
numbers in, a study of the design space for a hybrid that gossips
unless a more expensive scheme is needed (section 5a).

## 4. P6g.2: the kernel

The steps.  Each builds, passes the ATF suite on the guest, and is
committed on kwq-bulk with crest's go-ahead (reminders at each
checkpoint).

| step | what |
|---|---|
| K0 (P6g.0) | the sibling last in the ring order, as an A/B build on lab, plus kwq_crypto pinned to sibling pairs and to separate cores for the kernel's SMT yield (decision D4 on keeping it as a fix of today's code) |
| K1 | `kwq_place.{h,c}` with the kernel environment; the torus from `smp_topo()`; a sysctl dump of the torus |
| K2 | the per-core blocks with their CTASSERTs; the load figure and the status word in the BULK worker; the per-class counter table |
| K3 | the merge: worker between passes and at wake, producers at enqueue, the try-flag, the interval; probe `kwq:::merge` |
| K4 | placement under `place=1` with the repair of failed checks and the fallback; probe `kwq:::place(q, from, to, rank, reads)` |
| K5 | spill for fixed CPUs on stealable queues |
| K6 | hand-off in place of the idle-path steal, behind the drain fence; probe `kwq:::handoff` |
| K7 | the knob, flippable under load; `place=1` writes no idle mask and skips the idle-path steal |

P8 under `place=2` (G1), beside the steps above:

| step | what |
|---|---|
| K8 | `kwq_bulk_idle`, one cpuset, cut into one line per group of 16 CPUs in topology order, each written by its own CPUs; the group size a loader tunable, so the 4-CPU guest can run groups of 1 and exercise the far probe |
| K9 | the donor bitmap and a per-CPU hint naming the longest stealable list, written by the owner at its decision points |
| K10 | the idle path: the donor steal in place of `kwq_steal()`'s scan; the steal wakes as today, the pull at the end of a pass, the bound, the lone item; no cascade wake (D11).  The steal wake and the pass start's helper test look for a sleeper in reach across the group lines that cover it, as today's code does in its one mask: the simulator's P8 looked in the own group only, which on the guest with groups of one CPU left a donor nobody to wake while three workers slept (6 of 3000 items on the far CPU) |
| K11 | `kwq_pick_any()` over the group lines, then one far group from a per-CPU cursor; donors avoided; spill for fixed-CPU queues at a donor |
| K12 | the starved tick: a per-CPU callout(9), armed while the CPU holds stealable work, flagging a worker that reached no decision point since a starved donor.  A direct callout, in the clock interrupt: BULK runs at `PI_SOFT`, above every user thread, so what starves it is an interrupt thread or NET-class work at `PI_INTR` (or a thread at its own priority), and those do not stop the tick |

ATF cases (kwq/tests/kwq_test.sh, kwq_test.ko):

| case | checks |
|---|---|
| handoff | 2000 items of 100 us onto one CPU of a stealable queue: as many run elsewhere as today's `steal` case (guest: 1506, the ideal 1500); `handoffs_in == handoffs_out` machine-wide |
| handoffdrain, spilldrain | 100 activate/drain cycles under hand-off and under spill: nothing lost or doubled |
| place_switch | `place` flipped between 0 and 1 every 10 ms for 10 s under load: nothing lost, no assertion |
| place_any | one producer with every worker idle: items go to idle cores first and to the sibling last (the placement counters by rank) |
| place_stale | 1 s idle, then a burst: no expired entry used (counter `expired_used` stays 0) |
| torus | the kernel's torus matches the userland builder's for this machine |
| existing | steal, steal_off, stealdrain, stealdrain_discard and scatter pass under `place=0`, and the ones that apply under `place=1` and `place=2` |
| island | `place=2`, groups of 1 on the guest: producers on one CPU above its capacity reach CPUs beyond the near groups (the far probe) |
| starved | `place=2`: a real-time spinner on one CPU for 100 ms while its list holds items: they run elsewhere within a few ticks (the starved tick) |

The guest runs GENERIC-DEBUG: INVARIANTS and WITNESS clean is part of
W8.

## 5. P6g.3: kernel measurements

Order: the guest (correctness, direction only), then lab, then a07,
which waits for crest's permission (decision D8).  The rules:

- Stages run interleaved on one boot: `place=0` (today),
  `place=0 steal=0` (no stealing), `place=1` (gossip), `place=2` (P8),
  and stock GELI where it applies.
- At least three repetitions; medians and ranges.
- Unpinned, default MSRs; pinning only for the SMT diagnostic.
- Uptime before and after; every setting logged and undone; one battery
  per machine at a time; machines left clean.

Tests:

- geli_bench `scale` (1 to 128 providers, small and bulk) and `ssd`;
- the responsiveness probes;
- kwq_crypto's weight case;
- kwq_test `handoff` and `steal`;
- the four-providers-on-one-CPU case with `cpu=N`;
- new kwq_test loads:
  - `island`: producers on a block of CPUs above capacity, the rest
    idle;
  - `idleburst`: 1 s and 10 s idle, then a burst.

The kernel against the model: dtrace aggregations of `kwq:::place`,
`kwq:::merge` and `kwq:::handoff` give M8-M10 and the movement counts.
These are compared with the simulator's predictions for the same
scenario.  A difference beyond 2 x sends the model back to 3.2, and
G1's worst cases are rerun before any conclusion.  hwpmc's line-transfer
counters on lab for `place=0` against `place=1` are a diagnostic; the
events are chosen on the box.

Gate G2: the default of `place`.  The better of 1 and 2 if K1-K5 hold;
0 is kept for one round of A/B either way (decision D3).

## 5a. The hybrid (after P6g.3)

With the kernel's numbers for both schemes in: the design space for a
hybrid that gossips (a local table, no shared line on the item path)
unless a more expensive scheme is needed, as where the simulator found
gossip weak (an island on a flat 128-CPU machine, stale tables at
returning load) and P8 strong.  What triggers the costlier path, what
it costs when idle, and its bounds; candidates simulated in placesim
and judged by section 1 before any kernel code (decision D9).

## 6. P6g.4: documentation

- SCHED.md: a new section for placement by gossip; S16 marked as the
  `place=0` path.
- KWQ.md: S3's `KWQ_CPU_ANY` text, and the client table's GELI row,
  which still describes the notifier plan.
- OBSERVABILITY.md: the knob, the counters, the probes.
- BULK.md: the results.
- PLAN.txt: the P6g entry and its status.
- docs/reference.md: sysctl and probes.
- README: the directory map, with sim/place.
- The 128-provider samples and scripts of 2026-11-04 (steal_lab,
  prof2_lab) go into samples/.

Then the post-merge doc review: every statement against the code.

## 7. Order and effort

| stage | estimate | ends with |
|---|---|---|
| P6g.0 | half a day | lab numbers for the simulator; K0's A/B |
| P6g.1 | 3 to 4 days | G1 report and artifact |
| P6g.2 | 3 to 4 days | ATF clean on the guest |
| P6g.3 | 1 to 2 days plus a07's slot | G2 report |
| P6g.4 | 1 day | docs reviewed |

Reports at every stage end, push notifications at the gates and at any
blocker.

## 8. Risks

| risk | handling |
|---|---|
| the simulator's coherence and timing model is too coarse | V3 must reproduce lab's measured cases first; P6g.3 compares the kernel's probes with the predictions |
| the load figure is noisy or biased (DRR, the share limit, the tick regime) | ablations in S13 and S18; the figure's parts can be tested one at a time in the kernel |
| herding on idle islands (S4) | the fast rise, randomization, checks; the "incoming" counter as the measured remedy if W4 fails |
| the scheme is more machinery than lab needs | P4 group words are simulated beside it; G1 may choose them |
| a07's flat topology makes the torus layout arbitrary | all a07 CPUs are equally far apart in FreeBSD's view, so only the hop count matters there; T3 tests it |
| `pc_cp_time` is statclock-coarse | it only sets a bucket; the share average carries the fine part |
| the merge at wake interferes with the tick regime's accounting (KWQ.md S16) | the merge runs outside the round the regime judges; checked by the existing tick counters' identities |
| flipping the knob under load races with hand-offs in flight | the drain fence covers batches in transit; `place_switch` tests it |

## 9. Decisions (taken 2026-11-05; D9-D11 at G1)

| | decision | taken |
|---|---|---|
| D1 | the acceptance criteria of section 1 | adopted as proposed; adjusted only with crest after the first simulator runs |
| D2 | under `place=1`, does the hand-off replace stealing entirely, or does a gated idle-path steal stay as a fallback? | the hand-off replaces it; today's code stays under `place=0` |
| D3 | after G2: `place=1` the default if K1-K5 hold, with `place=0` kept for one round of A/B? | yes |
| D4 | K0 (the sibling last for today's code): a separate quick fix, or only an input to the simulator? | no fix: B loses 20 % with 8 requests on 16 threads and is level elsewhere (section 0a); an input to the simulator |
| D5 | the synthetic multi-socket topology T4, with no machine to validate it | in, its results marked unvalidated |
| D6 | the last-target hint per (queue, producer CPU) | a simulator ablation; into the kernel only if it wins |
| D7 | the per-queue "overloaded until" word | out: no client in the tree drops on refusal |
| D8 | permission for a07 at P6g.3 (scale, ssd and the new loads, about an hour) | asked when P6g.3 gets there |
| D9 | G1: which placement goes into the kernel | both P8 (`place=2`) and the gossip scheme (`place=1`), compared on kwq-geli; then the hybrid study of section 5a |
| D10 | G1: criteria | A1f for A1, W4o for W4, W5 for steady loads only, the model's known gaps accepted (section 3.8); W2b kept as written; further changes may be proposed |
| D11 | G1: P8's cascade wake | left out of the kernel (no run changed by much in the ablations) |
