#!/bin/sh
# guest_quiet.sh (guest, root): the slow crypto stand-in without probes
# (IO_QUIET): under stock the timer probe outlasts the load, so the spinner
# runs after it, while under kwq the spinner takes its share of the load.
# Three repetitions, stages rotated.  Direction only.
G=/root/ifpair-scratch/stages/p6g11; M=$G/mods; L=/root/ifpair-scratch/settings_guest.log
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
B=$G/gb/kwq-geli/tests/geli_bench.sh; O=$G/out/quiet; mkdir -p $O
E="MODDIR=$M VICTIM=$G/gb/victim SETTINGS_LOG=$L REPS=1 PROBE_S=2 IDLE_S=1 SECS=8 MD_TYPE=sim MB=64 MDSIZE=1024 TESTS=dev DEV_N=2 DEV_FAST=0 DEV_W=450 DEV_R=520 DEV_LAT=100 DEV_QD=4 IO_QUIET=1"
restore() { devctl enable aesni0 2>/dev/null; kldstat -q -m kwq && kldunload kwq; }
trap restore EXIT INT TERM HUP
rot() {
	n=$1; shift
	while [ "$n" -gt 0 ]; do w=$1; shift; set -- "$@" "$w"; n=$((n - 1)); done
	echo "$*"
}
devctl disable aesni0 && echo "aesni0 off"
r=1
while [ $r -le 3 ]; do
	env $E OUT_TSV=$O/stock_r$r.tsv sh $B eli > $O/stock_r$r.log 2>&1
	echo "r$r stock: exit $?, $(wc -l < $O/stock_r$r.tsv) rows"
	kldload $M/kwq.ko || exit 1
	st=$(rot $((r - 1)) kwqrq kwqout kwqnb)
	env $E OUT_TSV=$O/kwq_r$r.tsv sh $B $st > $O/kwq_r$r.log 2>&1
	echo "r$r kwq ($st): exit $?, $(wc -l < $O/kwq_r$r.tsv) rows"
	kldunload kwq
	r=$((r + 1))
done
restore
dmesg | egrep -i "witness|lock order|panic|assert|leaked" | head
echo "end: modules $(kldstat | grep -ciE 'kwq|kwqeli|nop|simdisk'), sd $(ls /dev/sd* 2>/dev/null | wc -w), aesni $(devinfo | grep -c aesni0)"
uptime
