#!/bin/sh
# guest_bench.sh (guest, root): the battery's tests quickly on the debug
# kernel before bare metal: every stage on dummy-data drives (md -t null),
# then the slow-crypto stand-in with aesni0 detached.  hz=100, so a gnop
# delay is 10 ms and rates are coarse: direction only.
G=/root/ifpair-scratch/stages/p6g11; M=$G/mods; L=/root/ifpair-scratch/settings_guest.log
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
[ -x $G/gb/victim ] || cc -O2 -o $G/gb/victim $G/gb/tests/victim.c -lm || exit 1
B=$G/gb/kwq-geli/tests/geli_bench.sh; O=$G/out; mkdir -p $O
E="MODDIR=$M VICTIM=$G/gb/victim SETTINGS_LOG=$L REPS=1 PROBE_S=2 IDLE_S=1 SECS=4 MD_TYPE=null MB=64 MDSIZE=1024"
gb() {	# TAG STAGES OPTS...
	t=$1; st=$2; shift 2
	env $E OUT_TSV=$O/$t.tsv "$@" sh $B $st > $O/$t.log 2>&1
	echo "$t ($st): exit $?, $(wc -l < $O/$t.tsv) rows, console $(dmesg | egrep -ic 'witness|lock order|panic|assert|leaked'), modules $(kldstat | grep -ciE 'kwq|kwqeli|nop'), md $(mdconfig -l | wc -w)"
}
run() {	# TAG-SUFFIX STAGES
	gb tput_$1 "$2" TESTS="tput resp"
	gb scale_$1 "$2" TESTS=scale SCALE_NS="4 16" SCALE_MDSIZE=1024
	gb dev_$1 "$2" TESTS=dev DEV_N=8 DEV_FAST=2 DEV_DELAY_MS=10 DEV_QD=2
}
run stock eli
kldload $M/kwq.ko || exit 1
run kwq "kwqshard kwqbio kwqtime"
kldunload kwq
gb dev_nop nop TESTS=dev DEV_N=8 DEV_FAST=2 DEV_DELAY_MS=10 DEV_QD=2
devctl disable aesni0 && echo "aesni0 off"
gb slow_stock eli TESTS=dev DEV_N=2 DEV_DELAY_MS=10 DEV_QD=2 DEV_BS=1048576
kldload $M/kwq.ko || { devctl enable aesni0; exit 1; }
gb slow_kwq "kwqshard kwqbio kwqtime" TESTS=dev DEV_N=2 DEV_DELAY_MS=10 DEV_QD=2 DEV_BS=1048576
kldunload kwq
devctl enable aesni0 && echo "aesni0 on"
dmesg | egrep -i "witness|lock order|panic|assert|leaked" | head
echo "end: modules $(kldstat | grep -ciE 'kwq|kwqeli|nop'), md $(mdconfig -l | wc -w), aesni $(devinfo | grep -c aesni0)"
uptime
