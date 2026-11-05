#!/bin/sh
# probes_guest.sh (guest, root): probes.d over probes_load.sh with the stage's modules
S=/root/ifpair-scratch/stages/p6g2/${1:-pgq}; B=/root/ifpair-scratch/stages/p6g4
kldstat -q -m kwq && { echo "kwq loaded"; exit 1; }
dl=0; kldstat -q -m dtraceall || { kldload dtraceall && dl=1; }
kldload $S/kwq/kwq.ko && kldload $S/kwq_test/kwq_test.ko && kldload $S/kwq_crypto/kwq_crypto.ko || exit 1
echo "defaults: place=$(sysctl -n kern.kwq.bulk.place) directed=$(sysctl -n kern.kwq.bulk.shard.directed) directed_us=$(sysctl -n kern.kwq.bulk.shard.directed_us)"
dmesg -c >/dev/null
dtrace -L $S -q -s $B/probes.d -c "sh $B/probes_load.sh" 2>&1
kldunload kwq_crypto; kldunload kwq_test; kldunload kwq
[ $dl = 1 ] && kldunload dtraceall
echo "modules: $(kldstat | grep -ci kwq) dtrace loaded by us: $dl"; dmesg | egrep -i "witness|lock order|panic|assert" | head -3
