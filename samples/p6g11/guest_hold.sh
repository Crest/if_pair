#!/bin/sh
# guest_hold.sh (guest, root): write_hold on the debug kernel.  Forced detach
# with writers and readers running, so that a write may be held at teardown;
# then kwqhold on two dummy drives, reporting the holds.
G=/root/ifpair-scratch/stages/p6g11; M=$G/mods
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
kldload $M/kwq.ko && kldload $M/geom_kwqeli.ko || exit 1
sysctl kern.geom.kwqeli.write_hold=1 >/dev/null
GK="env GEOM_LIBRARY_PATH=$M geom kwqeli"
dd if=/dev/random of=key bs=64 count=1 2>/dev/null
h0=$(sysctl -n kern.geom.kwqeli.write_holds)
n=0
while [ $n -lt 20 ]; do
	m=$(mdconfig -a -t swap -s 64m)
	geli init -B none -P -K key -s 4096 "$m" >/dev/null || exit 1
	$GK attach -p -k key "$m" || exit 1
	for i in 0 1 2 3 4 5 6 7; do
		dd if=/dev/zero of="/dev/$m.kweli" bs=128k count=2000 oseek=$((i * 32)) 2>/dev/null &
	done
	dd if="/dev/$m.kweli" of=/dev/null bs=128k count=2000 2>/dev/null &
	dd if="/dev/$m.kweli" of=/dev/null bs=128k count=2000 2>/dev/null &
	sleep 0.$((n % 5 + 1))
	$GK detach -f "$m.kweli" 2>/dev/null
	wait
	# the forced detach finishes when the last user closes
	k=0; while [ -e "/dev/$m.kweli" ] && [ $k -lt 50 ]; do sleep 0.1; k=$((k + 1)); done
	[ -e "/dev/$m.kweli" ] && { echo "run $n: provider left behind"; exit 1; }
	# GEOM retastes md after the last close
	k=0; until mdconfig -d -u "$m" 2>/dev/null; do
		k=$((k + 1)); [ $k -lt 100 ] || { echo "run $n: $m stays busy"; exit 1; }
		sleep 0.1
	done
	n=$((n + 1))
done
h1=$(sysctl -n kern.geom.kwqeli.write_holds)
echo "forced detach: $n runs, $((h1 - h0)) writes held, none left"
kldunload geom_kwqeli && kldunload kwq || exit 1
rm -f key
kldload $M/kwq.ko || exit 1
env MODDIR=$M VICTIM=$G/gb/victim SETTINGS_LOG=/root/ifpair-scratch/settings_guest.log REPS=1 MD_TYPE=sim PROBE_S=2 IDLE_S=1 SECS=4 OUT_TSV=$G/hold.tsv \
    TESTS=dev DEV_N=2 DEV_FAST=0 DEV_W=450 DEV_R=520 DEV_LAT=100 DEV_QD=4 sh $G/gb/kwq-geli/tests/geli_bench.sh kwqdef > $G/hold.log 2>&1
echo "bench exit $?"
egrep 'both_(w|r)_mbps|both_write_holds' $G/hold.tsv
kldunload kwq
echo "console: $(dmesg | egrep -ic 'witness|lock order|panic|assert|leaked'), modules $(kldstat | grep -ciE 'kwq|kwqeli|nop|simdisk'), md $(mdconfig -l | wc -w)"
uptime
