#!/bin/sh
# Run the kwq_test scenarios in the test guest and print kwq's counters.
# Usage: tests/run_p1b.sh [scenario ...]   (default: all P0 and P1b scenarios)
# Runs through tests/kwqvm.sh ssh; the modules must already be in $MODDIR
# (default /root/kwq, from tests/kwqvm.sh mods; /root/kwq-generic from
# 'mods generic' for the stock kernel).  Scenario knobs: kwq_test/kwq_test.c.
set -eu
cd "$(dirname "$0")"
MODDIR=${MODDIR:-/root/kwq}
# KWQ_SSH: how to reach a root shell on the test machine (default: the
# bhyve guest).  For a07: KWQ_SSH="ssh a07 doas -n" MODDIR=/home/crest/kwq-mods
KWQ_SSH=${KWQ_SSH:-./kwqvm.sh ssh}
scenarios=${*:-"lifecycle fifo notify reject discard fairness fairness2 latency gaming overrun yield cost tq_baseline switch_baseline"}
$KWQ_SSH 'sh -s' <<GUEST
kldstat -q -m kwq_test && kldunload kwq_test; kldstat -q -m kwq && kldunload kwq
kldload $MODDIR/kwq.ko && kldload $MODDIR/kwq_test.ko || { echo LOADFAIL; exit 1; }
uname -v | sed 's/^/kernel: /'; sysctl -n kern.hz hw.ncpu hw.machine | tr '\\n' ' ' | sed 's/^/hz ncpu machine: /'; echo
run() { name=\$1; shift
	sysctl -q kern.kwq_test.cost_us=0 kern.kwq_test.limit=0 kern.kwq_test.cpu=-1 kern.kwq_test.items=1000 kern.kwq_test.reps=10 kern.kwq_test.secs=3 kern.kwq_test.weight_a=1 kern.kwq_test.weight_b=1 kern.kwq_test.cost_a=50 kern.kwq_test.cost_b=500 kern.kwq_test.batch=1 kern.kwq_test.pairs=1 kern.kwq_test.fanin=1 >/dev/null
	for kv in "\$@"; do sysctl -q kern.kwq_test.\$kv >/dev/null; done
	sysctl -q kern.kwq_test.scenario=\$name >/dev/null; start=\$(date +%s); sysctl -q kern.kwq_test.run=1 >/dev/null
	while [ "\$(sysctl -n kern.kwq_test.result_state)" = running ]; do sleep 0.2; [ \$(( \$(date +%s) - start )) -gt 120 ] && { echo "\$name: TIMEOUT"; return 1; }; done
	R() { sysctl -n kern.kwq_test.result_\$1; }
	printf '%-16s %-5s in=%s out=%s rej=%s runs=%s cyc_a=%s cyc_b=%s lat_a=%sus lat_b=%sus ns/item=%s prod=%s ns=%s %s\n' "\$name" "\$(R state)" "\$(R in)" "\$(R out)" "\$(R rejected)" "\$(R runs)" "\$(R cycles_a)" "\$(R cycles_b)" "\$(R maxlat_a_us)" "\$(R maxlat_b_us)" "\$(R ns_per_item)" "\$(R prod_ns_per_item)" "\$(R ns)" "\$(R msg)"
	[ "\$(R state)" = done ]
}
C() { sysctl kern.kwq.net.queue.kta.cpu.1 kern.kwq.net.queue.ktb.cpu.1 2>/dev/null | grep -v ": 0\$" | grep -E "boosts|grace|parks|overruns|passes|items|maxlat|requeued" | sed 's/^/    /'
      sysctl kern.kwq.net.cpu.1 | grep -E "rounds|yields|handbacks|wakeups" | tr '\n' ' ' | sed 's/kern.kwq.net.cpu.1.//g; s/^/    worker: /; s/\$/\n/'; }
rc=0
for s in $scenarios; do
	case \$s in
	lifecycle) run lifecycle reps=100 || rc=1 ;;
	fifo) run fifo items=100000 reps=2 || rc=1 ;;
	notify) run notify items=100000 || rc=1 ;;
	reject) run reject limit=8 cost_us=20 items=5000 reps=2 || rc=1 ;;
	discard) run discard cost_us=20 limit=100000 items=5000 reps=2 || rc=1 ;;
	fairness) run fairness || rc=1; C ;;
	fairness2) run fairness weight_a=2 || rc=1; C ;;
	latency) run latency cost_a=20 || rc=1; C ;;
	gaming) run gaming || rc=1; C ;;
	overrun) run overrun || rc=1; C ;;
	yield) run yield || rc=1; C ;;
	yield0) run yield items=0 || rc=1 ;;	# callout only: VM/timer noise baseline
	cost16) run cost items=1024 batch=16 || rc=1 ;;	# S16 batching: 16 items per kwq_enqueue_list()
	scale) run scale items=1024 batch=${BATCH:-16} pairs=${PAIRS:-1} || rc=1 ;;	# PAIRS producer/consumer pairs on disjoint CPUs
	fanin) run fanin items=1024 batch=${BATCH:-16} fanin=${FANIN:-2} limit=1000000 || rc=1 ;;	# FANIN producers into one (queue, CPU)
	cost) b0=\$(sysctl -n kern.kwq.net.cpu.1.busy_ns); run cost items=1024 || rc=1; b1=\$(sysctl -n kern.kwq.net.cpu.1.busy_ns); echo "    consumer busy ns per item: \$(( (b1 - b0) / \$(sysctl -n kern.kwq_test.result_out) )) (cpu_ticks glitches discarded by the test: \$(sysctl -n kern.kwq_test.result_glitches))" ;;
	tq_baseline) run tq_baseline items=1024 || rc=1 ;;
	switch_baseline) run switch_baseline || rc=1 ;;
	*) echo "unknown scenario \$s"; rc=1 ;;
	esac
done
dmesg | grep -i -E "witness|lock order|panic" | grep -v "WITNESS option" | tail -3
exit \$rc
GUEST
