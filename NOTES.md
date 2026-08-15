# if_pair — developer notes

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
NDP (IPv6). For the common case — routed IP traffic between two vnets —
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
ifconfig pair0a inet 192.0.2.1/31
ifconfig pair0b vnet myjail
jexec myjail ifconfig pair0b inet 192.0.2.0/31
```

Destroying `pair0a` destroys both halves. The `b` side cannot be
destroyed directly (matching `epair(4)` semantics); vnet teardown may
destroy either side via `IFC_F_FORCE`.

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

- `README.md` — user-facing introduction and quick start.
- `Makefile` — standard `bsd.kmod.mk` out-of-tree module build.
- `if_pair.c` — the driver.
- `if_pair.4` — man page (`man ./if_pair.4` to preview).
- `tests/smoke.sh` — root-only smoke test using two vnet jails
  (IPv4 + IPv6 ping across the pair).
- `LICENSE` — BSD-2-Clause.
- `ports/net/if_pair-kmod/` — FreeBSD port skeleton (see Roadmap).

## Roadmap

1. **FreeBSD port** (`net/if_pair-kmod`): the skeleton in `ports/` is
   ready except for distribution — it needs a public repository or
   release tarball (`USE_GITHUB`/`MASTER_SITES` + `make makesum`), a
   `WWW` line, and a `poudriere testport` run. Style: `portlint -AC`.
2. **Eventually, maybe, base system inclusion**: the driver is written
   base-style throughout (kernel normal form, in-tree APIs only, mdoc
   man page, `SPDX-License-Identifier: BSD-2-Clause`). Known items for
   that step: move the man page install into the base build glue,
   convert `tests/` to ATF (`tests/sys/net/` conventions, like
   `if_epair` tests), and resolve the two upstream interactions
   documented below (libalias NAT repair gate; `divert_packet()` and
   `CSUM_IP`) — ideally by landing those fixes independently, since
   they affect `epair(4)` today.

## Design notes / status

The driver uses the modern opaque-`ifnet` accessor API (`if_t`,
`if_get*`/`if_set*`) and the `ifc_attach_cloner()` cloner interface, both
required/current on FreeBSD 14+ — modeled on `epair(4)` and `gif(4)`.

### Performance / delivery design

Every transmitted packet is enqueued onto the receiving side's
`mbufq` (one per pool worker, guarded by a per-queue mutex — the
driver's only locks) and delivered by a pinned worker — **never
inline**, regardless of `net.isr.dispatch`. The workers hand packets
to `netisr_dispatch()`, which with the default direct policy runs the
input path right there in the worker; netisr's own queues and threads
are involved only if the admin selects deferred dispatch. (netisr
itself, despite its per-CPU workstream architecture, ships as a
single *unpinned* thread — `net.isr.maxthreads=1`,
`net.isr.bindthreads=0` — which is why the driver brings its own
pool rather than leaning on it.)

- **Why queueing is mandatory, not a choice** (learned the hard way —
  see the postmortem below): inline dispatch runs the peer's entire
  input path nested inside the sender's call chain. For TCP between
  two local sockets that chain loops back: the sender's
  `tcp_output()` holds its inpcb lock when the peer's inline ACK
  re-enters `tcp_input()` for the *same connection in the same
  thread*. On `INVARIANTS` kernels this dies as "recursed on
  non-recursive mutex"; on production kernels the ownership KASSERT
  is compiled out, the mutex silently recurses, and the nested ACK
  processing mutates the connection under the suspended outer
  `tcp_output()`, corrupting its send-buffer snapshot — a delayed
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
  (locally originated TCP/UDP carries one — `ip_output()` stamps
  `inp_flowid`), falling back to a per-side round-robin static
  assignment. Three deliberate improvements over epair: (1) epair has
  the per-CPU pinned pool **only on RSS kernels** — on GENERIC it runs
  a single unpinned worker (a fact discovered late; earlier versions
  of these notes wrongly credited GENERIC epair with per-CPU
  spreading); we pin unconditionally. (2) per-flow steering on
  GENERIC, where epair collapses to one queue. (3) pool lifecycle in
  `SYSINIT/SYSUNINIT(SI_SUB_TASKQ)` instead of `MOD_UNLOAD` —
  `kern_linker.c` fires module events *before* file SYSUNINITs, so
  epair frees its pool while its cloner (and any live pairs) still
  exist; our ordering keeps the pool alive until after cloner
  teardown. Also unlike epair: no `sched_bind()` of the loading
  thread for NUMA locality — with the module preloaded from
  loader.conf, SYSINITs run before `SI_SUB_SMP` releases the APs and
  binding to an offline CPU hangs the boot. `net.isr` tuning is now
  irrelevant to if_pair (netisr is only involved if the admin sets
  deferred dispatch, in which case its rcvif serialization covers our
  packets).
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
+37%/+74% measured) — absence of attempts, not a known dead end; the
technique's only in-tree outing (tap, for bhyve) shipped and works.
Open homework before implementing: behavior of a forwarded
`CSUM_TSO` frame reaching a non-TSO egress, and the LRO/checksum-flag
interplay with our keep-request-bits contract. Cheap ceiling
measurement first: `mtu 65535` on both sides approximates
deliver-whole TSO for pair-local TCP; benchmark against 16384 before
building anything. Working hypothesis (and the likely reason lo(4)
never needed TSO): 16K already amortizes per-traversal costs ~11x,
and the remaining loopback-style cost is per-byte — the two socket
copies — which no segment size touches; loopback could also always
raise its MTU freely (no transit, so none of TSO's scoping advantage
applies there).

**MEASURED 2026-08-15 (VM): 64K MTU 89.5 Gbit/s vs 16K MTU
86.2 Gbit/s — +3.8% for 4x fewer traversals. Hypothesis confirmed;
per-byte costs dominate beyond 16K. TSO/LRO emulation is PARKED: its
ceiling for pair-local TCP is this ~4%, which does not justify the
forwarded-CSUM_TSO verification burden and new code. The 16384
default stands, empirically; `mtu 65535` remains available to anyone
who wants the last few percent. This also empirically closes the
lo(4) question: 16K genuinely is nearly as good as TSO for
same-machine traffic.**

Benchmarking note (2026-08-15): an apparent directional throughput
asymmetry turned out to be an iperf3 `--bidir` artifact (both
directions share one client process; its CPU saturation throttles
unevenly) — use two separate unidirectional runs or two independent
client/server pairs. For the record, the driver's one real per-side
asymmetry is `sc_defqid` (flowid-less traffic serializes onto a
different pinned worker per side); a software flow-hash fallback that
would erase it is sketched in the flow-steering design discussion and
remains unimplemented for lack of a demonstrated need.

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
design).

#### Runtime test log

- 2026-08-14, NAS: ping OK; iperf3 panicked (see postmortem above).
- 2026-08-15, VM: always-queue design survives iperf3 with 1-30
  parallel connections. Found: `ifconfig pair0a destroy` returned
  `EINVAL` — `if_clone_destroy()` resolves the owning cloner via
  `ifp->if_dname` (`ifc_find_cloner_in_vnet()`), and we had set
  `if_dname` to the full "pairNa". Fixed by keeping `if_dname` =
  "pair" and putting the full name only in `if_xname`
  (`if_setname()`), as epair does (`if_epair.c:625-626`).
- 2026-08-15, VM (continued): destroy via `pair0b` failed with ENXIO —
  only the create-returned `a` ifp is linked into the cloner list (and
  the `pair` interface group!) by the framework; the `b` side needs an
  explicit `if_clone_addif()`, as epair does (`epair_clone_add()`).
  Fixed, and adopted modern epair's either-side destroy semantics
  (nested `if_clone_destroyif()` for the partner with a cleared-softc
  recursion guard) — the man page's old claim that b-side refusal
  matched epair was stale lore. Bonus fix: `b` sides are now actually
  in interface group `pair`, so `on pair` firewall rules see them.
  Retest: destroy confirmed working from either side.
- 2026-08-15, VM: destroy-under-load PASSED — pair side destroyed
  inside the iperf3 server jail while `iperf3 -c ... -P 4` ran from
  the peer jail; no panic. First live exercise of the quiesce
  protocol (both-sides-down + `NET_EPOCH_WAIT()`) against in-flight
  bidirectional transmitters, and of netisr's `m_rcvif_restore()`
  drop path for packets queued at destroy time.
- 2026-08-15, VM: `kldunload if_pair` during `iperf3 -P 4` at
  ~100 Gb/s between the jails PASSED — module-unload teardown
  (`VNET_SYSUNINIT` → `if_clone_detach` walking a list holding both
  siblings per pair, nested-destroy recursion guard) destroyed both
  interfaces cleanly under load; iperf3 fell to zero, both sides
  vanished from the jails. The ~100 Gb/s figure (VM on a laptop,
  16384 MTU, hot caches) is an observation, not a benchmark, but
  shows the always-queue datapath is not a bottleneck at these
  rates.
- 2026-08-15: pinned per-CPU worker pool implemented (see performance
  section). NOT yet runtime-tested — the full VM battery (smoke,
  iperf3 reproducer, destroy-under-load, kldunload-under-load, churn)
  must be re-run before this design is trusted; the always-queue
  netisr design was the last one validated. Workers are visible as
  `pair_task_N` in `top -SH`; multi-stream iperf3 should now spread
  across them.
- 2026-08-15, VM: interface moved OUT of a jail back to the host
  (manual `if_vmove` — the same path `vnet_if_return` takes on jail
  death), IPv4 config reapplied (addresses are stripped on any vnet
  move; standard behavior), then host-to-jail iperf3 PASSED — first
  host↔jail traffic validation, and confirms a pair migrates between
  vnets with no driver-side fixup needed.
- **Checksum elision**: the interfaces advertise TX/RX checksum offload
  for TCP/UDP over IPv4 and IPv6 (toggleable via `ifconfig ...
  [-]txcsum`) — the exact same `if_hwassist` set as `epair(4)`.
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
  Never set `CSUM_DATA_VALID | CSUM_PSEUDO_HDR` here — the input paths
  then read `csum_data` as the hardware-computed checksum value, but it
  holds the checksum field offset. Two offloads are deliberately NOT
  advertised: SCTP CRC (completion hooks exist only in kernels built
  with SCTP support, which an out-of-tree module cannot assume) and
  the IPv4 header checksum, `CSUM_IP` (`divert_packet()` completes
  pending L4 checksums before a packet reaches a divert(4) socket, but
  not `ip_sum` — an elided header sum would reach natd(8) as garbage
  and be dropped on inbound reinjection; divert(4) documents exactly
  this contract: "Packets written as incoming and having incorrect
  checksums will be dropped"). Since `CSUM_IP` is never
  pending, `pair_csum_vouch()` vouches `CSUM_IP_CHECKED |
  CSUM_IP_VALID` unconditionally, sparing the peer's `ip_input()` a
  software verification.
- **pf**: needs no special driver support (verified against
  `sys/netpfil/pf` in 15.0). pf attaches to interfaces generically via
  pfil/pfi hooks; both sides of a pair are in interface group `pair`
  for `on pair` rules (the `a` side automatically via the cloner
  framework, the `b` side via our explicit `if_clone_addif()` — until
  the 2026-08-15 fix the `b` side was in no group and `on pair` rules
  silently missed it). pf's NAT/rewrite helpers detect pending
  checksums (`CSUM_DELAY_DATA*`) and adapt instead of corrupting them,
  and `pf_route`/`pf_route6` (route-to) perform the same
  hwassist-aware checksum completion as `ip_output()`, including SCTP.
  `pair_output()` releases `CSUM_SND_TAG` send tags (as epair does)
  since `snd_tag` shares union space with `rcvif` in the pkthdr.
- **ipfw** (all kernel modules audited against 15.0 sources):
  - *Compatible — verified*: plain filtering, dynamic rules and table
    lookups (never read checksum bytes); `fwd`; **dummynet** — every
    reinjection case in `dummynet_send()` re-enters
    `ip_output(IP_FORWARDING)`/`ip6_output()` (checksum completion) or
    netisr→input (request bits honored); the `PROTO_LAYER2`/`PROTO_IFB`
    cases are unreachable for non-Ethernet interfaces; the QoS
    classifiers (ipfw opcodes, `fq_codel`/`fq_pie` flow hashing) and
    AQM modules (codel/pie) only *read* header fields, never bytes that
    could be pending; **divert/natd and `tee`** for L4
    (`divert_packet()` completes TCP/UDP/SCTP for both families before
    userland, on the `m_dup`'d copy too since `m_dup` copies pkthdr
    flags; `CSUM_IP` dropped from our hwassist for this — see above);
    **pmod/tcpmod** (MSS clamping) — explicitly offload-aware: skips
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
    checksums on traffic forwarded from a pair (or an epair — stock
    FreeBSD has the same exposure). natd(8) via divert is *not*
    affected (divert completes checksums first). Workaround:
    `ifconfig pairNb -txcsum` on pairs whose forwarded traffic passes
    through `ipfw nat` or `ng_nat`. Full analysis below.
- **MTU**: the default is 16384, matching `lo(4)` — with no Ethernet
  framing constraint, a large MTU is the cheapest way to boost bulk TCP
  throughput between vnets (fewer stack traversals per byte, similar in
  effect to TSO on loopback), and 16384 stays clear of 16-bit IP
  length-field edge cases while capturing most of the gain. Anything
  from 72 to 65535 is accepted via `ifconfig pairNa mtu ...` (set both
  sides). For traffic transiting the host toward a 1500-byte uplink,
  ordinary TCP is unaffected (MSS exchange caps segments) and in-host
  PMTUD handles the rest; to bound forwarded traffic without giving up
  the large pair-local MTU, set the MTU on the route instead of the
  interface (`route change default -mtu 1500` in the jail) — see the
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
currently in the IP header — and it is *not* a wire-format checksum.

**What libalias does to it.** libalias rewrites addresses/ports in the
raw bytes and patches checksum fields with the RFC 1624 differential
update `HC' = ~(~HC + ~m + m')`, which is only valid for complemented,
complete checksums. Applied to the non-complemented partial `P` (using
`~x ≡ −x` in one's-complement arithmetic):

```
result = ~(~P + ~m + m') = P + m − m'
needed = P − m + m'      (partial must track the address change)
error  = 2(m − m')       (correction applied with inverted sign)
```

Port rewrites corrupt it a second, independent way: port bytes lie
*inside* the region `in_delayed_cksum()` will sum, so a pending packet
needs *no* fixup for them at all — libalias applies one anyway. The
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

1. *libalias*: checksum fields hold complete, wire-format checksums —
   inherited from its userland natd(8) origin, where every packet had
   crossed or was about to cross a wire. (Its own TODO: "make libalias
   mbuf aware".)
2. *`ip_fw_nat`*: a pending checksum implies a locally-originated
   packet mid-`ip_output()` (`rcvif == NULL`); equivalently, *received*
   packets always carry complete checksums. Historically airtight —
   every `if_output` led to a wire. epair-with-txcsum and if_pair
   exist to violate exactly this: their `if_output` is an *input*
   source, producing packets with `rcvif` set and checksums pending.
   The repair would work for them; it is simply never triggered, while
   libalias still corrupts the field.
3. *`ng_nat`*: differential fixups need repair only when libalias
   edited payload (`TH_RES1` mark). That case is handled fully
   offload-aware (`ng_nat.c` rebuilds the pseudo-header and keeps
   `CSUM_TCP` packets pending), but plain address/port rewrites get no
   repair and not even the `rcvif == NULL` pre-pass — which also bites
   locally-originated pending packets reaching `ng_nat` below
   `ip_output()`'s completion point (e.g. via `ng_ether` on a txcsum
   NIC); hence the long-standing folklore "disable checksum offload
   when using netgraph NAT".

Incidentally, libalias also differentially patches `ip_sum` — which
would have been a third instance of the same corruption had if_pair
kept `CSUM_IP` in its hwassist. With it dropped, the header sum
crossing a pair is genuine wire-format bytes and that fixup is valid.

**Fixes.** Driver-side there is none: `pair_output()` cannot know a
packet will later meet libalias, and preemptive completion would
forfeit elision for all traffic. Upstream, both fixes are small:
widen the `ip_fw_nat` gate to `csum_flags & (CSUM_DELAY_DATA |
CSUM_DELAY_DATA_IPV6)` (drop the `rcvif == NULL` conjunct — the
repair rebuilds from final addresses, so origin is irrelevant), and
add the same pre-pass to `ng_nat`. Until then: `-txcsum` on affected
pairs. Testable prediction for the lab box: forwarded epair traffic
through in-kernel `ipfw nat` should exhibit this on stock FreeBSD
today, and `-txcsum` on the epair should make it disappear.

The module builds clean under `-Werror` on FreeBSD 15.0-RELEASE amd64.

Teardown is epoch-synchronized per the epoch(9) contract (wait-then-
free is its documented reclamation pattern; the no-mutexes-across-wait
rule holds on every destroy path): `pair_output()` asserts it runs
within the network epoch (true for all entry paths — verified in the
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
| SCTP delayed-CRC completion in the forwarding paths | `bcb298fa9e23` (same) | — |
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
lands on code moves, imports, or unrelated churn — or the claim is a
negative one no commit can attest): `ip_input()` having no request-bit
shortcut; the `ip_fw_nat.c` `rcvif == NULL` gate and libalias's
offload-unawareness (rationale exists only in the code comments
themselves); the `dummynet_send()` reinjection paths (2010 wholesale
ipfw3 import); `bpfwrite()` entering the net epoch (`bpf.c`, epoch
entry directly before its `if_output` call); epair's a-side-only
destroy semantics; `lo(4)`'s treatment of bpf-injected packets.

Link state (implemented 2026-08-15): both sides report a synthetic
carrier via `pair_set_state()` — `LINK_STATE_UP` coupled with
`IFF_DRV_RUNNING` at attach, `LINK_STATE_DOWN` as the first teardown
step (epair's `epair_set_state()` ordering), with `IFCAP_LINKSTATE`
advertised (lo(4) precedent) and force-enabled. The peer-reflecting
variant (carrier mirrors the other side's admin state — the truer p2p
semantic) is deferred: `SIOCSIFFLAGS` runs outside the network epoch,
so touching `sc_peer` there would reopen the closed teardown race; if
ever wanted, it needs an epoch section (or equivalent) around the
peer access plus a cross-vnet `if_link_state_change()`. Note:
`ifconfig` may not print a `status:` line for a mediumless interface;
the functional consumers (routing daemons, devd, route-socket
listeners) receive the state regardless — VM test should verify with
`ifconfig -v` / `route -n monitor` during create/destroy.

Known open items:
- Runtime verification pending (needs root): smoke test, destroy/unload
  paths with live pairs and jailed `b` sides, transit checksum test via
  tcpdump on a real egress, iperf3 comparison against epair.
- MTU is not synchronized between the two sides (documented: set both).
- The man page is not installed by the Makefile.
