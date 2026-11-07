#!/bin/sh
# guest_sim.sh (guest, root): every candidate on geom_simdisk.ko on the debug
# kernel before bare metal: tput resp, scale, the storage model, and the slow
# crypto stand-in with aesni0 detached.  Direction only.
G=/root/ifpair-scratch/stages/p6g11; M=$G/mods; L=/root/ifpair-scratch/settings_guest.log
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
B=$G/gb/kwq-geli/tests/geli_bench.sh; O=$G/out; mkdir -p $O
E="MODDIR=$M VICTIM=$G/gb/victim SETTINGS_LOG=$L REPS=1 PROBE_S=2 IDLE_S=1 SECS=4 MD_TYPE=sim MB=64 MDSIZE=1024"
SYS="TESTS=dev DEV_N=26 DEV_FAST=2 DEV_W=200 DEV_R=200 DEV_LAT=100 DEV_SEEK=8000 DEV_QD=2"
gb() {	# TAG STAGES OPTS...
	t=$1; st=$2; shift 2
	env $E OUT_TSV=$O/$t.tsv "$@" sh $B $st > $O/$t.log 2>&1
	echo "$t ($st): exit $?, $(wc -l < $O/$t.tsv) rows, console $(dmesg | egrep -ic 'witness|lock order|panic|assert|leaked'), modules $(kldstat | grep -ciE 'kwq|kwqeli|nop|simdisk'), sd $(ls /dev/sd* 2>/dev/null | wc -w)"
}
run() {	# SUFFIX STAGES
	gb s_tput_$1 "$2" TESTS="tput resp"
	gb s_scale_$1 "$2" TESTS=scale SCALE_NS="4 16" SCALE_MDSIZE=1024
	gb s_dev_$1 "$2" $SYS
}
run stock eli
kldload $M/kwq.ko || exit 1
run kwq "kwqshard kwqtime kwqrq"
kldunload kwq
gb s_dev_nop nop $SYS
devctl disable aesni0 && echo "aesni0 off"
SLOW="TESTS=dev DEV_N=2 DEV_FAST=0 DEV_W=450 DEV_R=520 DEV_LAT=100 DEV_QD=4"
gb s_slow_stock eli $SLOW
kldload $M/kwq.ko || { devctl enable aesni0; exit 1; }
gb s_slow_kwq "kwqshard kwqtime kwqrq" $SLOW
kldunload kwq
devctl enable aesni0 && echo "aesni0 on"
dmesg | egrep -i "witness|lock order|panic|assert|leaked" | head
echo "end: modules $(kldstat | grep -ciE 'kwq|kwqeli|nop|simdisk'), sd $(ls /dev/sd* 2>/dev/null | wc -w), aesni $(devinfo | grep -c aesni0)"
uptime
