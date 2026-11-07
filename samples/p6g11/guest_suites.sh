#!/bin/sh
# guest_suites.sh (guest, root): kwq-geli's suite on the debug kernel with
# cut_us, then the kwq suite (the new KPI is in kwq.ko).
G=/root/ifpair-scratch/stages/p6g11; M=$G/mods
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
suite() {	# TAG DIR VAR
	kyua -v $3=$M test -k $2/Kyuafile > $G/kyua_$1.out 2>&1
	echo "$1: $(tail -1 $G/kyua_$1.out)"
	kyua report --results-filter=failed,broken 2>/dev/null | egrep -- "->" | head
}
suite kwq $G/kwq-tests test_suites.kwq.moddir
kldload $M/kwq.ko || exit 1
suite geli $G/geli-tests test_suites.kwqeli.moddir
kyua report --verbose --results-filter=passed 2>/dev/null | grep -A3 'cut_us' | head -8
kldunload kwq
echo "console: $(dmesg | egrep -ic 'witness|lock order|panic|assert|leaked'), modules $(kldstat | grep -ciE 'kwq|kwqeli|nop'), md $(mdconfig -l | wc -w)"
uptime
