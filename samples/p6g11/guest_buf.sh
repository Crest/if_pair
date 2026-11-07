#!/bin/sh
# guest_buf.sh (guest, root): the storage model on the debug kernel: reads ahead,
# and kwq-geli as it loads.
G=/root/ifpair-scratch/stages/p6g11; M=$G/mods; L=/root/ifpair-scratch/settings_guest.log
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
B=$G/gb/kwq-geli/tests/geli_bench.sh; O=$G/out; mkdir -p $O
E="MODDIR=$M VICTIM=$G/gb/victim SETTINGS_LOG=$L REPS=1 PROBE_S=2 IDLE_S=1 SECS=4 MD_TYPE=sim MB=64 MDSIZE=1024"
SYS="TESTS=dev DEV_N=26 DEV_FAST=2 DEV_W=200 DEV_R=200 DEV_LAT=100 DEV_SEEK=8000 DEV_QD=2"
kldload $M/kwq.ko || exit 1
env $E OUT_TSV=$O/b_dev.tsv $SYS sh $B kwqrq kwqdef > $O/b_dev.log 2>&1
echo "b_dev: exit $?, $(wc -l < $O/b_dev.tsv) rows"
kldunload kwq
echo "console: $(dmesg | egrep -ic 'witness|lock order|panic|assert|leaked'), modules $(kldstat | grep -ciE 'kwq|kwqeli|nop|simdisk'), sd $(ls /dev/sd* 2>/dev/null | wc -w)"
uptime
