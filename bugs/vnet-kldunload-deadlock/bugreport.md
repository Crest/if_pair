# kldunload of an interface cloner deadlocks against the destruction of a vnet jail (vnet_destroy vs. vnet_deregister_sysuninit lock order)

Draft bug report, prepared 2026-10-27.  Product: FreeBSD base system,
kernel, sys/net.  Reproduced on 15.1-RELEASE-p3 with the stock epair(4)
driver alone (section "How to reproduce").  main has the same code in
all three functions as of 2026-10-27 (sys/net/vnet.c and
sys/net/if_clone.c on cgit), so the inversion is present there by
inspection; it was not run on a CURRENT kernel.  Evidence in
`evidence/` (listed at the end).

## Summary

Unloading a kernel module that registers an interface cloner per vnet
(any driver whose `VNET_SYSUNINIT` calls `if_clone_detach()`: epair(4),
vlan(4), the out-of-tree if_pair driver this was found with) while a
vnet jail is being destroyed deadlocks the kernel.  Two global sx locks
are taken in opposite orders:

- `vnet_destroy()` (sys/net/vnet.c:300) takes `ifnet_detach_sxlock`
  exclusively, then `vnet_sysuninit()` takes `vnet_sysinit_sxlock`
  shared (vnet.c:644).
- `vnet_deregister_sysuninit()` (vnet.c:602) takes `vnet_sysinit_sxlock`
  exclusively, then calls the module's destructor for every vnet;
  `if_clone_detach()` takes `ifnet_detach_sxlock` exclusively
  (sys/net/if_clone.c:685).

Both threads block for ever in D state.  Since `vnet_sysinit_sxlock` is
then held exclusively, every later vnet creation and destruction (jail
-c with vnet, jail -r) hangs too.  The only way out is a reset.

## Environment where it was hit

- FreeBSD 15.1-RELEASE-p3, releng/15.1-n283611-88e7371d9dc2, amd64,
  GENERIC-DEBUG (WITNESS, INVARIANTS), bhyve guest with 4 vCPUs.
- The module was the out-of-tree if_pair.ko (a layer-3 epair
  alternative); its vnet hooks are `VNET_SYSINIT` ->
  `ifc_attach_cloner()`, `VNET_SYSUNINIT` -> `ifc_detach_cloner()`,
  the same shape as `vnet_epair_uninit()` in sys/net/if_epair.c:922.
  Nothing of the driver is on either stack; the cycle is entirely in
  sys/net/vnet.c and sys/net/if_clone.c.
- The workload was a test harness that, per stage, creates two vnet
  jails connected by a pair, runs iperf3 between them, removes both
  jails with `jail -r` and unloads the driver at once.  The guest had
  gone through roughly two hundred such teardowns on this boot before
  the hang; two hardware machines with GENERIC kernels went through
  several thousand without hitting it.

## What was observed

At 11:42 guest time `kldunload if_pair` never returned.  28 minutes
later (evidence/guest-stacks-2026-10-27.txt):

```
$ procstat -kk 18449
18449 100131 kldunload  mi_switch sleepq_switch _sx_xlock_hard _sx_xlock if_clone_detach+0xe7
    vnet_deregister_sysuninit+0x95 linker_file_sysuninit+0x147 linker_file_unload+0x2a9
    kern_kldunload+0x190 amd64_syscall fast_syscall_common
$ procstat -kk -a | grep _sx_
    0 100017 kernel  jail_remove taskq  mi_switch sleepq_switch _sx_slock_hard _sx_slock_int
    vnet_destroy+0x128 prison_deref+0xaf5 taskqueue_run_locked taskqueue_thread_loop fork_exit
$ ps axo pid,stat,wchan,etime,command | grep kldunload
18449  D  ifnet_de  28:20  kldunload if_pair
$ jls -d jid name dying
875 ifp_compare_c true
```

No other thread was blocked on a lock.  `vnet_destroy()`'s `_sx_slock`
is the `VNET_SYSINIT_RLOCK()` of the inlined `vnet_sysuninit()`; at that
point it holds `ifnet_detach_sxlock` (vnet.c:300).  `if_clone_detach()`'s
`_sx_xlock` at offset 0xe7 is if_clone.c:685; at that point
`vnet_deregister_sysuninit()` holds `vnet_sysinit_sxlock` exclusively
(vnet.c:602) and the vnet list lock shared (vnet.c:603).  The state was
preserved with `sysctl debug.kdb.panic=1` (dump 2026-10-27 12:12:52
UTC, evidence/guest-env-witness-info0.txt) and the guest rebooted.

WITNESS had not reported the pair on that boot (its message buffer had
long wrapped; the boot was 18 days old).  On the next boot the very first
teardown of the same harness produced this report, at the same line
(evidence/guest-env-witness-info0.txt):

```
lock order reversal:
 1st 0xffffffff822d86a8 vnet_sxlock (vnet_sxlock, sx) @ /usr/src/sys/net/vnet.c:603
 2nd 0xffffffff822d5d98 ifnet_detach_sx (ifnet_detach_sx, sx) @ /usr/src/sys/net/if_clone.c:685
lock order vnet_sxlock -> ifnet_detach_sx attempted at:
#0 witness_checkorder+0xbe1
#1 _sx_xlock+0x60
#2 if_clone_detach+0xe7
#3 vnet_deregister_sysuninit+0x95
#4 linker_file_sysuninit+0x147
#5 linker_file_unload+0x2a9
#6 kern_kldunload+0x190
```

WITNESS names the vnet list lock (`vnet_sxlock`, held shared on the same
path) against the detach lock; the lock that actually deadlocked is
`vnet_sysinit_sxlock`, held exclusively on the same path and not
mentioned because WITNESS reports one reversal per acquisition.

## Why it is reachable

`jail -r` on a vnet jail returns immediately.  The jail stays in the
dying state, listed by `jls -d`, while anything still references it:
its processes until they have exited, sockets in TIME_WAIT for their
2MSL, so a jail that had TCP connections stays listed for up to a
minute.  When the last reference drops, `prison_deref()` marks the
prison invalid and removes it from `allprison`
(sys/kern/kern_jail.c:3550), releases `allprison_lock`, and only then
calls `vnet_destroy()` (kern_jail.c:3595), in whatever context dropped
the reference: the `jail -r` syscall itself when nothing else held the
jail, an exiting process, or the jail_remove taskqueue when the drop
came from a context that cannot sleep.  A `kldunload` of any per-vnet
cloner whose `vnet_deregister_sysuninit()` overlaps that
`vnet_sysuninit()` chain deadlocks.  So `jls -d` shows the long,
harmless phase and not the critical one: during `vnet_destroy()` the
jail is already gone from the list (the reproducer's deadlocked jail is
not listed, evidence/guest-epair-repro-2026-10-27.txt).  Waiting for a
jail to disappear from `jls -d` and unloading at once lands exactly in
the window; a userland workaround has to wait a further moment after
the disappearance.  The chain takes 25 to 65 ms per vnet on the debug
guest (dtrace, evidence/guest-epair-repro-2026-10-27.txt), less on a
production kernel.

## History

- `ifnet_detach_sxlock` was introduced by e133271fc1b5 / D27279
  (2020-11) to serialize epair destruction against `vnet_if_return()`.
- 6d2a10d96fb5 (Kristof Provost, 2021-02-08, D28530, "Widen the
  ifnet_detach_sxlock to cover the entire vnet sysuninit code")
  moved the acquisition in `vnet_destroy()` around the whole
  `vnet_sysuninit()` so that the UDP pcb cannot be freed under the
  detach code.  From then on `vnet_destroy()` holds the detach lock
  while it takes the sysinit lock shared (evidence/commit-6d2a10d96fb5-
  widen-ifnet_detach_sxlock.diff).  `vnet_deregister_sysuninit()` has
  held the sysinit lock exclusively around the destructor calls since
  the vnet code was written.  The inversion exists since this commit
  for every cloner that destroys interfaces from its `VNET_SYSUNINIT`,
  but only when interfaces remain to be destroyed.
- c624e04a0bd0 (Zhenlei Huang, 2026-04-13, D56298, "if_clone: Make
  ifnet_detach_sxlock opaque to consumers", in release/15.1.0) moved
  the `sx_xlock(&ifnet_detach_sxlock)` from inside
  `if_clone_destroyif()` (once per interface) to `if_clone_detach()`
  around the whole loop, unconditionally (evidence/commit-c624e04a0bd0-
  if_clone-opaque-lock.diff).  Since then the lock is taken even when
  the cloner has no interfaces left.  That defeated the one driver-side
  mitigation that existed: if_pair destroys every interface from
  MOD_UNLOAD, before the SYSUNINITs run, precisely so that its
  `VNET_SYSUNINIT` would find empty lists and never take the detach
  lock under the sysinit lock (the comment above `pair_sweep()` in
  evidence/if_pair.c describes the cycle and names epair).  On 15.1 the
  sweep still empties the lists and the deadlock happens anyway.

## How to reproduce

evidence/reproducer-epair.sh, stock epair(4) only, nothing else loaded
(run on the guest with evidence/repro-driver.sh from the host).  Each
round: load if_epair, create two epairs, give one end of each to a new
vnet jail, start one process in the first jail, `jail -r` the first
jail (returns at once; its vnet dies on the jail_remove taskqueue when
the process exits, a chain of about 30 ms), start `kldunload if_epair`
in the background, `jail -r` the second jail (no process: its vnet dies
synchronously in the syscall, after waiting for `ifnet_detach_sxlock`
behind the first chain).  Three parties then race: the unload has
queued as a writer on `vnet_sysinit_sxlock` during the first chain; when
that chain releases the lock the writer and the second `vnet_destroy()`
wake together; if the writer gets the sysinit lock and the second
teardown gets the detach lock, both block for ever.  Signature: the
`kldunload` in D state on wchan `ifnet_de` and the `jail -r` in D state
on wchan `vnet_sys`, with these stacks
(evidence/guest-epair-repro-2026-10-27.txt):

```
54588 100253 kldunload  _sx_xlock_hard _sx_xlock if_clone_detach+0xe7 vnet_deregister_sysuninit+0x95
    linker_file_sysuninit+0x147 linker_file_unload+0x2a9 kern_kldunload+0x190
54589 100588 jail       _sx_slock_hard _sx_slock_int vnet_destroy+0x128 prison_deref+0xaf5
    sys_jail_remove+0x15c
```

Two shapes that never hit, and why (dtrace of one round each, same
file): jails without a process, where `jail -r` destroys the vnet
synchronously before it returns, 0 of 200 rounds; a process in both
jails, where both teardowns run back to back on the one jail_remove
thread and the second re-takes the shared lock before the woken writer
can, 0 of 100 rounds.  Reliability (evidence/repro-trials.txt): five trials of up to 300
rounds each, all five deadlocked, none reached 300 rounds.  The four
trials run with the final detector hit at rounds 23, 52, 25 and 25 (56
to 87 seconds of wall time each); a first run before the detector was
fixed hit by round 234.  So in the hitting shape the deadlock is
reached well within a hundred rounds and is easy to trigger, not a
one-in-a-thousand race.  On GENERIC-DEBUG the deadlock resolver
(DEADLKRES) panics the hang on its own after about thirty minutes if
nothing else does ("possible deadlock detected ... (jail)",
evidence/guest-epair-deadlkres-panic.txt); the trials panicked it at
the freeze instead, for a faster reboot.

The dump of the stock-epair deadlock is evidence/guest-epair-vmcore.1
with guest-epair-info.1.  The harness that first hit it is
tests/compare.sh of the if_pair tree (evidence/compare.sh,
`stage_down()`), log of that run in
evidence/guest-charge2_guest_intr.log (it stops after the CHW stage's
udp_load test, whose teardown started the kldunload).

## Possible fixes

Either establish one order or stop holding the sysinit lock across the
destructor calls:

1. In `vnet_destroy()`, take `vnet_sysinit_sxlock` shared before
   `ifnet_detach_sxlock`, so both paths order sysinit lock first.
   `vnet_sysuninit()` would then run under a lock its caller already
   holds shared; today it takes it itself.
2. In `vnet_deregister_sysuninit()`, do not hold `vnet_sysinit_sxlock`
   exclusively while calling `vs->func()` for each vnet; the lock
   protects the destructor list, and the loop over vnets runs under
   the vnet list lock.  A try-lock with back-off on
   `ifnet_detach_sxlock` in `if_clone_detach()` would be the retry
   variant of the same idea.

A module cannot work around it: on the unload path it owns neither
lock (the sysinit lock is its caller's, the detach lock is taken by
`if_clone_detach()` after the module's code has returned into it), it
cannot veto any more (MOD_UNLOAD has already succeeded), and it cannot
tell that a `vnet_destroy()` is running.  The one thing a driver can do
is detach its per-vnet cloners from MOD_UNLOAD itself, under the detach
lock, and make its `VNET_SYSUNINIT` a no-op on the unload path; that
removes that driver from the cycle and leaves epair and the others in
it.

That driver-side avoidance is implemented for if_pair (commit 3be931a on
the kwq branch): pair_detach_cloners() detaches every vnet's cloner from
MOD_UNLOAD, walking the vnet list and taking the detach lock in base's
own order (list, then detach), and vnet_pair_uninit() then takes no lock
on the unload path.  Verified on the test VM
(evidence/guest-ifpair-fix-verify-2026-10-27.txt): with the same
reproducer and a patient 180 s freeze threshold, the pre-fix module
deadlocks permanently (round 111, the stacks above) while the fixed
module completes 500 rounds.  It only removes if_pair from the cycle;
epair, vlan and every other per-vnet cloner keep the base bug, so this
PR is still the real resolution.

## Workaround for users

Do not unload a per-vnet cloner module while a vnet jail may be dying.
`jls -d` lists dying jails until their last reference drops; the vnet
teardown that can deadlock runs right after that, invisibly, so wait
until the jails are gone from the listing and then a further second or
two.  The if_pair harness does that now (evidence/0001-tests-Wait-for-
the-stage-s-jails-to-die-before-unloa.patch and the follow-up commit
8723f42 that adds the delay after the disappearance); a mitigation, not
a fix.

## Evidence (evidence/)

| file | what |
|---|---|
| guest-stacks-2026-10-27.txt | procstat -kk of both threads, ps, kldstat, jls -d, vmstat -m, 28 min into the hang |
| guest-env-witness-info0.txt | uname, kernel ident, the WITNESS report of the next boot, messages around the hang, the dump header (info.0), /var/crash listing |
| guest-charge2_guest_intr.log | the harness log of the run that hung |
| guest-vmcore.0, guest-info.0 | symlinks to the copy of the dump and its header in ~/if_pair-scratch/crash (591 MB; the original stays on the guest) |
| guest-kernel.debug | symlink to the debug kernel of the guest's build (/bhyve/obj-15.1/.../GENERIC-DEBUG/kernel.debug) for kgdb |
| reproducer-epair.sh, repro-driver.sh, repro-trials.txt | the stock-epair reproducer, the host-side driver that detects the hang and reboots the guest, and the trial results |
| guest-epair-repro-2026-10-27.txt | state of the guest deadlocked by the stock-epair reproducer: stacks, ps, jls -d (empty), round count |
| guest-epair-vmcore.1, guest-epair-info.1 | symlinks to the dump of the stock-epair deadlock (352 MB) |
| guest-epair-deadlkres-panic.txt | the DEADLKRES auto-panic message naming the blocked jail thread |
| repro-watch.sh | the host-side watcher that detects the hang by the frozen round counter (no procstat) and reboots the guest between trials |
| reproducer-ifpair.sh | the same hitting shape for if_pair, used to verify the driver fix (kwq kept loaded, two vnet jails, unload racing a dying vnet) |
| guest-ifpair-fix-verify-2026-10-27.txt | the fix verification: pre-fix deadlocks permanently (stacks), fixed module clears 500 rounds; and the transient-epoch-drain trap in measuring it |
| commit-6d2a10d96fb5-widen-ifnet_detach_sxlock.diff | the commit that created the inversion |
| commit-c624e04a0bd0-if_clone-opaque-lock.diff, -tags.txt | the commit that made it unconditional, and the release tags containing it |
| 0001-tests-Wait-for-the-stage-s-jails-to-die-before-unloa.patch | the harness mitigation |
| src-15.1-sys-net-vnet.c, -if_clone.c, -if_epair.c, src-15.1-sys-kern-kern_jail.c, -kern_linker.c | symlinks into the 15.1 source tree the guest kernel was built from (/bhyve/src-15.1, head in src-15.1-head.txt) |
| if_pair.c, compare.sh | symlinks into the if_pair tree: the driver with the defeated mitigation (pair_sweep) and the harness |

No crashinfo(8) output exists yet: neither the guest nor the host has
devel/gdb installed, which crashinfo needs.  With kgdb on any machine,
`kgdb evidence/guest-kernel.debug evidence/guest-vmcore.0` and
`info threads` / `bt` on tids 100017 and 100131 reproduce the stacks
above from the dump, and the sx owners can be read from
`vnet_sysinit_sxlock` and `ifnet_detach_sxlock`.
