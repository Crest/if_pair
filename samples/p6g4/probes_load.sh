#!/bin/sh
# probes_load.sh (guest or a07, root): the loads that fire every BULK
# placement probe: kwq_test steal and anyburst under place 2, 0, 1, starved
# under 2, kwq_crypto with one stealable queue fed on CPU 1 under 2.
SU=${SU:-}
run() {
	name=$1; shift
	for kv; do $SU sysctl -q kern.kwq_test.$kv >/dev/null; done
	$SU sysctl -q kern.kwq_test.scenario=$name >/dev/null
	$SU sysctl -q kern.kwq_test.run=1 >/dev/null
	while [ "$(sysctl -n kern.kwq_test.result_state)" = running ]; do sleep 0.2; done
	echo "$name place=$(sysctl -n kern.kwq.bulk.place): $(sysctl -n kern.kwq_test.result_state) dups=$(sysctl -n kern.kwq_test.result_dups) offcpu=$(sysctl -n kern.kwq_test.result_offcpu)" >&2
}
for p in 2 0 1; do
	$SU sysctl -q kern.kwq.bulk.place=$p >/dev/null
	run steal cpu=1 items=20000 cost_us=20 limit=100000
	run anyburst cpu=0 items=20000 cost_us=20 limit=100000
done
$SU sysctl -q kern.kwq.bulk.place=2 >/dev/null
run starved cpu=1 items=200 cost_us=100 spin_ms=100 limit=100000
$SU sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=1 kern.kwq_crypto.depth=16 kern.kwq_crypto.cpu=1 \
    kern.kwq_crypto.stealable=1 kern.kwq_crypto.secs=2 kern.kwq_crypto.verify=1 kern.kwq_crypto.bytes=65536 >/dev/null
$SU sysctl -q kern.kwq_crypto.run=1 >/dev/null
while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.2; done
echo "crypto: $(dmesg | grep 'kwq_crypto: kwq nq' | tail -1 | sed 's/.*cpu 1: //; s/ driver.*//')" >&2
