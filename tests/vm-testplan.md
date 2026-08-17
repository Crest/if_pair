# if_pair VM test plan

Ordered stages; stop at the first failure and capture state. The
always-queue rework (post-iperf3-panic) has never been runtime-tested
- stage 2 is the old crash reproducer and the most important test in
this file.

## Stage 0 - VM preparation

Getting the module in (either works):

- Copy just the module: `scp /usr/obj/home/crest/if_pair/if_pair.ko vm:`
  - fine if the VM kernel is any 15.0 (module records a dependency on
  kernel.1500068; kldload will refuse a mismatch, not crash).
- Or copy the source and build in the VM (needs /usr/src there).

Crash capture (do this first - a panic without a dump wastes a crash):

```sh
sysrc dumpdev=AUTO && service dumpon start
ls /var/crash            # after any panic: core.txt.N / vmcore.N
```

Nice to have: an INVARIANTS+WITNESS kernel makes lock/epoch mistakes
announce themselves instead of corrupting. Check with:
`sysctl kern.conftxt | grep -cE "INVARIANTS|WITNESS"` (0 on stock
GENERIC). Stock is still useful - the VM's job is crash containment.

Baseline notes to record: `sysctl net.isr.maxthreads net.isr.dispatch`,
`uname -a`.

## Stage 1 - smoke

```sh
kldload ./if_pair.ko
sh tests/smoke.sh        # jails, IPv4+IPv6 ping, b-side destroy refusal
```

Also eyeball: `ifconfig pair0a` (flags, mtu 16384),
`tcpdump -c 5 -n -i pair0a` during a ping (clean IP, no Ethernet).

## Stage 2 - the crash reproducer (iperf3)

This is the exact scenario that panicked the NAS under the old
inline-dispatch design. Expected now: survives indefinitely.

```sh
# jails dummy1/dummy2 with a pair between them, as in smoke.sh, then:
jexec dummy2 iperf3 -s -D
jexec dummy1 iperf3 -c <dummy2-addr> -t 60
jexec dummy1 iperf3 -c <dummy2-addr> -t 60 -R       # reverse
jexec dummy1 iperf3 -c <dummy2-addr> -t 60 -P 8     # parallel streams
# both directions at once (the ABBA-shaped load):
jexec dummy1 iperf3 -c <dummy2-addr> -t 120 &
jexec dummy2 iperf3 -c <dummy1-addr> -t 120 -p 5202 &
```

Watch during runs: `netstat -Q` (netisr queue lengths/drops),
`netstat -id | grep pair` (Idrop column = ring/queue drops).
Capture counters **while traffic is running / before any destroy** -
interface counters vanish with the interface, so post-destroy
`netstat` can only confirm removal, never drop behavior.

## Stage 3 - abuse the queue path

```sh
# UDP blast (expect IQDROPS to count, not a crash):
jexec dummy1 iperf3 -c <dummy2-addr> -u -b 10G -t 30
# latency sanity + small packets:
jexec dummy1 ping -f -c 100000 <dummy2-addr>
# MTU sweep:
jexec dummy1 ping -s 16000 -c 10 <dummy2-addr>          # near-MTU
ifconfig pair0a mtu 1500; jexec dummy1 ifconfig pair0b mtu 1500
jexec dummy1 iperf3 -c <dummy2-addr> -t 30              # re-run at 1500
```

## Stage 4 - teardown under fire (where drivers actually die)

```sh
# destroy while iperf3 is running (expect: connections drop, no panic):
jexec dummy1 iperf3 -c <dummy2-addr> -t 600 &
sleep 5; ifconfig pair0a destroy

# jail death with a live pair end inside:
#   (pair created on host, b moved in -> expect b returns to host)
jail -r dummy2

# module unload with live pairs (cloner detach path):
ifconfig pair create; ifconfig pair create
kldunload if_pair

# create/destroy churn:
kldload ./if_pair.ko
for i in $(seq 50); do ifconfig pair create >/dev/null; done
for i in $(seq 0 49); do ifconfig pair${i}a destroy; done
kldunload if_pair
```

Create/destroy race stress - targets the create-return window and is
the regression test for the destroy-side wait-retry (before that
mitigation, this recipe was the way to plant the stale
cloner-list/group corruption; on epair(4) the same recipe panics):

```sh
kldload ./if_pair.ko
# job 1: churn unit 0 as fast as possible (each destroy frees the
# unit, so the wildcard create keeps producing pair0a/pair0b)
while :; do
    ifconfig pair create >/dev/null 2>&1
    ifconfig pair0a destroy 2>/dev/null
done &
# job 2: hammer the b side, aiming for the gap between create's
# return and the framework linking pair0a
while :; do ifconfig pair0b destroy 2>/dev/null; done &
sleep 60; kill %1 %2; wait
ifconfig pair0a destroy 2>/dev/null   # reap a possible survivor
```

Expected: no panic, and afterwards `ifconfig -g pair` lists only
interfaces that actually exist - ghost members are the old
corruption's signature.  Follow with `kldunload if_pair` and the
four-line verification below.  An INVARIANTS kernel is especially
valuable here: UMA trashing turns the old corruption into a
deterministic panic at first touch, so a survived run carries real
weight.  The window is microseconds wide, so a clean run mostly
proves the retry plumbing (parked destroyers, ENXIO retries, pause
wakeups) under contention rather than exhaustively hitting the race
- that is expected; run it longer than 60s when in doubt.

After any `kldunload`, verify nothing of the module remains (run
`vmstat -m | grep if_pair` BEFORE the unload to see live allocations;
after it, the malloc type is unregistered and grep finds nothing):

```sh
dmesg | tail -n 20 | grep -i leaked   # kernel warns "memory type if_pair
                                      # leaked memory" if any M_PAIR
                                      # allocation was not freed
procstat -ta | grep pair_task         # no worker threads left
ifconfig -g pair                      # no members; group gone
vmstat -m | grep if_pair              # no output: type unregistered
```

All four must come back empty/silent.

## Stage 5 - only after 1-4 are green

- sendfile / unmapped-mbuf passage: serve a large file across the
  pair (e.g. `jexec dummy2 python3 -m http.server` + fetch from
  dummy1, or nginx+sendfile). Exercises M_EXTPG mbuf chains through
  pair_output/pair_input - our version-nibble mtod() read relies on
  TCP keeping headers in a mapped mbuf; verify with a multi-GB
  transfer, and compare throughput vs plain iperf3 (sendfile skips
  the sender-side copy, the dominant remaining cost).

- Transit checksum test (needs a second interface): jail -> host ->
  egress; `tcpdump -v -n -i <egress>` must show valid checksums.
- `-txcsum` toggles on one/both sides + re-run iperf3.
- Performance numbers: VM-on-laptop numbers are not representative;
  functional pass here, benchmark on real hardware later.

## If it panics

```sh
kgdb /boot/kernel/kernel /var/crash/vmcore.last   # bt, then:
# 'bt' of the panicking thread; 'ps' for what else ran
```

Save core.txt.N + the exact test that triggered it.
