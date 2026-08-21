# if_pair - developer notes

This file is the **developer documentation** for if_pair: design
rationale, the verified claims about kernel behavior the driver
depends on (with their provenance), audit results for pf and ipfw,
build notes and open items. If you just want to *use* the driver,
start with [README.md](README.md) (a gentle introduction with a
quick-start example) and the [if_pair(4)](if_pair.4) manual page.

if_pair is an out-of-tree FreeBSD (15.0+) kernel module providing
pairs of cross-connected **point-to-point layer-3 interfaces** for
connecting vnet jails to each other or to the jail host.

## Why not epair(4)?

`epair(4)` emulates a full Ethernet link: each side has a MAC address,
frames get Ethernet headers, and address resolution runs ARP (IPv4) or
NDP (IPv6). For the common case - routed IP traffic between two vnets -
all of that is overhead and complexity with no benefit.

`if_pair` instead creates two `IFF_POINTOPOINT` interfaces (`pairNa` and
`pairNb`) whose transmit paths are cross-connected at layer 3:

- No Ethernet headers, no MAC addresses, no ARP/NDP.
- A packet transmitted on one side is enqueued for the peer and
  delivered into the peer's IPv4/IPv6 input path, in the peer's vnet,
  by a CPU-pinned pool worker.
- BPF is attached with `DLT_NULL` (4-byte AF pseudo-header), so `tcpdump`
  works on both sides, as with `lo(4)`.

## Usage

```sh
kldload ./if_pair.ko
ifconfig pair create                     # -> pair0a + pair0b
ifconfig pair0a inet 192.0.2.1/32 192.0.2.2
ifconfig pair0b vnet myjail
jexec myjail ifconfig pair0b inet 192.0.2.2/32 192.0.2.1
```

Destroying either side destroys both halves, as with modern
`epair(4)`.

## Building

This must be built **on FreeBSD 15.0 or newer** with kernel sources
installed.

```sh
make                       # uses /usr/src/sys
make SYSDIR=/path/to/src/sys   # explicit kernel source location
```

To keep the source tree pristine, give the build an object directory.
`/usr/obj${.CURDIR}` is on make's default `.OBJDIR` search path, so
once it exists, a plain `make` uses it automatically:

```sh
make obj    # creates /usr/obj$(pwd) (or: mkdir -p /usr/obj$(pwd))
make        # builds there; project directory stays clean
```

Objects, generated option headers and the `machine`/`x86`/`i386`
include symlinks all land in the object directory. (An alternative
prefix can be chosen with `MAKEOBJDIRPREFIX`, set in the environment
or on the command line; see the `.OBJDIR` section of make(1).)
Without any object directory the build is
"untied" and drops those artifacts into the project directory; they
are covered by `.gitignore` and removed by `make clean`. Run
build and clean with the same object-directory setup, since `clean`
cleans the `.OBJDIR` of its own invocation.

Port builds (`ports/net/if_pair-kmod`) stage everything in their own
`WRKDIR` and never touch the source directory.

## Layout

- `README.md` - user-facing introduction and quick start.
- `Makefile` - standard `bsd.kmod.mk` out-of-tree module build.
- `if_pair.c` - the driver.
- `if_pair.4` - man page (`man ./if_pair.4` to preview).
- `tests/` - root-only shell test suite: `lib.sh` (shared helpers),
  fourteen `t_*.sh` cases (expected use and corner cases; failures
  leave state in place for debugging), `run_all.sh`, `cleanup.sh`
  (post-failure sweep), `vm-testplan.md` (the manual battery), and
  the older `smoke.sh`.
- `example-jail.conf` - working jail.conf(5) reference for a pair
  between two vnet jails; the authoritative example of the
  point-to-point addressing syntax (local/32 + peer destination).
- `samples/` - captured evidence referenced by these notes: the
  original iperf3 panic backtrace (`crash.txt`), the
  top(1) captures behind the flow-hashing and scheduler analyses
  (`top-*.txt`), the GENERIC-DEBUG run's filtered syslog with
  the vnet-teardown LOR (`syslog-after-test.txt`), the 128-core
  starvation log behind the batch/yield fix (`starve.log`), and
  the big-iron scaling investigation's benchmark and profiling
  captures (`t17_*.txt`, `t18_*.txt`, `t19_*.txt`,
  `dtrace-tail.txt`).
- `LICENSE` - BSD-2-Clause.
- `ports/net/if_pair-kmod/` - FreeBSD port skeleton (see Roadmap).

## Roadmap

1. **FreeBSD port** (`net/if_pair-kmod`): the skeleton in `ports/` is
   ready except for distribution - it needs a public repository or
   release tarball (`USE_GITHUB`/`MASTER_SITES` + `make makesum`), a
   `WWW` line, and a `poudriere testport` run. Style: `portlint -AC`.
2. **Eventually, maybe, base system inclusion**: the driver is written
   base-style throughout (kernel normal form, in-tree APIs only, mdoc
   man page, `SPDX-License-Identifier: BSD-2-Clause`). Known items for
   that step: move the man page install into the base build glue,
   convert `tests/` to ATF (`tests/sys/net/` conventions, like
   `if_epair` tests), and resolve the upstream interactions
   documented below: four affecting `epair(4)` today (libalias NAT
   repair gate; `divert_packet()` and `CSUM_IP`; the cloner
   create-return window, a root-triggerable panic in `epair(4)`;
   the vnet teardown lock order, an unload-vs-jail-removal
   deadlock - see the locking audit) plus the routed-TSO
   tryforward gap that transit offload would need - ideally by
   landing those fixes independently.

## Design notes / status

The driver uses the modern opaque-`ifnet` accessor API (`if_t`,
`if_get*`/`if_set*`) and the `ifc_attach_cloner()` cloner interface, both
required/current on FreeBSD 14+ - modeled on `epair(4)` and `gif(4)`.

### Performance / delivery design

Every transmitted packet is enqueued onto the receiving side's
`mbufq` (one per pool worker, guarded by a per-queue mutex - the
driver's only locks) and delivered by a pinned worker - **never
inline**, regardless of `net.isr.dispatch`. The workers hand packets
to `netisr_dispatch()`, which with the default direct policy runs the
input path right there in the worker; netisr's own queues and threads
are involved only if the admin selects deferred dispatch. (netisr
itself, despite its per-CPU workstream architecture, ships as a
single *unpinned* thread - `net.isr.maxthreads=1`,
`net.isr.bindthreads=0` - which is why the driver brings its own
pool rather than leaning on it.)

- **Why queueing is mandatory, not a choice** (learned the hard way -
  see the postmortem below): inline dispatch runs the peer's entire
  input path nested inside the sender's call chain. For TCP between
  two local sockets that chain loops back: the sender's
  `tcp_output()` holds its inpcb lock when the peer's inline ACK
  re-enters `tcp_input()` for the *same connection in the same
  thread*. On `INVARIANTS` kernels this dies as "recursed on
  non-recursive mutex"; on production kernels the ownership KASSERT
  is compiled out, the mutex silently recurses, and the nested ACK
  processing mutates the connection under the suspended outer
  `tcp_output()`, corrupting its send-buffer snapshot - a delayed
  crash. This is why `lo(4)` has always used `netisr_queue()`
  (`if_loop.c`: "mbuf is free'd on failure") and part of why
  `epair(4)` decouples transmit from receive with its own queues
  (not only `ether_input()` context requirements, as an earlier
  version of these notes claimed).
- **Parallelism comes from a driver-private pinned worker pool**
  (implemented 2026-08-15): one taskqueue with one CPU-pinned thread
  per CPU (`pair_task_N`), created at `SI_SUB_TASKQ` and shared by all
  pairs. Each side owns one `mbufq` receive queue per worker
  (`PAIR_QLIMIT` 4096, epair's `RXRSIZE`), with epair's
  IDLE/WAKING/RUNNING state machine and its flush-once-per-run
  anti-starvation guard. Steering: by mbuf `flowid` when present
  (locally originated TCP/UDP carries one - `ip_output()` stamps
  `inp_flowid`), falling back to a per-side round-robin static
  assignment. Three deliberate improvements over epair: (1) epair has
  the per-CPU pinned pool **only on RSS kernels** - on GENERIC it runs
  a single unpinned worker (a fact discovered late; earlier versions
  of these notes wrongly credited GENERIC epair with per-CPU
  spreading); we pin unconditionally. (2) per-flow steering on
  GENERIC, where epair collapses to one queue. (3) pool lifecycle in
  `SYSINIT/SYSUNINIT(SI_SUB_TASKQ)` instead of `MOD_UNLOAD` -
  `kern_linker.c` fires module events *before* file SYSUNINITs, so
  epair frees its pool while its cloner (and any live pairs) still
  exist; our ordering keeps the pool alive until after cloner
  teardown. Also unlike epair: no `sched_bind()` of the loading
  thread for NUMA locality - with the module preloaded from
  loader.conf, SYSINITs run before `SI_SUB_SMP` releases the APs and
  binding to an offline CPU hangs the boot. `net.isr` tuning is now
  irrelevant to if_pair (netisr is only involved if the admin sets
  deferred dispatch, in which case its rcvif serialization covers our
  packets).
- **gtaskqueue(9) evaluated and declined** (2026-08-15): FreeBSD's
  per-CPU task groups (`TASKQGROUP_DEFINE`; the shared
  `qgroup_softirq` runs epoch callbacks and linuxkpi tasklets, and
  `if_wg` uses a private group for its crypto) are the same
  primitive class as our pool - so the pool is a choice among
  existing options, not a workaround for a missing API. Switching
  would gain nothing (identical structure, task wakeups already
  amortized per burst) and cost three things: taskqgroup threads are
  hardcoded to `PI_SOFT` (`subr_gtaskqueue.c` `taskqgroup_cpu_create`)
  vs our `PI_NET` (= `PI_INTR`, one class higher - correct for
  threads that run the peer's protocol input), there is no
  `NET_GROUPTASK` so the epoch wrap becomes hand-maintained, and the
  boot-preload lifecycle analysis would need redoing. Sharing
  `qgroup_softirq` is rejected outright: line-rate packet work must
  not be able to starve the stack's epoch reclamation callbacks.
  Priority landscape of the pinned workers (audited 2026-08-15):
  above `PI_NET` sits only the `PI_REALTIME` class (clock/AV
  ithreads, `intr_priority()` in `kern_intr.c`) plus raw interrupt
  filters - short-duration by construction; equal-priority NIC
  ithreads and netisr share round-robin (deliberate parity - the
  workers run a peer's protocol input); userland, including rtprio,
  can never preempt them (the realtime user band is below the ithread
  band). The inversion matters more: a worker saturating its CPU
  starves that CPU's `PI_SOFT` residents - softclock (TCP timers'
  callouts) and `qgroup_softirq` (epoch callbacks) - exactly as any
  saturated NIC ithread always has; measured worker load (28-54%
  at benchmark saturation) leaves ample gaps in practice.
  **UPDATE 2026-08-20: "ample gaps in practice" did not survive big
  iron.**  On a 128-core arm64 server, `iperf3 -P 40` between two
  jails saturated ~35 workers at 99.8% (the flow-hash birthday math,
  load average ~35 with the other ~90 CPUs idle), the callout
  threads - top priority -54 vs the workers' -55 - got ~30% of a
  CPU, TCP timers stalled machine-wide, interactive ssh froze and
  iperf3's own control connection timed out.  Full evidence in
  `samples/starve.log` (batch-mode top + procstat, captured through
  the freeze with a pre-launched daemon(8) logger); an 8-core VM
  never shows this because client and server compete with the
  workers for the same CPUs and the feedback loop self-throttles.
  FIXED 2026-08-20 with NAPI-style dual budgets: workers
  kern_yield(PRI_USER) on the earlier of `net.link.pair.batch`
  packets (default 64, CTLFLAG_RWTUN - both a loader tunable and a
  runtime sysctl; values above PAIR_QLIMIT are clamped with a
  console warning, <= 0 disables yielding entirely) or a hardclock
  tick elapsing mid-batch, detected per packet by a getsbinuptime()
  comparison (~ns: it reads the cached per-tick snapshot, and a
  change in it IS the deadline signal - a tick fired, callouts may
  be pending).  The yield drops the worker below the callout
  threads and userland for one scheduling decision.  Sizing: the
  tick check bounds callout lateness to one tick plus one packet
  regardless of per-packet cost (a pure count budget stretches with
  MTU - 64 x ~20 us at mtu 65535 overruns the 1 ms tick); the count
  budget provides sub-tick fairness to userland and remains the
  effective bound at hz=100 VM guests (10 ms ticks).  An
  uncontended yield resumes in well under a microsecond
  (sub-percent overhead); Linux's NAPI uses the same shape (64
  packets + a 2 ms jiffies time budget) for the same problem, with
  lighter per-packet work.  Tick-triggered yields are counted in the
  read-only counter(9) sysctl `net.link.pair.batch_overruns`
  (per-CPU slots match the pinned workers; COUNTER_U64_DEFINE_EARLY
  avoids the window where a module's sysctl is visible before a
  SYSINIT-time counter_u64_alloc() has run); count-budget yields are
  deliberately not counted - yielding between batches is normal
  under load, an overrun means a batch outlived a callout deadline.
  The yield is legal inside the NET_TASK epoch section (a voluntary
  yield takes the same mi_switch() path as the involuntary
  preemption EPOCH_PREEMPT is designed for) and happens with no
  locks held.  Needs revalidation on the 128-core box and one
  INVARIANTS pass.
- **No stack-depth guard needed**: since transmit never delivers
  inline, chained pairs and routing loops cannot grow the kernel
  stack; each hop is a fresh pass of the next queue's worker task.
  (An earlier design direct-dispatched with a `GET_STACK_USAGE()`
  guard; deferral subsumes it.)

#### Future work: TSO/LRO emulation (researched 2026-08-15, not implemented)

Deliver-whole TSO (advertise `IFCAP_TSO`, hand the peer the unsplit
frame) + `tcp_lro(9)` aggregation could cut stack traversals ~4x for
bulk TCP. In-tree precedent: `if_tuntap.c`'s virtio-net-header
support (embedded `struct lro_ctrl`, `tcp_lro_rx`/`tcp_lro_flush_all`
on input, `virtio_net_tx_offload()` passing TSO frames whole);
`if_vxlan.c` for `if_hw_tsomax` arithmetic. Archaeology: lo(4) never
attempted TSO (`git log -S TSO -- if_loop.c` is empty; its perf work
ended with `3cb73e3d8bda`, the 2009 checksum-avoidance commit,
+37%/+74% measured) - absence of attempts, not a known dead end; the
technique's only in-tree outing (tap, for bhyve) shipped and works.
Open homework before implementing: behavior of a forwarded
`CSUM_TSO` frame reaching a non-TSO egress, and the LRO/checksum-flag
interplay with our keep-request-bits contract. Cheap ceiling
measurement first: `mtu 65535` on both sides approximates
deliver-whole TSO for pair-local TCP; benchmark against 16384 before
building anything. Working hypothesis (and the likely reason lo(4)
never needed TSO): 16K already amortizes per-traversal costs ~11x,
and the remaining loopback-style cost is per-byte - the two socket
copies - which no segment size touches; loopback could also always
raise its MTU freely (no transit, so none of TSO's scoping advantage
applies there).

**MEASURED 2026-08-15 (VM): 64K MTU 89.5 Gbit/s vs 16K MTU
86.2 Gbit/s - +3.8% for 4x fewer traversals. Hypothesis confirmed;
per-byte costs dominate beyond 16K. TSO/LRO emulation is PARKED: its
ceiling for pair-local TCP is this ~4%, which does not justify the
forwarded-CSUM_TSO verification burden and new code. The 16384
default stands, empirically; `mtu 65535` remains available to anyone
who wants the last few percent. This also empirically closes the
lo(4) question: 16K genuinely is nearly as good as TSO for
same-machine traffic.**

#### Future work: transit offload for jail<->world traffic (2026-08-18)

The 2026-08-15 verdict above covers pair-LOCAL traffic only, where
the 16K MTU already batches ~11x.  Transit traffic (jail <-> world,
routed by the host through a 1500-MTU NIC) gets no batching at all:
the remote peer's MSS (~1460) clamps the jail's TCP segments, so
every transit packet crosses the pair at wire size and pays full
per-traversal cost.  LRO/TSO would batch up to ~44 segments per
traversal - order-of-magnitude territory, unlike the local +3.8%.
The "open homework" above (forwarded CSUM_TSO at a non-TSO egress;
LRO vs the keep-request-bits contract) is now done, with sources:

Inbound (world -> jail): deployable today with no driver change.
Enable LRO on the NIC (hardware, or the generic software engine in
sys/netinet/tcp_lro.c that most drivers embed) and run the pair at
mtu 65535: the default aggregate limit is TCP_LRO_LENGTH_MAX =
65280 (tcp_lro.h:200, applied in tcp_lro.c:188), which fits.
Merged frames carry CSUM_DATA_VALID|CSUM_PSEUDO_HDR plus the
computed value (tcp_lro.c:804/817), which composes with our
kept-request-bits contract - the jail's input accepts them as
validated.  The flows terminate in the jail's own TCP stack, which
is what makes LRO on a forwarding box sound here (endpoint
semantics; the Linux GRO+veth container pattern).  The in-tree
stance on LRO-plus-forwarding is exactly one data point: if_bridge
strips IFCAP_LRO from members unconditionally (BRIDGE_IFCAPS_STRIP,
if_bridge.c:204); no driver gates LRO on ipforwarding (tree-wide
grep: zero hits) and ifconfig(8)'s lro text carries no forwarding
warning - so the endpoint-semantics safety argument is ours to
document, not the base system's.  Caveat: any OTHER transit through
the same NIC toward a 1500-MTU egress breaks while LRO is on.

Outbound (jail -> world): needs the pair to advertise IFCAP_TSO;
the base plumbing is ready at every layer except one.
- tcp_maxmtu() (tcp_subr.c:3657; v6 twin :3699) grants TF_TSO iff
  the route's first-hop interface - pairNb, for a jail - has
  IFCAP_TSO in capenable and CSUM_TSO in hwassist, and copies the
  limits from its if_hw_tsomax{,segcount,segsize}: the pair alone
  controls the jail's chain building.  tso_segsz stays the
  remote-clamped MSS (tcp_output.c:1405), so segmentation size is
  always correct regardless of the pair MTU.
- ip_output() passes oversized chains iff csum_flags &
  egress->if_hwassist & CSUM_TSO (ip_output.c:779; the vxlan
  comment there is the in-tree precedent for non-origin TSO
  frames); ip6_output() likewise (ip6_output.c:1132, minus
  extension-header cases).  pf_route() implements the identical
  exemption and balk (pf.c:9298/9318), so pf route-to composes.
- The gap: ip_tryforward() checks bare ip_len <= nh->nh_mtu
  (ip_fastfwd.c:494 - the comment above it promises "or if
  hardware will fragment for us", unimplemented for TSO) and
  ip6_tryforward() likewise (ip6_fastfwd.c:210/230); both consume
  the packet with an ICMP error and never fall back to the slow
  path, so routed TSO dies on the default forwarding path today.
  Fix: mirror ip_output()'s hwassist test - a few lines, twice.
  Upstream item below.
- The failure mode at a non-TSO egress dictates default-off:
  tcp_output()'s EMSGSIZE handler self-heals by clearing TF_TSO
  (tcp_output.c:1694-1708; its comment anticipates exactly the
  non-TSO-egress case) - but only when its OWN ip_output() call
  fails.  In the routed topology the failure surfaces at the
  host's forwarding hop and returns as ICMP needfrag;
  tcp_mss_update() re-probes tcp_maxmtu(), which still reports the
  TSO-capable pair, TF_TSO survives, and the connection stalls.
  So pair TSO must ship default-off, enabled by an operator who
  knows the egress NIC can take it - consistent with vlan(4)
  (inherits parent TSO and maintains limits via
  if_hw_tsomax_common()/_update(), if_vlan.c:2082-2127 - the
  ready-made KPI for our advertised limits) and if_bridge (TSO
  kept only when every member supports it, bridge_mutecaps()).
  lo(4) has no TSO (if_loop.c:133), so an advertising pair goes
  beyond its role model - worth stating in review.

Benchmarking note (2026-08-15): an apparent directional throughput
asymmetry turned out to be an iperf3 `--bidir` artifact (both
directions share one client process; its CPU saturation throttles
unevenly) - use two separate unidirectional runs or two independent
client/server pairs. For the record, the driver's one real per-side
asymmetry is `sc_defqid` (flowid-less traffic serializes onto a
different pinned worker per side); a software flow-hash fallback that
would erase it is sketched in the flow-steering design discussion and
remains unimplemented for lack of a demonstrated need.

Pool observability capture (2026-08-15, VM, `top -SHPaziocpu` during
`iperf3 -P 8`): pinning and worker visibility confirmed
(`pair_task_N` threads of proc 0, bound to their CPUs). Finding: only
TWO workers active for 8 flows - pair-local connections never acquire
an `inp_flowid` on non-RSS systems (no NIC ever stamps one), so ALL
traffic takes the per-side `sc_defqid` static fallback. The
client-side worker saturates at ~100% (ACK delivery drives the
senders' tcp_output() in that worker, serializing all flows' transmit
processing) while five CPUs idle - the demonstrated need for the
previously parked software flow-hash steering fallback.
**Implemented same day**: `pair_hash_mbuf()` - jenkins_hash32 over
src/dst address, IP protocol and (when safely readable) TCP/UDP
ports, random per-boot seed, for both address families; fragments
hash without ports (all fragments of a datagram stay on one queue);
IPv6 extension chains are not walked (L3-only hash then); headers
read via `m_copydata()` (no contiguity/mappedness assumptions -
handles M_EXTPG); result written back as `M_HASHTYPE_OPAQUE_HASH` so
the peer stack and further hops inherit it; unhashable packets take
the static default queue. Retest criterion: the same `-P 8` run
should light up (up to) 8 `pair_task_N` threads in `top -SHPaziocpu`
and lift the single-worker throughput ceiling.

**Retested 2026-08-15 - flow hashing works:**

- `-P 8`: **128 Gbit/s** (up from ~86), six distinct workers active at
  28-48% (8 flows over 8 buckets: E[distinct] = 8(1-(7/8)^8) ~ 5.2 -
  birthday math, observed 6), all 8 CPUs ~100% system, zero idle: the
  VM is now machine-saturated, not worker-serialized.
- Single flow: 60 Gbit/s typical, occasional ~42. Capture shows the
  bottleneck is the *server iperf3 thread* at 100% (soreceive copyout
  - the per-byte cost that owns this workload), with the sole active
  worker at only 54%: the driver is not the limiter.
- **Emergent finding - direction convergence**: only ONE worker
  serves both directions of a connection. The hash writeback is
  learned by the receiving TCP (`inp_flowid` from our
  `M_HASHTYPE_OPAQUE_HASH`), so its ACKs return carrying the SAME
  flowid; both directions then map to the same queue index = same
  pinned worker. Not designed, but desirable: one worker per
  connection (better many-connection capacity, whole-connection cache
  locality), at the cost of the old static split's accidental
  two-worker direction pipelining for a single flow.
- The occasional 42 Gbit/s: **confirmed by capture**
  (top-slow-single.txt): the iperf3 server thread (64%) and
  `pair_task_0` (36%) sharing CPU 0 at 98% combined while five CPUs
  idle - and 0.64 x 60 Gbit/s ~ the observed 42. Mechanism: *wakeup
  affinity*, not random placement - the worker wakes the receiver
  out of sbwait, ULE places the wakee near its waker (the worker's
  pinned CPU), the short copyout burst ends in sleep before idle
  stealing can migrate it, and the next wakeup re-plants it. The
  regime is *metastable*, not permanent: idle stealing never gets a
  window, but ULE's periodic load balancer eventually does - observed
  recovery mid-run after tens of seconds, 42 -> ~62 Gbit/s, once the
  userland thread was migrated off the worker's CPU. Short runs
  therefore look bimodal; long runs show phases. Inherent
  to pinned-kernel-waker vs floating-userland-wakee; disappears with
  multiple flows (see -P 8: machine-saturated) or `cpuset(1)` on the
  application. No driver-side fix is appropriate: choosing workers
  by userland thread location is RFS territory the kernel does not
  offer, and single-flow throughput remains copy-bound either way.

#### Future work: NUMA-aware flow steering (designed 2026-08-15, parked)

Goal: make it highly unlikely that a new flow's worker lives in a
different NUMA domain than its sender. Design (small, ordering-safe,
ready to build when a multi-domain if_pair host exists to measure on):

1. At pool init, build per-domain worker tables from
   `pcpu_find(cpu)->pc_domain` (`pcpu.h`): `pt_domain_qids[dom][]`.
2. In `pair_hash_mbuf()` - the only moment steering is decided, on a
   flow's first packet - read the transmitting CPU's domain
   (`PCPU_GET(domain)`; also where the sender's socket-buffer pages
   were first-touched) and pick a worker within it:
   `qid = pt_domain_qids[dom][hash % count[dom]]`.
3. Encode the choice into the written-back flowid so stickiness lives
   in the flow, not in curcpu: `flowid = hash - (hash % pt_count) +
   qid` - residue selects the worker through the existing modulo
   path, high bits keep jenkins entropy for downstream consumers.

Properties: per-flow ordering absolute (sender migration degrades
locality, never order); the flowid-reflection loop keeps BOTH
directions of a connection on that one in-domain worker, and wakeup
affinity then tends to pull the receiving application into the same
domain; single-domain machines degenerate to current behavior; empty
domains fall back to the global modulo. Companion change: allocate
each `pair_queue` with `malloc_domainset(9)` /
`DOMAINSET_PREF(domain_of(qid))` so every worker's hot mutex+mbufq
live in its own domain - this also discharges the NUMA debt from
dropping epair's `sched_bind()` allocation trick at pool init
(boot-preload safety).

Bounded honestly: benefits only multi-domain hardware (the test VM is
single-domain - unmeasurable there); the worker touches headers,
queue structures and socket-buffer bookkeeping, while the dominant
per-byte copyout runs in the application's thread, whose placement
the scheduler owns. Parked per project discipline: no optimization
without a demonstrated need and a machine to measure it on.

#### Big-iron scaling investigation (2026-08-20/21)

Symptom (t_17 connection sweep, 5 s per point, receiver averages in
Gbit/s): an 8-core VM behaves classically - climbs to a peak at
P=8 (122), declines gently (67.9 at P=128).  A 128-core arm64
server (Ampere, bare metal, GENERIC) peaks at the SAME P=8 (89.9)
despite 16x the cores, then collapses: 72.5 at 16, 48.9 at 32,
19.5 at P=128 - HALF its own single-connection rate, with ~90 CPUs
idle.  Both systems stayed fully responsive throughout (the
batch/yield fix doing its job).  Three hypotheses: (A) cross-CPU
coordination costs, (B) queue overflow feeding TCP loss collapse,
(C) the yield donating worker time to co-located userland.

Elimination chain, one instrument per step (evidence in samples/):

- t_18 (batch sweep 16/64/256, t18_*.txt): throughput is
  batch-invariant on the Ampere (<2% spread at every connection
  count across a 16x yield-frequency range) - hypothesis C dead.
  Bonus: batch 64 is best-or-tied on the VM; the default stands.
- batch_overruns reinterpreted: a worker can overrun at most ~once
  per tick, so the counter integrates busy-worker time (~hz x sum
  of duty cycles).  Dividing throughput by it gives per-busy-worker
  efficiency on the Ampere: ~10.5 Gbit/s per worker at P=8, 0.78
  at P=64, 0.28 at P=128 - a 37x efficiency collapse while fully
  busy.  (VM overruns are 100x lower: batches almost never outlive
  a tick there.)
- t_18 with per-run drop deltas (t18_*_a.txt): ZERO oqdrops and
  ZERO idrop on the Ampere at every point of the collapse, while
  the VM - declining gently - does drop at P>=64.  The inversion
  kills hypothesis B: loss is not the mechanism.
- t_19 (profile-497 kernel-PC sampling, t19_*.txt): the VM at P=64
  is a healthy saturated profile (52% copy, copycommon 49.8%,
  locks 13.5%, sched ~0).  The Ampere: lock_delay 30.8% of
  non-idle cycles, sched 0.0% (the wakeup-IPI variant of A:
  refuted), 33% idle - and a surprise: ipsec_kmod_hdrsize 21.8%,
  ipsec_kmod_output/input/check_policy/capability ~30% more, ~52%
  total.  The server runs ipsec.ko in production for BGP TCP-MD5
  session protection (two mature tcp-md5 SAs, empty SPD): once
  loaded, its hooks tax EVERY packet - traffic no policy will ever
  match.
- lockstat spin capture during t_19 (dtrace-tail.txt): ~4.9M lock
  spins in 10 s, top sites in_pcblookup_hash_smr (inpcb lock
  acquisition after the lockless SMR lookup, 1.75M),
  soreceive/sosend_generic_locked (sockbuf locks, 2.9M combined)
  and callout_reset_sbt_on (cross-CPU timer wheel locks, 0.23M).
  NO ipsec frames among the spins.

Conclusion - two independent taxes, neither a driver bug:

1. A ~52% per-packet cycle tax from the loaded ipsec.ko's hooks,
   which converts into throughput loss once workers saturate
   (P=1 is unaffected - see the A/B below): measured at 2.5x peak
   and 7x high-connection throughput on this machine.  Upstream-
   worthy on its own; see open item (c) below.
2. The scaling collapse itself: per-connection lock ping-pong.
   Each connection's inpcb, socket-buffer and callout locks are
   handed around a triangle - sender thread (sosend), pinned
   worker (whole TCP input/output in worker context), receiver
   thread (soreceive) - each on a different CPU of a 128-core
   coherence mesh.  Spin cost per handoff grows with topological
   distance and participant count; on the 8-core VM the same dance
   is nearly free inside one cache complex.  Honest driver-side
   admission: if_pair's deferred-worker design INSERTS the third
   party into that triangle (lo(4) delivers inline in the sender's
   context and keeps its locality), so part of tax 2 is the price
   of the architecture that fixed the inline-dispatch panic and
   bought small-system parallelism.  Corollary: the VM's
   "metastable 42 Gbit/s single-flow dip" (2026-08-15, above) was
   ULE co-locating worker and application - locality WORKING, not
   a scheduler quirk.

A/B confirmation (2026-08-21, t17_ampere_noipsec.txt): unloading
ipsec.ko was confirmed operationally safe on the Ampere and t_17
rerun without it.  Peak 89.9 -> 223 Gbit/s (moving from P=8 to
P=16), P=128 19.5 -> 140 (7.2x), falloff softened from -78% to
-37% off peak, still zero drops - the curve is now classic and the
128-core machine finally outruns the 8-core VM.  Tax 1 is thereby
causally confirmed and was the dominant scaling killer.  Two model
refinements: P=1 was UNCHANGED (within the 30-37 run spread), so
the hook tax converts into throughput loss only once workers
saturate - a single flow's worker absorbs it in idle headroom (an
earlier draft of this section wrongly blamed the tax for the
single-connection gap).  And tax 2 persists exactly as predicted:
at P=128 the overruns still integrate to ~77 busy CPU-equivalents
moving 140 Gbit/s (~1.8 Gbit/s per worker vs ~30 at the P=16
peak), so per-worker efficiency still collapses ~16x - the lock
triangle remains the residual, now-gentle falloff.  The post-unload
P=64 profile (t19_ampere_noipsec.txt) makes that residual vivid:
lock_delay is 86.9% of non-idle cycles (locks bucket 90.2%, copy
6.4%, protocol work 0.7%, idle down to 9.7%) - with the IPsec
cycle sink gone and packet rates 4x higher, tax 2 monopolizes the
machine; the remaining ceiling is pure lock contention and the
productive cycles are a sliver, which also quantifies the upside
of the LRO/steering levers below.  Supporting actor visible in the
top frames: mbuf cluster refcount atomics (mb_dupcl/mb_free_ext -
tcp_m_copym reference-shares clusters with the retransmit queue,
and the worker drops those references from another CPU).

Open items: (a) lo(4) baseline sweep on the Ampere (same stack, no
worker triangle) to apportion the residual tax 2 between platform
TCP behavior and if_pair's indirection; (b) if (a) implicates the
triangle: software LRO on the receive batches (divides per-packet
lock traffic by the aggregation factor; the in-tree tcp_lro engine
fits the worker's existing batch structure), sticky
transmit-CPU steering as an opt-in policy (costs single-flow
pipelining, helps flow-heavy big iron), and/or a capped worker set
sysctl, all as measured future work; (c) consider an upstream
report for tax 1 with these numbers - a 2.5x peak / 7x
high-connection cost on unrelated local traffic from merely
loading ipsec.ko for BGP TCP-MD5 affects every large FreeBSD
router.  Not blockers: both machines stay responsive, small
systems behave classically, and the remaining falloff is the
gentle kind operators expect.

#### Postmortem: iperf3 panic (2026-08-14)

First load of the module survived ping but panicked under iperf3
between two jails: page fault in `tcp_default_output()` (NULL mbuf
`m_len` read, i.e. a send-buffer chain walked past its end) in the
netisr thread, after the inline-dispatch reentry described above had
silently recursed an inpcb lock and corrupted the connection's
send-buffer state. Root cause: the original design direct-dispatched
into the peer's stack for performance, misreading `lo(4)`'s
always-queue behavior as legacy rather than load-bearing. Fixed by
unconditionally queueing (this section describes the corrected
design).  The captured backtrace from that panic is preserved as
`samples/crash.txt`.

#### Runtime test log

- 2026-08-14, NAS: ping OK; iperf3 panicked (see postmortem above).
- 2026-08-15, VM: always-queue design survives iperf3 with 1-30
  parallel connections. Found: `ifconfig pair0a destroy` returned
  `EINVAL` - `if_clone_destroy()` resolves the owning cloner via
  `ifp->if_dname` (`ifc_find_cloner_in_vnet()`), and we had set
  `if_dname` to the full "pairNa". Fixed by keeping `if_dname` =
  "pair" and putting the full name only in `if_xname`
  (`if_setname()`), as epair does (`if_epair.c:625-626`).
- 2026-08-15, VM (continued): destroy via `pair0b` failed with ENXIO -
  only the create-returned `a` ifp is linked into the cloner list (and
  the `pair` interface group!) by the framework; the `b` side needs an
  explicit `if_clone_addif()`, as epair does (`epair_clone_add()`).
  Fixed, and adopted modern epair's either-side destroy semantics
  (nested `if_clone_destroyif()` for the partner with a cleared-softc
  recursion guard) - the man page's old claim that b-side refusal
  matched epair was stale lore. Bonus fix: `b` sides are now actually
  in interface group `pair`, so `on pair` firewall rules see them.
  Retest: destroy confirmed working from either side.
- 2026-08-15, VM: destroy-under-load PASSED - pair side destroyed
  inside the iperf3 server jail while `iperf3 -c ... -P 4` ran from
  the peer jail; no panic. First live exercise of the quiesce
  protocol (both-sides-down + `NET_EPOCH_WAIT()`) against in-flight
  bidirectional transmitters, and of netisr's `m_rcvif_restore()`
  drop path for packets queued at destroy time.
- 2026-08-15, VM: `kldunload if_pair` during `iperf3 -P 4` at
  ~100 Gb/s between the jails PASSED - module-unload teardown
  (`VNET_SYSUNINIT` -> `if_clone_detach` walking a list holding both
  siblings per pair, nested-destroy recursion guard) destroyed both
  interfaces cleanly under load; iperf3 fell to zero, both sides
  vanished from the jails. The ~100 Gb/s figure (VM on a laptop,
  16384 MTU, hot caches) is an observation, not a benchmark, but
  shows the always-queue datapath is not a bottleneck at these
  rates.
- 2026-08-15: pinned per-CPU worker pool implemented (see performance
  section). NOT yet runtime-tested - the full VM battery (smoke,
  iperf3 reproducer, destroy-under-load, kldunload-under-load, churn)
  must be re-run before this design is trusted; the always-queue
  netisr design was the last one validated. Workers are visible as
  `pair_task_N` in `top -SH`; multi-stream iperf3 should now spread
  across them.
- 2026-08-15, VM: interface moved OUT of a jail back to the host
  (manual `if_vmove` - the same path `vnet_if_return` takes on jail
  death), IPv4 config reapplied (addresses are stripped on any vnet
  move; standard behavior), then host-to-jail iperf3 PASSED - first
  host<->jail traffic validation, and confirms a pair migrates between
  vnets with no driver-side fixup needed.
- 2026-08-17, VM (arm64), GENERIC: full shell test suite
  (tests/t_*.sh, 14 tests - creation properties, either-side
  destroy, unit/name corner cases, MTU bounds, host<->jail IPv4 and
  IPv6, jail<->jail, vmove group restoration, up/down gating,
  txcsum toggles, destroy under flood load, 30-pair churn,
  create-vs-destroy race stress, unload with live and jailed pairs
  including the four-line verification) PASSED.  First runtime
  validation of the pair_sx convergent destroy, the unload barrier,
  and the wait-retry.
- 2026-08-17, VM (arm64), GENERIC-DEBUG (WITNESS+INVARIANTS): suite
  PASSED with zero WITNESS output naming an if_pair lock - the
  first machine check of the audited lock orders, including
  pause() while holding pair_sx plus the caller's
  ifnet_detach_sxlock in the wait-retry, and the recursive pair_sx
  claim.  The run surfaced the base vnet teardown LOR (audit
  finding 4), fixed by the MOD_UNLOAD registry sweep; a fresh-boot
  debug rerun with the sweep produced no LOR at all, confirming
  if_pair no longer records the reversing order.  Debug-kernel
  throughput collapses under parallel load by design (WITNESS's
  global lock serializes every lock operation) - not performance
  data.
- **Checksum elision**: the interfaces advertise TX/RX checksum offload
  for TCP/UDP over IPv4 and IPv6 (toggleable via `ifconfig ...
  [-]txcsum`) - the exact same `if_hwassist` set as `epair(4)`.
  Checksums the sending stack requests from "hardware" are never
  computed for traffic terminating in the peer vnet. epair(4)
  documents this same-host contract ("the checksum is unnecessary and
  will be ignored if offloaded; such packets contain an incorrect
  checksum") and mbuf(9) documents the `csum_flags`/`csum_data`
  semantics, including `csum_data` holding the checksum field offset
  while a request is pending. Commit `bcb298fa9e23` introduced the
  input-path acceptance and forwarding-path completion ("such packets
  never have been on the wire"); commit `39d4094173f9` added the
  matching epair support. The mechanism (verified against the
  15.0 sources, same as epair's): the mbuf crosses the pair with its
  TX request bits *kept* and `csum_data` untouched. The TCP/UDP input
  paths accept a pending request bit as "packet from local host,
  checksum not required" (`tcp_input()`/`udp_input()` and IPv6
  variants; this input-path mechanism itself is undocumented). If the packet is
  instead forwarded toward a real interface,
  `ip_tryforward()`/`ip6_tryforward()`/`ip_output()`/pf's `pf_route()`
  all complete pending checksums at the true egress (NIC hardware or
  `in_delayed_cksum()`), so packets never leave the machine with
  invalid checksums, and elision composes across chained pairs: the
  checksum is computed exactly once, at the real edge, or never.
  Never set `CSUM_DATA_VALID | CSUM_PSEUDO_HDR` here - the input paths
  then read `csum_data` as the hardware-computed checksum value, but it
  holds the checksum field offset. Two offloads are deliberately NOT
  advertised: SCTP CRC (for epair parity and lack of demand, not
  safety - see the 2026-08-20 re-analysis below) and
  the IPv4 header checksum, `CSUM_IP` (`divert_packet()` completes
  pending L4 checksums before a packet reaches a divert(4) socket, but
  not `ip_sum` - an elided header sum would reach natd(8) as garbage
  and be dropped on inbound reinjection; divert(4) documents exactly
  this contract: "Packets written as incoming and having incorrect
  checksums will be dropped"). Since `CSUM_IP` is never
  pending, `pair_csum_vouch()` vouches `CSUM_IP_CHECKED |
  CSUM_IP_VALID` unconditionally, sparing the peer's `ip_input()` a
  software verification.
- **SCTP CRC re-analysis (2026-08-20)**: the original exclusion
  rationale ("completion hooks exist only in kernels built with SCTP
  support, which an out-of-tree module cannot assume") was wrong.
  Pending SCTP CRCs are produced only by the SCTP stack itself,
  whose protocol hooks (in_proto.c:115) require the
  SCTP/SCTP_SUPPORT kernel option - and the same option compiles
  sctp_delayed_cksum() completion into ip_output() (:765),
  ip_tryforward() (ip_fastfwd.c:106/477), pf_route() and
  divert_packet() (ip_divert.c:200 - so the natd(8) failure mode
  that forced the CSUM_IP exclusion does not exist for SCTP), while
  sctp_crc32.c itself is "optional inet | inet6" (conf/files:4421)
  and thus present in practically every kernel.  Producer implies
  completer: a kernel able to generate a pending SCTP CRC can always
  complete it, so advertising CSUM_IP_SCTP would be safe
  unconditionally - no compile-time gate or runtime probe needed.
  (For the record: config.mk does generate opt_sctp.h for
  out-of-tree builds - "#define SCTP_SUPPORT 1" when the src tree
  has MK_SCTP_SUPPORT, or symlinked from the real kernel build dir
  under KERNBUILDDIR - but that tests the build environment, not the
  running kernel, and turns out to be unnecessary here.  GENERIC
  ships SCTP_SUPPORT.)  The bit stays off for epair parity and lack
  of demand; the one open item before ever advertising it is whether
  libalias' alias_sctp.c recomputes the CRC from scratch after NAT
  rewriting - if it does, even the ipfw-nat caveat would not apply
  to SCTP.
- **pf**: needs no special driver support (verified against
  `sys/netpfil/pf` in 15.0). pf attaches to interfaces generically via
  pfil/pfi hooks; both sides of a pair are in interface group `pair`
  for `on pair` rules (the `a` side automatically via the cloner
  framework, the `b` side via our explicit `if_clone_addif()` - until
  the 2026-08-15 fix the `b` side was in no group and `on pair` rules
  silently missed it). pf's NAT/rewrite helpers detect pending
  checksums (`CSUM_DELAY_DATA*`) and adapt instead of corrupting them,
  and `pf_route`/`pf_route6` (route-to) perform the same
  hwassist-aware checksum completion as `ip_output()`, including SCTP.
  `pair_output()` releases `CSUM_SND_TAG` send tags (as epair does)
  since `snd_tag` shares union space with `rcvif` in the pkthdr.
- **ipfw** (all kernel modules audited against 15.0 sources):
  - *Compatible - verified*: plain filtering, dynamic rules and table
    lookups (never read checksum bytes); `fwd`; **dummynet** - every
    reinjection case in `dummynet_send()` re-enters
    `ip_output(IP_FORWARDING)`/`ip6_output()` (checksum completion) or
    netisr->input (request bits honored); the `PROTO_LAYER2`/`PROTO_IFB`
    cases are unreachable for non-Ethernet interfaces; the QoS
    classifiers (ipfw opcodes, `fq_codel`/`fq_pie` flow hashing) and
    AQM modules (codel/pie) only *read* header fields, never bytes that
    could be pending; **divert/natd and `tee`** for L4
    (`divert_packet()` completes TCP/UDP/SCTP for both families before
    userland, on the `m_dup`'d copy too since `m_dup` copies pkthdr
    flags; `CSUM_IP` dropped from our hwassist for this - see above);
    **pmod/tcpmod** (MSS clamping) - explicitly offload-aware: skips
    its differential fixup when the checksum is pending, which is
    correct since the pseudo-header partial doesn't cover option
    bytes; **ng_ipfw** (reinjects via `ip_input()`/
    `ip_output(IP_FORWARDING)`); **nat64** (stl/lsn/clat all use the
    common `nat64_translate` core, which completes delayed checksums
    before translating); **nptv6** (RFC 6296 translation is
    checksum-neutral, hence also neutral for pending pseudo-header
    partials); fragmentation (`ip_fragment()` completes L4 checksums
    before splitting, so large-MTU UDP fragments cross the pair with
    valid bytes).
  - *Known upstream bug, not fixable in the driver*: libalias-based
    in-kernel NAT (`ipfw nat` and `ng_nat`) corrupts pending delayed
    checksums on traffic forwarded from a pair (or an epair - stock
    FreeBSD has the same exposure). natd(8) via divert is *not*
    affected (divert completes checksums first). Workaround:
    `ifconfig pairNb -txcsum` on pairs whose forwarded traffic passes
    through `ipfw nat` or `ng_nat`. Full analysis below.
- **MTU**: the default is 16384, matching `lo(4)` - with no Ethernet
  framing constraint, a large MTU is the cheapest way to boost bulk TCP
  throughput between vnets (fewer stack traversals per byte, similar in
  effect to TSO on loopback), and 16384 stays clear of 16-bit IP
  length-field edge cases while capturing most of the gain. Anything
  from 72 to 65535 is accepted via `ifconfig pairNa mtu ...` (set both
  sides). For traffic transiting the host toward a 1500-byte uplink,
  ordinary TCP is unaffected (MSS exchange caps segments) and in-host
  PMTUD handles the rest; to bound forwarded traffic without giving up
  the large pair-local MTU, set the MTU on the route instead of the
  interface (`route change default -mtu 1500` in the jail) - see the
  man page's MTU CONFIGURATION section.

### Known upstream bug: libalias in-kernel NAT vs. delayed checksums

Verified against the 15.0 sources (`sys/netpfil/ipfw/ip_fw_nat.c`,
`sys/netgraph/ng_nat.c`, `sys/netinet/libalias/`). This affects stock
`epair(4)` with its default-on `txcsum` identically; if_pair merely
adds a second producer of the packet state that triggers it.

**The packet state.** A stack transmitting through an interface whose
`if_hwassist` claims TCP/UDP checksums does not compute them: the
checksum field holds only the *non-complemented pseudo-header partial*
(`in_pseudo(src, dst, proto+len)`), `CSUM_DELAY_DATA` request bits are
set, and `csum_data` holds the field offset. The completion function,
`in_delayed_cksum()`, later sums the L4 bytes *including the field's
current content* (the partial stands in for the pseudo-header) and
stores the complemented result. The field's invariant while pending:
it must hold the pseudo-header partial consistent with the addresses
currently in the IP header - and it is *not* a wire-format checksum.

**What libalias does to it.** libalias rewrites addresses/ports in the
raw bytes and patches checksum fields with the RFC 1624 differential
update `HC' = ~(~HC + ~m + m')`, which is only valid for complemented,
complete checksums. Applied to the non-complemented partial `P` (using
`~x == -x` in one's-complement arithmetic):

```
result = ~(~P + ~m + m') = P + m - m'
needed = P - m + m'      (partial must track the address change)
error  = 2(m - m')       (correction applied with inverted sign)
```

Port rewrites corrupt it a second, independent way: port bytes lie
*inside* the region `in_delayed_cksum()` will sum, so a pending packet
needs *no* fixup for them at all - libalias applies one anyway. The
packet then proceeds with request bits still set, the egress completion
folds the corrupted partial into the final checksum, and the packet
leaves the machine invalid by a deterministic constant. Symptom:
NAT'ed connections blackhole on data transfer, while tcpdump on the
pair already shows "incorrect" checksums cosmetically (normal for
offloading interfaces), masking the real corruption.

**The existing repair and its too-narrow gate.** `ip_fw_nat.c` knows
about this (its comment admits libalias "does not have any knowledge
about checksum offloading") and carries a repair (`ldt` path): discard
libalias's field edits and *rebuild* the pseudo-header partial from
the post-NAT addresses, reset `csum_data`, and leave the packet
pending (or complete it in software if it was not pending). The repair
is origin-agnostic. But it is gated on:

```c
if (mcl->m_pkthdr.rcvif == NULL &&
    mcl->m_pkthdr.csum_flags & CSUM_DELAY_DATA)
        ldt = 1;
```

**The invalid assumptions:**

1. *libalias*: checksum fields hold complete, wire-format checksums -
   inherited from its userland natd(8) origin, where every packet had
   crossed or was about to cross a wire. (Its own TODO: "make libalias
   mbuf aware".)
2. *`ip_fw_nat`*: a pending checksum implies a locally-originated
   packet mid-`ip_output()` (`rcvif == NULL`); equivalently, *received*
   packets always carry complete checksums. Historically airtight -
   every `if_output` led to a wire. epair-with-txcsum and if_pair
   exist to violate exactly this: their `if_output` is an *input*
   source, producing packets with `rcvif` set and checksums pending.
   The repair would work for them; it is simply never triggered, while
   libalias still corrupts the field.
3. *`ng_nat`*: differential fixups need repair only when libalias
   edited payload (`TH_RES1` mark). That case is handled fully
   offload-aware (`ng_nat.c` rebuilds the pseudo-header and keeps
   `CSUM_TCP` packets pending), but plain address/port rewrites get no
   repair and not even the `rcvif == NULL` pre-pass - which also bites
   locally-originated pending packets reaching `ng_nat` below
   `ip_output()`'s completion point (e.g. via `ng_ether` on a txcsum
   NIC); hence the long-standing folklore "disable checksum offload
   when using netgraph NAT".

Incidentally, libalias also differentially patches `ip_sum` - which
would have been a third instance of the same corruption had if_pair
kept `CSUM_IP` in its hwassist. With it dropped, the header sum
crossing a pair is genuine wire-format bytes and that fixup is valid.

**Fixes.** Driver-side there is none: `pair_output()` cannot know a
packet will later meet libalias, and preemptive completion would
forfeit elision for all traffic. Upstream, both fixes are small:
widen the `ip_fw_nat` gate to `csum_flags & (CSUM_DELAY_DATA |
CSUM_DELAY_DATA_IPV6)` (drop the `rcvif == NULL` conjunct - the
repair rebuilds from final addresses, so origin is irrelevant), and
add the same pre-pass to `ng_nat`. Until then: `-txcsum` on affected
pairs. Testable prediction for the lab box: forwarded epair traffic
through in-kernel `ipfw nat` should exhibit this on stock FreeBSD
today, and `-txcsum` on the epair should make it disappear.

The module builds clean under `-Werror` on FreeBSD 15.0-RELEASE amd64.

Teardown is epoch-synchronized per the epoch(9) contract (wait-then-
free is its documented reclamation pattern; the no-mutexes-across-wait
rule holds on every destroy path): `pair_output()` asserts it runs
within the network epoch (true for all entry paths - verified in the
15.0 sources for the IP stack, netisr workers (via their
`INTR_TYPE_NET` swi, commit `511d1afb6bfe`), and `bpfwrite()`,
which epoch(9) itself does not specify) and checks
its own `IFF_DRV_RUNNING` before dereferencing `sc_peer`;
`pair_clone_destroy()` clears `IFF_DRV_RUNNING` on both sides and then
`NET_EPOCH_WAIT()`s before anything else, so no transmit can hold a
peer reference once teardown proceeds. Packets sitting in the
driver's own pool queues (which carry every pair packet, holding raw
`rcvif` pointers) are handled by ordering, not by weak references:
`pair_drain_queues()` drains each worker task and flushes the queues
*before* either side is detached or freed, so a queued pointer never
outlives its interface. Packets a worker has already pushed onward
into netisr (only under the deferred dispatch policy) are covered by
netisr's own `rcvif` serialization (`m_rcvif_serialize()`
index+generation, revalidated at dequeue). `NET_EPOCH_ASSERT()` is
`INVARIANTS`-only, so on release kernels the assertions document
rather than enforce.

### Claim provenance

Claims about kernel behavior that no man page documents are pinned to
the FreeBSD commits that introduced or last shaped the behavior
(commit messages being the harder-to-reach part of the documentation;
where a commit was an MFC, the original commit to -CURRENT is cited):

| Claim | Commit | Author, date |
|---|---|---|
| TCP/UDP input paths accept pending checksum-request bits; forwarding paths complete them | `bcb298fa9e23` "sctp, tcp, udp: improve deferred computation of checksums" | Timo Völker, 2025-08-01 |
| SCTP delayed-CRC completion in the forwarding paths | `bcb298fa9e23` (same) | - |
| `divert_packet()` completes delayed L4 checksums | `f0cada84b1e2` | Andre Oppermann, 2004-08-03 |
| netisr serializes/revalidates `rcvif` across its queues | `6871de9363e5` (MFC'd as `51f798e761b1`) | Gleb Smirnoff, 2022-01-26 |
| netisr workers run in the net epoch (`INTR_TYPE_NET` swi) | `511d1afb6bfe` | Gleb Smirnoff, 2020-01-23 |
| `NETISR_POLICY_FLOW` semantics | `d4b5cae49bff` | Robert Watson, 2009-06-01 |
| `lo(4)` MTU 16384 and its rationale | `af78195e0024` | David Greenman, 1995-03-04 |
| epair checksum offload: txcsum default-on, forced RXCSUM, TXCSUM synced between ends | `39d4094173f9` | Timo Völker, 2025-09-04 |
| `snd_tag` machinery and its pkthdr placement | `f3e7afe2d7b2` | Hans Petter Selasky, 2017-01-18 |
| `IFC_F_FORCE` cloner flag | `09ee0fc023c0` | Alexander Chernikov, 2022-09-22 |
| nat64 completes delayed checksums | `aaef76e1fd8c` | Andrey Elsukov, 2020-08-05 |
| ng_nat offload-aware repair path | `f74c0dc583d6` | Maxim Sobolev, 2025-08-25 |
| tcpmod (`ipfw_pmod`) introduction | `aac74aeac76d` | Andrey Elsukov, 2017-04-03 |
| `pf_route()` checksum completion | `2bbe8ffc9d0e` (2004 pf import), `078468ede4ef` (CSUM_IP cleanup) | Max Laier / Gleb Smirnoff |

Remaining source-only claims, with no commit-message backing (blame
lands on code moves, imports, or unrelated churn - or the claim is a
negative one no commit can attest): `ip_input()` having no request-bit
shortcut; the `ip_fw_nat.c` `rcvif == NULL` gate and libalias's
offload-unawareness (rationale exists only in the code comments
themselves); the `dummynet_send()` reinjection paths (2010 wholesale
ipfw3 import); `bpfwrite()` entering the net epoch (`bpf.c`, epoch
entry directly before its `if_output` call); epair's a-side-only
destroy semantics; `lo(4)`'s treatment of bpf-injected packets.

Link state (implemented 2026-08-15): both sides report a synthetic
carrier via `pair_set_state()` - `LINK_STATE_UP` coupled with
`IFF_DRV_RUNNING` at attach, `LINK_STATE_DOWN` as the first teardown
step (epair's `epair_set_state()` ordering), with `IFCAP_LINKSTATE`
advertised (lo(4) precedent) and force-enabled. The peer-reflecting
variant (carrier mirrors the other side's admin state - the truer p2p
semantic) is deferred: `SIOCSIFFLAGS` runs outside the network epoch,
so touching `sc_peer` there would reopen the closed teardown race; if
ever wanted, it needs an epoch section (or equivalent) around the
peer access plus a cross-vnet `if_link_state_change()`. Note:
`ifconfig` may not print a `status:` line for a mediumless interface;
the functional consumers (routing daemons, devd, route-socket
listeners) receive the state regardless - VM test should verify with
`ifconfig -v` / `route -n monitor` during create/destroy.

Unload safety (implemented 2026-08-17, resolving review blocker B2):
all control-plane operations - create, destroy, and the module unload
gate - serialize under one recursive sx (`pair_sx`), which the packet
path never touches (the user-set rate hierarchy: line-rate packet
work unaffected; human-paced create/destroy pay one lock; unload a
few times per uptime). Destroy is now convergent under any
interleaving of ioctl, vnet teardown and kldunload: the first
destroyer through `pair_sx` with a live softc owns the teardown and
clears both softcs (after the quiesce - clearing earlier would race
producers between their RUNNING check and if_getsoftc()); everyone
else no-ops on NULL; the nested partner unlink handles ENXIO via the
wait-retry dichotomy (audit finding 2: create window -> wait for the
pending addif; unload interleaving -> tolerate); the old panic() is
gone (KASSERT only). `MOD_QUIESCE` flips `pair_unloading` under the
same sx, making the flag a barrier that closes the create-vs-unload
orphan window: any create either completed before the flip (so the
pair is in the registry and the MOD_UNLOAD sweep destroys it) or
refuses with ENXIO. Lock order is uniform: destroy callers and vnet
teardown hold `ifnet_detach_sxlock` before `pair_sx`, and the
MOD_UNLOAD sweep takes the two in that same order itself - the
reverse order is never taken.  Worst-case control-op latency: one
destroy's epoch wait + queue drains, tens of ms.  (An earlier note
here proposed if_clone_detach() taking ifnet_detach_sxlock as the
upstream fix; that analysis matured into audit finding 4 and the
vnet_deregister_sysuninit() ordering fix described below.)

Forced unload (`kldunload -f`): an extreme measure, not normal
operation - the supported path is destroying pairs (or letting the
unload's MOD_UNLOAD sweep do it) and a plain `kldunload`.  The driver
nevertheless stays safe under force.  What `-f` actually changes on
15.0 (kern_linker.c `linker_file_unload()`): `MOD_QUIESCE` still
fires - only its veto is ignored - while `MOD_UNLOAD`'s veto is
honored even when forced, so forced unload is not irresistible; a
module can refuse it there.  if_pair does not use that veto
(returning an error would strand the module quiesced but loaded,
and epair(4) does not either).  The creation barrier is flipped in
`MOD_UNLOAD` as well as `MOD_QUIESCE` (the former runs even under
`-f`, before the SYSUNINITs): older linkers skipped the quiesce
loop entirely when forced, and the double flip keeps the barrier
independent of linker behavior.  The convergent destroy protocol
needs no caller cooperation.  Forced unload is deliberately not
part of the routine VM test battery.

Locking audit (2026-08-17) findings: (1) FIXED - the unloading
barrier is flipped in BOTH MOD_QUIESCE and MOD_UNLOAD.  (On 15.0 a
forced unload fires MOD_QUIESCE and merely ignores its veto, but
older linkers skipped the quiesce loop under force; MOD_UNLOAD runs
even when forced, still before the SYSUNINITs, so the double flip
keeps the barrier independent of linker behavior.) (2) Documented,
framework-shaped,
epair-shared: the create-return window - the framework
(`if_clone_createif_nl()`) links the returned 'a' side only after
create_f returns and pair_sx is released, while 'b' is destroyable
from inside create.  A destroy via 'b' that wins that gap
nested-destroys the not-yet-linked 'a' (the tolerated ENXIO), frees
both sides, and the caller then links the torn-down 'a' anyway.
Consequences of the stale link: the addif writes (a list insert
through the linkage embedded in the ifnet, plus `if_addgroup()`)
usually land in dying-but-still-allocated memory only because the
final free is epoch-deferred (`NET_EPOCH_CALL` in `if_free()`);
nothing takes a reference, so the ifnet is freed shortly after and
two control-plane structures dangle permanently - the cloner list
(walked *through* the freed ifnet's embedded linkage) and the "pair"
group member list (re-added after `if_detach()`'s group purge, so no
cleanup path ever removes it).  Any later walk - `ifconfig -g pair`
(SIOCGIFGMEMB), pf group processing, the unload-time cloner detach
loop - traverses freed memory; the detach loop's NULL-softc
tolerance converges the entry only while the freed memory sits
unreused, and UMA trashing on INVARIANTS kernels makes the first
touch a deterministic panic.  The data plane never sees the stale
entry (`if_free()` clears the ifindex slot early).  epair(4) shares
the window exactly - it is the only other multi-ifnet cloner in base
and self-links its 'b' via `if_clone_addif()` too - with a harsher
outcome: its convergent destroy panic()s when the nested
`if_clone_destroyif()` of the unlinked side returns ENXIO ("... for
our 2nd iface failed"), so there the race is a root-triggerable
panic with a two-line reproducer (loop `ifconfig epair create`
against `ifconfig epairNb destroy`).  Every other in-tree cloner is
single-ifnet, where the window degenerates to a spurious ENXIO on a
visible interface - no corruption path.  Mitigated in-driver
(2026-08-17) by the destroy-side wait-retry: when the nested
`if_clone_destroyif()` of the partner returns ENXIO with
`pair_unloading` clear, that proves the create window (ioctl and
netlink destroyers are serialized by their callers'
`ifnet_detach_sxlock`, vnet teardown holds that same lock across its
detach loop (`vnet_destroy()`), and the unload-time destroyers - the
sweep and the cloner detach loops - cannot start before the flag is
set), so the destroyer sleeps and retries
until the creator's deferred addif lands - an unlinked side is
never freed outside unload interleavings.  The invariant: never
free an interface whose unlink failed.  Residual exposure after the
mitigation: unload-vs-create (the `pair_unloading` branch keeps the
old tolerate-and-free, since a concurrent destroyer parked on
pair_sx may have unlinked the partner for good and waiting would
deadlock) and the netlink creator tail (`modify_nl` and the reply
cookie run after the addif, with nothing observable to wait on).
The complete fix stays in the framework and is described after the
audit summary. (3) Non-issue on
reflection: packets queued across an if_vmove of a side may deliver
with pre-move FIB or vnet context, but this is observably equivalent
to the move having happened slightly earlier or later - and strictly
hybrid cases (old FIB, new vnet) can only affect packets that the
move's own address purge has already orphaned.  The standard workflow
(assign the interface to its long-term vnet before configuring
addresses or bringing it up, as example-jail.conf does) never has
traffic in flight during a move at all.  Same pattern as epair.
All other protocols verified: the
pq_mtx state machine (lost-wakeup-free), write-once-before-publish
fields, the quiesce/claim protocol, one-way lock orders (pq_mtx ->
taskqueue lock; pair_sx -> {epoch wait, pq_mtx, taskqueue}; callers'
ifnet_detach_sxlock -> pair_sx, never reversed), single-consumer
queue draining, and per-CPU counters.

Debug-kernel validation (2026-08-17, arm64 VM, WITNESS+INVARIANTS,
full test suite): no WITNESS output names any if_pair lock - the
pq_mtx state machine, the recursive pair_sx convergent destroy, and
the wait-retry's pause() while holding pair_sx plus the caller's
ifnet_detach_sxlock all ran silent.  (The other console lines were
benign test artifacts: the nd6 below-1280 MTU warning from the
minimum-MTU test, and ICMP response rate limiting from the flood
test.)  One lock order reversal surfaced, base-framework-shaped
with no if_pair frame in either chain - finding (4): jail removal
(vnet_destroy()) takes ifnet_detach_sxlock exclusively and then
vnet_sysinit_sxlock (running the dying vnet's sysuninits under it),
while module unload with live cloned interfaces takes the two in
the reverse order: vnet_deregister_sysuninit() holds
vnet_sysinit_sxlock while running the per-vnet cloner detach loops,
whose nested if_detach() calls acquire ifnet_detach_sxlock.  A
kldunload with live pairs racing a jail -r can therefore deadlock.
epair(4) has the identical AB/BA (kldunload if_epair with live
epairs).  FIXED in if_pair (2026-08-17): a driver-global pair
registry (one entry per pair, 'a' side, under pair_sx) lets
MOD_UNLOAD - which runs before the SYSUNINITs and holds neither
vnet lock - sweep all remaining pairs itself, taking
ifnet_detach_sxlock before pair_sx exactly as every ioctl destroyer
does; the per-vnet detach loops then find empty cloner lists and
the bad ordering is never taken.  Residue: a pair sitting in the
create-return window at sweep time is skipped (nested ENXIO) and
still reaped by the detach loop, so the bad order remains reachable
only inside the already-documented unload-vs-create residue.  Jail
death itself was never a problem: vnet_destroy() runs the dying
vnet's own detach loop while already holding ifnet_detach_sxlock
(recursive acquisition, consistent order).

Upstream fix for the create-return window (audit finding 2): every
destroy entry point already takes `ifnet_detach_sxlock` exclusively
(`SIOCIFDESTROY` in net/if.c; RTM_DELLINK in netlink/route/iface.c),
but no create entry point does - that asymmetry is the bug.  Minimal
patch: take `ifnet_detach_sxlock` at the create entry points
(`SIOCIFCREATE`/`SIOCIFCREATE2` in net/if.c; `create_link()` in
netlink/route/iface.c), mirroring exactly where the destroy side
takes it.  Entry-point placement matters: it spans the whole creator
tail - create_f, `if_clone_addif()`, `modify_nl`
(`_nl_modify_ifp_generic()` dereferences the new ifp heavily,
including an indirect call through its if_ioctl), and netlink's
`nl_store_ifp_cookie()`, which lies outside `if_clone_createif_nl()`
and would escape a lock taken there.  This makes create-plus-link
atomic against every destroy, for all cloners at once, with no KPI
change; the lock order matches the one destroys
already impose (callers' `ifnet_detach_sxlock` -> driver sx), and the
cost falls only on human-paced create.  Alternative shape: an
`IFC_F_SELFLINK`-style cloner flag declaring that create_f links the
returned ifp itself, letting a multi-ifnet cloner publish both sides
under its own serialization.  Either form is submission-worthy on its
own: it fixes the epair(4) panic above without reference to if_pair.

Upstream fix for the vnet teardown lock order (audit finding 4):
vnet_deregister_sysuninit() should acquire ifnet_detach_sxlock
before vnet_sysinit_sxlock, matching vnet_destroy()'s order, so
module unloads that destroy live cloned interfaces from their
VNET_SYSUNINITs stop deadlocking against jail removal.  This fixes
`kldunload if_epair` racing `jail -r` today; if_pair no longer
depends on it after the MOD_UNLOAD sweep, but remains a beneficiary
(the unload-vs-create residue path can still reach the old
ordering).

Upstream fix for routed TSO (transit-offload prerequisite, found
2026-08-18): ip_tryforward() and ip6_tryforward() lack ip_output()'s
TSO exemption - they compare packet length against the egress MTU
without testing csum_flags & ifp->if_hwassist & CSUM_TSO
(ip_fastfwd.c:494; ip6_fastfwd.c:210/230) and consume the packet
with an ICMP error, no slow-path fallback, so a forwarded TSO chain
dies on the default forwarding path even when the egress NIC could
segment it.  Mirroring the ip_output.c:779 test (pf_route() at
pf.c:9298 already has it) enables routed TSO for every
TSO-advertising interface.  Independent of if_pair - though nothing
in-tree generates routed TSO frames today, which is presumably why
the gap has gone unnoticed.

Known open items:
- Remaining runtime verification: transit checksum test via tcpdump
  on a real egress (needs a second interface), sendfile/M_EXTPG
  passage, and an iperf3 comparison against epair on real hardware.
  Everything else from the old list (smoke, destroy/unload with live
  and jailed sides, churn, race stress) is covered by tests/ and
  passed 2026-08-17 on both GENERIC and GENERIC-DEBUG; see the
  runtime test log.
- MTU is not synchronized between the two sides (documented: set both).
