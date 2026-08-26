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
`net.isr.bindthreads=0`.  Both are loader tunables (RDTUN;
maxthreads=-1 means all CPUs), so an operator CAN turn netisr into
a pinned per-CPU pool at boot - the reasons the driver brings its
own pool anyway: netisr is system-shared infrastructure (every
netisr_queue() consumer competes in the same ~256-deep per-proto
queues), it is boot-frozen where our knobs are runtime sysctls,
requiring global retuning for one driver fails GENERIC-first, and
swi_net has no batch/yield discipline - a 128-CPU pinned PI_NET
netisr would reproduce the big-iron callout starvation with no
place to put the fix, while our pool is exactly where the
batch/tick/yield machinery and the LRO seam live.)

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
  tick boundary having passed since the thread's last VOLUNTARY
  context switch.  (Reworked 2026-08-21: the original per-pass
  getsbinuptime() snapshot could not see across passes -
  back-to-back passes each under both budgets, with tick crossings
  landing on final packets, could chain into unbounded PI_NET
  occupancy, a measure-zero-but-real tail.  The tick budget is now
  anchored to td_swvoltick, which mi_switch() stamps on every
  voluntary switch - our yields, the wait-retry's pause(), the
  taskqueue idle sleep - so it spans passes and resets exactly when
  the monopoly it measures is broken; an end-of-pass check yields
  before a rescheduled pass when a tick has passed and work is
  queued.  This is should_yield(9)'s mechanism recalibrated from
  two timeslices to one tick, and reading `ticks` is cheaper than
  the timehands snapshot it replaced.)  The yield drops the worker
  below the callout threads and userland for one scheduling
  decision.  Sizing: the tick check hard-bounds callout lateness to
  one tick plus one packet regardless of per-packet cost or pass
  pattern (a pure count budget stretches with MTU - 64 x ~20 us at
  mtu 65535 overruns the 1 ms tick); the count budget provides
  sub-tick fairness to userland and remains the effective bound at
  hz=100 VM guests (10 ms ticks).  An
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
  CORRECTED 2026-08-21: the build validated above contained an
  accident.  kern_yield(PRI_USER) demotes via sched_prio(9), which
  rewrites td_base_pri - the anchor every priority-restoration
  mechanism (turnstile unlending, sched_userret()) unwinds to - and
  a pure taskqueue kthread has no restoration net (no userret; the
  taskqueue idle sleep passes priority 0).  The FIRST yield
  therefore demoted each worker to timeshare for the thread's
  lifetime, and the validated responsiveness partly rode on that
  permanent demotion rather than on the designed bounded-donation
  contract; equally, throughput under concurrent user CPU load
  would have degraded to timeshare fair-share, and post-first-yield
  delivery latency depended on unrelated user load.  Fixed by
  re-asserting PI_NET after every yield (thread_lock + sched_prio,
  the sched_userret_slowpath() idiom), making the
  one-scheduling-decision semantics real: interrupt-class service
  between yields, donation bounded per batch/tick.  REVALIDATED
  2026-08-21: t_16 (hw.ncpu/3 connections, host heartbeat canary)
  passed on the 128-core server with the re-assert in place, and
  t_18 throughput is statistically identical to the demoted build
  (the re-assert costs nothing measurable at ~385k yields per
  sweep).  The designed contract - interrupt-class service between
  yields, starvation bounded by the batch/tick budgets alone -
  now stands validated without the accident's help.
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

**Implementation requirements (2026-08-24, verified against this
host's /usr/src).**  Everything above re-verified; one gap is new.

Driver side (if_pair.c) - small, self-contained:

- Advertise IFCAP_TSO4|IFCAP_TSO6 in if_capabilities but NOT in the
  default capenable (default-off per the stall analysis above).
- SIOCSIFCAP: map IFCAP_TSO4 -> CSUM_IP_TSO and IFCAP_TSO6 ->
  CSUM_IP6_TSO into hwassist.  tcp_maxmtu()/tcp_maxmtu6() require
  BOTH the capenable bit and hwassist & CSUM_TSO (tcp_subr.c:3657/
  3699).  Follow driver convention: TSO only alongside the matching
  TXCSUM (tcp_output marks TSO frames CSUM_TCP, assuming checksum
  offload rides along).  Each side's capenable is independent and
  that is the right granularity: a jail's connections consult the
  first-hop ifp, i.e. the jail's own side - enabling TSO on pairNb
  grants TSO to that jail, not to the host.
- TSO limits: leave if_hw_tsomax{,segcount,segsize} at 0.
  if_attach() fills the tree-wide conservative defaults - 65518
  bytes, 35 segments, 2048 per segment (if.c:917-920) - which any
  TSO-capable NIC is expected to meet, so no vxlan-style
  arithmetic and no new tunables.  Cosmetic: if_attach() then
  prints "Using defaults for TSO" once per interface when
  IFCAP_TSO is in if_capabilities; set the same values explicitly
  before if_attach() to silence it.
- pair_output()/pair_input(): NO changes needed - deliver-whole
  already works.  tcp_output() sets CSUM_TCP alongside CSUM_TSO
  (tcp_output.c:1384/1404), so a TSO frame terminating in the peer's
  stack is accepted by tcp_input()'s existing local-host request-bit
  branch (tcp_input.c:718/652) - the keep-request-bits contract
  covers TSO with zero new code.  The M_EXTPG guard in pair_input()
  is compatible: sendfile-built TSO chains keep the headers in a
  mapped lead mbuf; pair_hash_mbuf() already reads via m_copydata().
  pair_csum_vouch() stays valid: CSUM_IP is not in our hwassist, so
  ip_output() computes ip_sum in software even on TSO frames.
- Considerations to document (not blockers): mbufq is count-limited,
  so worst-case queue memory rises from 64 MB (16K) to 256 MB
  (4096 x 64K) per queue - in practice bounded by aggregate TCP
  windows, but worth a byte-cap thought if it ever bites; OPACKETS
  counts one per unsplit frame (ip_output's ia counters divide by
  tso_segsz - we could do the same when CSUM_TSO is set).

Kernel side - the payoff case (routed transit) needs three small
upstream patches, all the same one-line shape (add the TSO-hwassist
exemption ip_output.c:779 already implements):

- ip_tryforward(): bare ip_len <= nh->nh_mtu at ip_fastfwd.c:494.
- ip6_tryforward(): two bare checks, ip6_fastfwd.c:210 and :230.
- NEW (2026-08-24): ip6_forward() - the v6 SLOW path - also balks:
  bare IN6_LINKMTU check at ip6_forward.c:388.  IPv4's slow path is
  exempt only by delegation (ip_forward() -> ip_output()); v6 does
  its own check, so for IPv6 both paths are broken today.

Already in place upstream (verified): ip_output.c:779,
ip6_output.c:1131, pf_route pf.c:9298/9318.  No generic software
GSO exists in-tree, so there is no fallback that avoids the kernel
patches for routed traffic; unpatched, a routed TSO frame dies as
ICMP too-big and TF_TSO survives the re-probe -> connection stalls,
which is exactly why default-off is a hard requirement, not a
preference.  Host->jail and jail->host TSO (pair-local termination)
works without any kernel patch, but that is the measured +3.8% case.

Test matrix when built: pair-local TSO both directions; transit on a
patched kernel through a TSO NIC (expect order-of-magnitude, the
~44x batching argument); transit to a non-TSO egress (expect and
document the stall); pf enabled (pf_test on 64K frames, route-to via
the pf_route exemption); tcpdump on giant frames; INVARIANTS pass.

**Router-grade LRO+TSO composition (investigated 2026-08-24).**
Question: could a FreeBSD router run LRO and TSO on all interfaces,
Linux GRO/GSO style (merge at ingress, re-segment at egress)?  Not
today: tcp_lro records only lro_nsegs (accounting) - never
tso_segsz, never CSUM_TSO (verified tree-wide) - and no output path
re-segments anything, so a merged frame at a smaller-MTU egress
blackholes (ICMP needfrag quoting an MTU the origin already
honors).  Enabling it = two separable upstream projects:

- Phase A, software GSO: a tcp_gso() that splits a CSUM_TSO frame
  in software (regenerate headers, step seq/ip_id, FIN|PSH only on
  the tail, per-segment checksums), hooked at every current balk:
  ip_output.c:819-825, ip6_output.c:1161 region, the three
  forwarding sites above, pf_route pf.c:9318.  Contained,
  independently valuable, and precedented: Garzarella's 2014 "GSO
  for FreeBSD" patch (freebsd-net), never merged.  DIRECT if_pair
  relevance: GSO at the balk points removes the non-TSO-egress
  stall - the sole reason pair TSO must ship default-off.  With
  Phase A upstream, pair TSO becomes safe unconditionally.
- Phase B, reversible ("GRO-mode") LRO: invasive tcp_lro surgery.
  Current merge is endpoint-oriented and lossy - tcp_lro.c:734
  overwrites the header tsval with the newest merged segment's
  ("Incorporate latest timestamp") - so a forwarding-safe mode
  must merge only what re-segmentation reconstructs exactly:
  equal-size full segments (record as tso_segsz), byte-identical
  TCP options (flush on tsval change instead of mutating),
  identical TTL/TOS/ECN (no CE smearing), DF-only with the RFC
  6864 stance on regenerated IP IDs, th_sum rewritten to
  pseudo-header form at hand-off.  The local/forwarded split is
  decided at the forwarding lookup, not at merge time: LRO stamps
  reversibility + tso_segsz; ip_tryforward converts to CSUM_TSO
  when the frame transits.  Hardware LRO engines cannot promise
  any of this, so GRO-mode forces the software engine.
- Phase C, policy: default-off knobs, pf/ipfw see 1 merged packet
  instead of N (state/counter semantics), bpf sees giants,
  if_bridge keeps stripping (L2 out of scope).

Not an if_pair work item - recorded because Phase A alone changes
the pair TSO calculus above, and any freebsd-net proposal for the
three forwarding exemptions can cite Phase A as the principled
fix the exemptions approximate.

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

Properties: per-flow ordering absolute for every flow that carries
or learns a flowid - see the 2026-08-25 correction below for the
residual class (sender migration degrades locality, never order,
for the covered flows); the flowid-reflection loop keeps BOTH
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

**Correction (2026-08-25)**: step 2's framing - "the only moment
steering is decided, on a flow's first packet" - and the ordering
property derived from it overstate the case. `pair_hash_mbuf()`
runs on EVERY flowid-less packet, so any curcpu-derived input
(here, the sender's domain) is re-read per packet, and a sender
migrating mid-flow would remap such a flow between workers -
ordering is absolute only for flows whose packets carry a flowid
or whose endpoints learn the written-back one (the TCP reflection
loop; a packet or two of pre-learning exposure is acceptable).
The residual flowid-less class is however far smaller than a
first draft of this correction assumed ("unidirectional UDP"):
verified on 15.0, every UDP socket is born with a synthetic
per-socket flowid (`udp_attach()`, udp_usrreq.c:1573-1575, atomic
counter, `M_HASHTYPE_OPAQUE`), and `connect()`ed sockets on
ROUTE_MPATH kernels (GENERIC) get one in `in_pcbconnect()`
(in_pcb.c:1171-1180; Toeplitz over the 5-tuple) - though only once
`net.route.hash_outbound` is on, which defaults to 0 and
auto-enables when the first multipath route is installed
(route_ctl.c:903-910).  So UDP always arrives pre-stamped, TCP
only on hosts that actually use multipath routes; elsewhere TCP
still relies on our writeback plus its learn sites and never
consults `pair_hash_mbuf()` again after learning.  TCP's learn site
(tcp_input.c:927-931, guarded by `inp_flowtype == M_HASHTYPE_NONE`;
server side inherits the SYN's flowid via tcp_syncache.c:813)
covers the rest of TCP.  What actually remains flowid-less:
raw-socket/ICMP traffic, kernel-originated packets without an
inpcb, and forwarded packets whose ingress NIC stamps no RSS hash
- and of those only the userland raw-socket sender migrates on
scheduler whim (ULE re-decides placement at every wakeup;
forwarding ithreads are effectively CPU-stable).  Design rule if
this is ever built: take the domain decision only on the
flowid-writeback path, and keep the no-writeback fallback
domain-independent (plain global modulo), so the residual class
retains today's absolute ordering.  The same 2026-08-25 discussion
also settled why curcpu-derived steering cannot dodge
sender/worker CPU collisions at all: ULE's wakeup affinity
(placement decided at every wakeup, cache-warmth window
`affinity` = hz/1000 ticks per topology level, sched_ule.c:317)
re-plants a blocking sender next to the worker that wakes it, so
hash-time avoidance decays within a few RTTs; only cpuset(1), the
~1 Hz balancer (sched_ule.c:1000), or per-side salting of the
queue index (deterministic, ordering-safe, but pointless while the
single-flow ceiling is the receiver's copyout at 100% vs the
shared worker at 54%) actually move that needle.

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

**MTU 65535 A/B on the Ampere (2026-08-25): batching lever
confirmed.**  Predicted above ("divides per-packet lock traffic by
the aggregation factor") and measured: with mtu 65535 on both
sides (jail-to-jail, single iperf3 client process), P=128 went
140 -> 283 Gbit/s (2.0x) and the peak moved from 223 at P=16 to
346 at P=64 (1.55x); falloff off peak softened from -37% to -18%.
The contrast with the VM's +3.8% for the same knob (2026-08-15) is
the whole story: on the VM locks were 13.5% of cycles and 4x fewer
packets bought almost nothing; on the Ampere locks were 86.9% of
non-idle cycles and the same 4x cut of lock-tour frequency doubled
high-connection throughput.  Batch size is a per-machine lever,
not a constant - the 16384 default (chosen on the VM) stands for
small systems, but many-core deployments should raise the MTU;
worth stating in the man page's MTU CONFIGURATION section.
Caveats and follow-ups: single-client-process iperf3 may itself
cap the sweep (the 2026-08-15 benchmarking note), so the true peak
may be higher with multiple client processes; a t_19-style profile
at mtu 65535 would show whether the residual -18% falloff is still
lock_delay or has shifted back to copy; an overruns capture would
give the per-busy-worker efficiency comparison.  This also
partially re-scopes the parked TSO/LRO work: for pair-local
traffic mtu 65535 captures the batching win directly, so LRO's
remaining case is transit traffic clamped to wire-size segments
by a remote MSS.

**debug.lock.delay_max sweep (2026-08-25): backoff is
load-bearing, default validated, knob closed.**  Hypothesis
tested: with ~100 waiters backing off up to 32767 spinwait
iterations, released locks might sit idle while spinners
oversleep, so a lower ceiling might re-acquire faster.  Measured
at P=64, mtu 65535, no ipsec (Gbit/s): 32767 -> 380, 8192 -> 159,
2048 -> 65.2, 512 -> 60.4.  Refuted, monotonically and 6x over:
aggressive re-probing collapses throughput because spinner probe
storms on the lock cache line slow the OWNER's critical section -
on a 128-core mesh the exponential backoff is what protects owner
progress, and the ncpus-scaled default (min(roundup_2(ncpus)*256,
SHRT_MAX), already saturated at 128 cores) is correct.  Do not
lower; nothing to raise (u16 ceiling).  Two byproducts: baseline
380 vs the sweep's earlier 346 peak bounds run-to-run spread at
~10% for these 10 s points; and the extreme sensitivity of
throughput to spin-probe rate is itself evidence that lock
contention - not copy - still owns the residual ceiling at 64K
MTU, without needing the deferred t_19 re-profile.  The locality
levers (capped worker set, cpuset confinement) therefore stay
live as the next experiments.

**t_20 lockstat attribution (2026-08-25, samples/t20_noargs.txt =
mtu 65535, t20_mtu16k.txt = mtu 16384; P=128, 10 s windows): the
top lock is GLOBAL, not the triangle.**  The instance analysis
found the #1 spin sink is a SINGLE lock instance: lo_name
"callout", 4.85M spins and 575 s of aggregate spin time in 10 s
(~57 CPU-equivalents) at 64K MTU; 5.04M/604 s at 16K.  Mechanism,
verified in source: with net.inet.tcp.per_cpu_timers=0 - the
DEFAULT on non-RSS kernels (tcp_timer.c:195) - inp_to_cpuid()
returns 0 (tcp_timer.c:250-252), so EVERY TCP timer in the system
lives on CPU 0's callout wheel, and every rearm (essentially every
ACK processed and every transmit, see the captured stacks:
callout_reset_sbt_on from tcp_do_segment/tcp_default_output) takes
CPU 0's cc_lock - a spin mutex hammered from all 128 CPUs.  The
triangle locks are real but second tier: tcpinp rw-spin 490 s
spread over 256 instances (top 0.8%), so_snd/so_rcv ~40 s over
128+129 - textbook per-connection ping-pong, no single fixable
instance.  Two more findings: lock wait totals are nearly
IDENTICAL across MTUs (575 vs 604 s callout, 490 vs 530 s tcpinp)
while throughput-under-capture doubles (200 vs 97.8 Gbit/s) - the
contention wall is a fixed per-machine cost and larger packets
simply move 4x the bytes per lock tour, which is exactly the
batching model.  And observation overhead is ~30% (200 under
capture vs 283 uninstrumented at 64K): treat t_20 throughput as
directional only.
Next experiment, one sysctl, reversible: net.inet.tcp.per_cpu_timers=1.
On non-RSS kernels that maps each connection's timer to
inp_flowid % (mp_maxid + 1) (tcp_timer.c:246) - and since our
worker steering is flowid % pt_count with pt_count == mp_ncpus,
dense CPU numbering makes the timer CPU EQUAL the connection's
worker CPU: rearms become CPU-local instead of cross-mesh, on top
of the wheel spreading 128 ways.  Predicted effect: the current
#1 sink (~57 busy CPU-equivalents of pure spinning) largely
vanishes; tcpinp becomes the leader.  Caveat for the record: the
sysctl default (0) has history - per-CPU timers interact with CPU
offlining (absent-CPU fallback is curcpu, tcp_timer.c:247-249) -
but on a fixed-topology server the exposure is nil.

**per_cpu_timers=1 A/B (2026-08-25, samples/t20_percpu.txt +
t20_percpu_mtu16k.txt): the callout wheel WAS the wall.**
net.inet.tcp.per_cpu_timers=1 (host-global, not CTLFLAG_VNET -
one setting covers all vnets), same P=128 t_20 runs:

- Throughput UNDER CAPTURE: 64K mtu 200 -> 335 Gbit/s; 16K mtu
  97.8 -> 332.  Under-observation now exceeds the old
  uninstrumented 283, so the true rates are unknown and higher -
  t_17 rerun needed for the real curve.
- The callout lock is gone from the 64K tables entirely; at 16K
  it re-enters at rank 9 with 6.5k spins over 87 instances (top
  30%) - spread across per-CPU wheels exactly as predicted by
  inp_flowid % (mp_maxid+1) == the connection's worker CPU.
- Aggregate lock wait collapsed ~5x (was ~1100 s per 10 s window
  across the top sinks, now ~230 s).
- REVISION of the batching narrative: with the global wheel fixed,
  the MTU advantage nearly vanishes (335 vs 332).  The earlier
  "batching lever confirmed" result was real but its mechanism is
  now clear: 4x fewer packets bought 4x fewer tours of ONE
  serialized lock.  With that lock spread, per-connection lock
  tours no longer bind at these rates, and mtu 16384 is
  competitive again on big iron.
- New leader: the mbuf UMA ZONE lock - single instance, 100%
  share, 110 s spin + 29 s block at 64K (413k spins) - i.e. the
  next wall is allocator refill pressure past the per-CPU UMA
  caches, with turnstile_chain (~20 s, one dominant chain) as its
  derivative: many threads now BLOCK on the same zone lock and
  hash to one turnstile chain.  Driver locks stay negligible
  (pairq ~2 s block, 150 instances, top 3.7%).
- Upstream-worthy alongside the ipsec finding: the non-RSS
  default (per_cpu_timers=0) funnels every TCP timer in the
  system through CPU 0's cc_lock; on 128 cores that one spin
  mutex burned ~57 CPU-equivalents and capped local TCP at less
  than half of what the machine does with the timers spread.
  Caveat for any report: the default likely protects CPU-offline
  edge cases (tcp_timer.c:247-249) and low-core-count machines
  won't see the cliff.

All four t20_*.txt samples were RE-CAPTURED 2026-08-25 with the
final t_20 (provenance header; idle-baseline delta subtraction),
toggling the knob per condition so every file self-describes.
The 2x2 matrix reproduced within the ~10% run spread: timers=0
64K/16K = 203/95.1 (was 200/97.8), timers=1 64K/16K = 334/330
(was 335/332) Gbit/s under capture, and the mbuf-zone finding is
stable (138 s spin + 39 s block above baseline, turnstile_chain
29 s derivative).  The numbers quoted above stand.

**16K vs 64K contention with per_cpu_timers=1 (2026-08-25,
t20_percpu_mtu16k.txt vs t20_percpu.txt): MTU is now
contention-neutral.**  At equal throughput (330 vs 334 Gbit/s
under capture), dropping the MTU 4x moves total spin from ~224 to
~170 CPU-s per 10 s window (~17% -> ~13% of the machine) - i.e.
DOWN, and by far less than the packet-count change.  The
composition confirms the model:

- Per-connection locks scale with PACKET rate but stay cheap:
  so_rcv 1.9M -> 4.8M spins (time only 29 -> 34 s), tcpinp 308k ->
  524k (24 -> 30 s), pairq 21k -> 35k (still noise).  Quadrupling
  the packet count costs ~10-15 s of machine-wide spin - the
  spread per-connection locks absorb the extra tours.
- The mbuf zone lock is BYTE-rate-driven and therefore MTU-immune:
  413k -> 401k spins, flat, because sosend chops copyin into
  PAGE_SIZE clusters regardless of packet size - clusters per byte
  are identical across MTUs at equal throughput.  No packet-level
  lever (MTU, batch, future TSO/LRO) touches it; only
  allocator-side changes (firmware NUMA domain split, a BUCKET_MAX
  bump - uma_core.c:252, buckets already autoscale to the cap on
  contention per uma_core.c:702) or moving fewer bytes.  Its spin
  TIME does differ (138 vs 90 s at equal count): longer zone-lock
  holds when 64K chains free in bigger bursts; second-order.
- turnstile_chain tracks mbuf blocking down (29 -> 14 s),
  confirming it is purely the mbuf zone lock's blocking shadow.

Recommended operating point on the Ampere going forward:
per_cpu_timers=1 (loader.conf/sysctl.conf), MTU per taste - the
16K default is no longer a big-iron penalty.  The sysctl
recommendation is now also in the man page (if_pair.4 TUNING
section, added 2026-08-25).  Next levers if the
new ceiling matters: t_17 uninstrumented rerun first, then the
mbuf zone (UMA per-CPU bucket sizing / allocation batching) and
the lo(4) baseline in open item (a).

**Uninstrumented t_17 A/B after reboot (2026-08-26,
samples/t17_ampere_retest.txt; security patches installed, default
16K MTU): peak 479 Gbit/s.**  timers=0 first: the curve reproduces
the 2026-08-21 no-ipsec baseline almost exactly (223 @ P=16 peak,
144 @ P=128 vs 223/140) - the baseline is stable across a reboot
and a patch level, which also retroactively validates comparing
the pre/post-reboot captures.  timers=1: 354 @ 16, **479 @ P=32
peak** (2.15x), 362 @ 64, 337 @ 128 (2.34x); the peak moved from
16 to 32 connections, the same only-more-connections-fit-now shift
the MTU change produced, and high-P overruns FELL (374k -> 191k
counts while moving 2.3x the data) - per-worker efficiency up,
not just aggregate.  Cross-checks: t_20's 334 under capture at
P=128/64K vs 337 uninstrumented at P=128/16K confirms both MTU
neutrality and that lockstat capture overhead collapsed along with
the contention it observes (probe cost is per contention event;
the events are gone).  One honest wrinkle: low connection counts
dip slightly under timers=1 (2 conns 68 -> 56, 4 conns 120 -> 110)
- plausibly the flowid-placed timer landing on the connection's
busy worker CPU; at or beyond the old ~10% run spread, negligible
against the 2x scaling win, not worth chasing.  The man page's
"more than doubled" claim is now backed uninstrumented at the
default MTU (223 -> 479).  Residual falloff past P=32 (-30% off
peak) remains the mbuf-zone + triangle territory mapped by t_20.
Slides note: the 330 figure is again stale (pending, slides parked
2026-08-26).

**t_20 retest on 15.1-p3 (2026-08-26, samples/t20_retest.txt):
stable, and the mbuf-zone traffic is now stack-attributed.**
Same shape as the 2026-08-25 percpu captures (329 Gbit/s under
capture, mbuf 118 s spin + 38 s block, so_rcv 25 s, tcpinp 21+17 s,
turnstile 24 s) - the contention picture survives the reboot and
patch level.  New: the loaded-window stacks split the mbuf zone
lock traffic by site, confirming the alloc/free CPU-split model
exactly: ALLOC in transmit contexts - cache_alloc_retry via
tcp_m_copym (retransmit-chain header mbufs, from both
tcp_usr_send and ACK-clocked tcp_do_segment output) and via
m_getjcl <- mc_uiotomc (sosend copyin) - and FREE in receive
contexts - cache_free via m_free from soreceive_generic_locked
(post-copyout) and mb_free_ext (shared-cluster refs).  Notable:
tcp_m_copym means every transmit allocates header mbufs even
though clusters are reference-shared, so the zone churn has a
per-segment component on top of the per-byte cluster component.

**t_19 at P=32 vs P=128 with timers=1 (2026-08-26,
samples/t19_p32.txt / t19_p128.txt): peak is half-idle, falloff is
copy-efficiency collapse.**  Two headline facts and one derived
mechanism:

- P=32 - the 479 Gbit/s PEAK - runs the machine at 49.6% IDLE.
  The ceiling at peak is per-connection pipeline depth (32 flows
  x their sender/worker/receiver legs occupy ~64 CPUs), not any
  global resource.  Non-idle split: copy 46.3% (copycommon
  45.3%), locks 22.1%, alloc 12.3%, proto 8.8%.
- P=128: 0.0% idle, yet LESS throughput (337).  Split: copy
  56.6%, locks 15.0%, alloc 13.2%.
- The derived mechanism: absolute copy productivity collapses.
  P=32: ~30 CPUs' worth of copycommon moves 479 Gbit/s -> ~16
  Gbit/s per copying CPU.  P=128: ~72 CPUs' worth moves 337 ->
  ~4.7.  The same memcpy running 3.4x slower per CPU is a
  memory-system effect: 128 connections' socket buffers blow the
  system-level cache, copies come from DRAM, and all 128 CPUs
  contend for bandwidth.  Locks are bit players everywhere now
  (~14 vs ~19 absolute CPU-equivalents at the two points,
  independently matching t_20).  The alloc bucket (~13%:
  mb_dupcl, mb_free_ext, mb_ctor_clust, m_demote) is the
  mbuf-lifecycle cost stack-attributed in the retest above.
- Falsifiable next test (cheap, causal): rerun P=128 with small
  fixed windows (iperf3 -w 256k) - recovery toward the peak
  confirms the working-set explanation; a STREAM run would supply
  the bandwidth denominator for the copy arithmetic.
- Caveat: total profile samples differ 2x between captures (482k
  at P=32 vs 244k at P=128 against ~636k theoretical), so some
  context at high P evades sampling (suspect interrupt-masked
  sections); within-capture percentages and the t_20-corroborated
  lock numbers are solid, fine cross-capture deltas are not.

**t_21 working-set sweep (2026-08-26, samples/t21_ampere.txt):
hypothesis CONFIRMED - the falloff is buffer working set vs
cache, and right-sized buffers ELIMINATE it.**  P=128 with
descending fixed windows, P=32 default-window reference first:

    conns  window   Gbit/s  netmemKiB
       32  default     486      43258   <- peak reference
      128  default     336    1198082   <- autoscaled: 1.2 GB(!)
      128  4m          339     549766
      128  1m          360     193939
      128  256k        397      77978
      128  64k         493      49583   <- EXCEEDS the peak

  Monotonic recovery tracking the measured footprint, zero drops
  anywhere; at 64k windows P=128 beats the machine's best
  (493 vs 486) - the "falloff" was never about connection count
  at all, only about the ~1.2 GB of autoscaled socket buffers
  (128 conns x up to 8+8 MB sendbuf_max/recvbuf_max) thrashing
  the cache hierarchy, exactly as the t_19 copy-efficiency
  arithmetic predicted.  With buffers fitting in ~50 MB the
  machine holds ~490 Gbit/s FLAT from P=32 to P=128; that
  plateau is the true ceiling (presumably copy/memory bandwidth
  - a STREAM run remains the missing denominator).  The knee
  below 64k is unexplored (WINDOWS="64k 32k 16k" would find it;
  BDP at these us RTTs is only tens of KB, so headroom likely
  remains).
  Operator lever, and it is jail-scoped: net.inet.tcp.sendbuf_max
  and recvbuf_max are CTLFLAG_VNET (tcp_output.c:126-127,
  tcp_input.c:222, 8 MB defaults) - a vnet jail can cap ITS OWN
  autoscaling (e.g. 256k-1m for machine-local bulk) without
  affecting the host's WAN connections, whose long-RTT BDPs
  genuinely need large buffers.  Autoscaling overshoots
  machine-local flows by ~100x: it grows toward bandwidth x
  loss-recovery targets sized for real networks, while a
  microsecond-RTT pair flow needs tens of KB.  Candidate for the
  man page TUNING section alongside per_cpu_timers.

**t_21 knee sweep (2026-08-26, samples/t21_to16k.txt): the
optimum is 64k, and the floor is segment-count granularity.**
P=128, powers of two 512k -> 16k (P=32 default reference 491):
512k 373, 256k 398, 128k 450, **64k 492**, 32k 425, 16k 342.
Above 64k the cache-thrash side of the curve (footprint 117 ->
49 MB); below it throughput falls while footprint barely moves
(44 -> 42 MB), so the small side is NOT working-set - it is
pipeline starvation, and the numbers say why: at the default
16384 MTU the MSS is ~16344, so a 64k window is ~4 segments in
flight, 32k is 2, and 16k is stop-and-wait with a single
segment.  The three-stage sender -> worker -> receiver pipeline
needs a few segments in flight to stay busy; ~4 x MSS is the
floor, and the optimum is the smallest window that clears it.
Consequences: the optimal window scales WITH the MTU (a 64K-MTU
pair would want ~256k windows), so at very high connection
counts the default 16K MTU plus small per-flow buffers is the
cache-friendliest operating point.
NOT man-page material (decided 2026-08-26): the vnet sysctls cap
EVERY TCP connection in the jail, and any external connection
has a BDP orders of magnitude above 64k - a jail-wide cap would
cripple exactly the traffic autoscaling exists for.  Capping is
only sound per APPLICATION (setsockopt/SO_*BUF, iperf3 -w) or
in a jail whose traffic is verifiably machine-local.  Recording
the curve here as benchmark insight, not operator guidance;
generalizing it would be overfitting one microbenchmark.

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

#### RSS status research (2026-08-21)

Context: the scaling levers above raised "integrate with the
kernel's RSS framework when present" as the principled version of
locality steering.  Researched before committing to it; findings:

What RSS offers over our jenkins+modulo: rss_hash2cpuid() is the
mapping stage (our `hash % pt_count` analog), backed by Toeplitz
with a shared key - the hash NIC hardware implements, so hardware
and software agree on values system-wide - and an indirection table
(2^rss_bits buckets, ~2x per CPU, rss_config.c:213) that decouples
flow identity from CPU choice.  It only trusts its own hash types:
M_HASHTYPE_RSS_* resolve, everything else (including our
OPAQUE_HASH writeback) returns NETISR_CPUID_NONE - adoption means
computing software Toeplitz and marking honest RSS types, not just
calling the mapper.  All of it is `optional inet rss | inet6 rss`
(conf/files:4237): none exists on GENERIC, which is why if_pair
rolled Jenkins (GENERIC-first requirement).

The table's celebrated rebalance-without-rehashing property is
DORMANT in FreeBSD: rss_table is written in exactly one place (boot
init, round-robin, rss_config.c:248), every sysctl is RD/RDTUN, and
no in-tree rebalancer or setter exists.  What IS exercised is
coherence: RSS-capable drivers (ixgbe, e1000, igc, ena, sfxge,
iavf) program their hardware indirection from the kernel table, so
hardware and stack steering agree.  Dynamic rebalancing is real in
the wider ecosystem (Windows RSS, Linux ethtool -X) but FreeBSD
never grew the consumer.

Why RSS is not in GENERIC - no official statement, but the tree
testifies: the option is absent from the sys/conf/NOTES catalog
(bare mapping in conf/options:477 only); the implementation's own
TODO (rss_config.c:64) still lists fundamentals - key
synchronization, config-change event handlers (the missing
rebalance consumer), "Randomize key on boot", IPv6 support; the
shipped key is the PUBLIC Microsoft specification key with "XXXRW:
And that we don't randomize it yet!" (:149), so flow-to-CPU
placement is attacker-computable - the exact hash-flooding concern
our per-load random pair_hash_seed avoids; and the header still
describes PCBGROUP machinery removed wholesale in 2021.
Meanwhile GENERIC gets the valuable half anyway: multiqueue NICs
spread flows across queues with their own keys/tables without
`options RSS` - the option only adds the (unfinished) stack-wide
coordination layer.

Git history of the core files (rss_config.*, in_rss.*, in6_rss.*):
born as ~599 lines by Robert Watson in one commit (2014-03-15);
essentially all development by Adrian Chadd - 20 commits,
+1670/-724, May 2014 through November 2015 - then NOTHING but
housekeeping for a decade (spelling, sysctl-flag sweeps,
whitespace, a warning fix, boilerplate removal), the only
substantive touch being Gleb Smirnoff's 2021-12-02 decoupling of
RSS from PCBGROUP so PCBGROUP could be deleted: life support, not
development.  The 2014-era TODO has never been worked.

Provenance and the production-use question (in_rss.h carries
"Copyright 2010-2011 Juniper Networks... developed by Robert N. M.
Watson under contract to Juniper"): Watson's merge message for the
2014 birth commit states directly that "this prototype (and
derived patches) are in use at Juniper and several other
FreeBSD-using companies", and gives the merge's purpose as
refining it "in collaboration rather than maintained as a set of
gradually diverging patch sets" - production use was the stated
premise, and fits Junos's FreeBSD-based control plane (a routing
engine terminating thousands of BGP sessions is the pcbgroup
scaling problem; the forwarding plane is ASIC territory).  The
collaboration never happened, and the sponsorship record proves
the negative sharply: Juniper remains a heavyweight FreeBSD
sponsor (25-36 commits/year in the early 2010s, 124 in 2022, 133
in 2023, still active 2025 - veriexec/secure-boot, toolchain,
platform work), yet across ~600 sponsored commits over 15 years,
exactly ONE ever touched the RSS core files: the 2014 merge
itself.  Whether they still use it (frozen feature set, resumed
private divergence, or superseded by the Linux-based Junos
Evolved on newer platforms) the tree cannot say; what it can say
is that the "reservations about its maturity" the merge promised
to refine away are still in the TODO, verbatim, eleven years
later.

Implications for if_pair: (1) the Jenkins-plus-per-load-seed choice
is vindicated - it targets the kernels people run and is more
steering-attack-resistant than RSS's shipped default; (2) "use RSS
when available" stays on the future-work list but only as a thin
optional veneer (software Toeplitz + honest hash types when
opt_rss.h says so) for the custom-kernel minority - never a
dependency on a framework whose last feature commit predates
FreeBSD 11; (3) cautionary precedent, sharpened by the Juniper
history: infrastructure calcifies half-finished even when it HAS
a production user - a sponsor with abundant ongoing upstream
capacity never funded its completion - so our steering/cap knobs
arrive together with the measurements and consumers that justify
them, or not at all.

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
