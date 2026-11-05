#!/bin/sh
# guest_check.sh (guest, root): BULK's new defaults (quantum_us 5000,
# cap_period_us 10000) on the debug kernel: the kwq suite, kwq-geli's suite
# with kwq.ko preloaded, and geli_bench's new stage, knob and rows quickly
# (eli kwqshard kwqshard1: tput resp onecpu, scale 4 16, tput at 32 KiB
# sectors), then the console for witness, lock order, panic or assert.
G=/root/ifpair-scratch/stages/p6g6; M=$G/mods; L=/root/ifpair-scratch/settings_guest.log
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
kldload $M/kwq.ko || exit 1
echo "defaults: bulk quantum_us=$(sysctl -n kern.kwq.bulk.quantum_us) cap_period_us=$(sysctl -n kern.kwq.bulk.cap_period_us) cap_permille=$(sysctl -n kern.kwq.bulk.cap_permille); net $(sysctl -n kern.kwq.net.quantum_us)/$(sysctl -n kern.kwq.net.cap_period_us); blocking $(sysctl -n kern.kwq.blocking.quantum_us)/$(sysctl -n kern.kwq.blocking.cap_period_us)"
kldunload kwq
kyua -v test_suites.kwq.moddir=$M test -k $G/kwq-tests/Kyuafile > $G/kyua_kwq.out 2>&1
echo "kwq suite: $(tail -1 $G/kyua_kwq.out)"
kyua report --results-filter=failed,broken 2>/dev/null | egrep -- "->" | head
kldload $M/kwq.ko || exit 1
kyua -v test_suites.kwqeli.moddir=$M test -k $G/geli-tests/Kyuafile > $G/kyua_geli.out 2>&1
echo "kwq-geli suite: $(tail -1 $G/kyua_geli.out)"
kyua report --results-filter=failed,broken 2>/dev/null | egrep -- "->" | head
kldunload kwq
cc -O2 -o $G/gb/victim $G/gb/tests/victim.c -lm || exit 1
B=$G/gb/kwq-geli/tests/geli_bench.sh
env MODDIR=$M VICTIM=$G/gb/victim OUT_TSV=$G/gb/tput.tsv SETTINGS_LOG=$L REPS=1 SECS=4 PROBE_S=2 MB=64 MDSIZE=128 \
    TESTS="tput resp onecpu" sh $B eli kwqshard kwqshard1 > $G/gb/tput.log 2>&1
echo "tput resp onecpu exit $?: $(wc -l < $G/gb/tput.tsv) rows"
env MODDIR=$M VICTIM=$G/gb/victim OUT_TSV=$G/gb/scale.tsv SETTINGS_LOG=$L REPS=1 PROBE_S=2 IDLE_S=1 \
    SCALE_NS="4 16" SCALE_MDSIZE=32 TESTS=scale sh $B eli kwqshard kwqshard1 > $G/gb/scale.log 2>&1
echo "scale exit $?: $(wc -l < $G/gb/scale.tsv) rows"
env MODDIR=$M VICTIM=$G/gb/victim OUT_TSV=$G/gb/tput32k.tsv SETTINGS_LOG=$L REPS=1 SECS=4 MB=64 MDSIZE=128 \
    ELI_SECSIZE=32768 TESTS=tput sh $B eli kwqshard kwqshard1 > $G/gb/tput32k.log 2>&1
echo "tput 32 KiB sectors exit $?: $(wc -l < $G/gb/tput32k.tsv) rows"
grep -h "kwq_\(read_\)\?\(requeued\|items\|overruns\|cap_cuts\)\|kwq_bulk_\(requeued\|items\)\|_mbps	" $G/gb/tput.tsv $G/gb/scale.tsv $G/gb/tput32k.tsv | grep -v "prov_" | head -60
tail -12 $L
echo "modules: $(kldstat | grep -ciE 'kwq|kwqeli|nop'), md: $(mdconfig -l | wc -w)"
dmesg | egrep -i "witness|lock order|panic|assert|kwq: " | head -5
uptime
