#!/bin/sh
# guest_steps.sh (guest, root): BULK.md S7.6's two steps on the debug kernel
# before bare metal: kwq-geli's budget check between bios only
# (budget_sectors 0) and BULK below md(4) (kern.kwq.bulk.priority=55), one
# at a time, then both.  Per setup kwq-geli's suite, the kwq suite where the
# priority moved, geli_bench quickly (tput resp onecpu, scale 4 16, tput at
# 32 KiB sectors).  Stock GELI's stage runs first and alone, kwq unloaded.
G=/root/ifpair-scratch/stages/p6g9; M=$G/mods; L=/root/ifpair-scratch/settings_guest.log
cd $G || exit 1
kldstat -q -m kwq && { echo "kwq loaded already"; exit 1; }
uptime; dmesg -c >/dev/null
cc -O2 -o $G/gb/victim $G/gb/tests/victim.c -lm || exit 1
B=$G/gb/kwq-geli/tests/geli_bench.sh
gb() {	# TAG STAGE
	env MODDIR=$M VICTIM=$G/gb/victim OUT_TSV=$G/gb/tput_$1.tsv SETTINGS_LOG=$L REPS=1 SECS=4 PROBE_S=2 MB=64 MDSIZE=128 \
	    TESTS="tput resp onecpu" sh $B $2 > $G/gb/tput_$1.log 2>&1
	echo "$1 tput resp onecpu exit $?: $(wc -l < $G/gb/tput_$1.tsv) rows"
	env MODDIR=$M VICTIM=$G/gb/victim OUT_TSV=$G/gb/scale_$1.tsv SETTINGS_LOG=$L REPS=1 PROBE_S=2 IDLE_S=1 \
	    SCALE_NS="4 16" SCALE_MDSIZE=32 TESTS=scale sh $B $2 > $G/gb/scale_$1.log 2>&1
	echo "$1 scale exit $?: $(wc -l < $G/gb/scale_$1.tsv) rows"
	env MODDIR=$M VICTIM=$G/gb/victim OUT_TSV=$G/gb/tput32k_$1.tsv SETTINGS_LOG=$L REPS=1 SECS=4 MB=64 MDSIZE=128 \
	    ELI_SECSIZE=32768 TESTS=tput sh $B $2 > $G/gb/tput32k_$1.log 2>&1
	echo "$1 tput 32 KiB sectors exit $?: $(wc -l < $G/gb/tput32k_$1.tsv) rows"
	grep -h "	tput	\(write_mbps\|read_mbps\|kwq_requeued\|kwq_overruns\)	" $G/gb/tput_$1.tsv
	echo "$1 console: $(dmesg | egrep -ic 'witness|lock order|panic|assert|leaked'), modules $(kldstat | grep -ciE 'kwq|kwqeli|nop'), md $(mdconfig -l | wc -w)"
}
suite() {	# TAG PROGRAM-DIR VAR: one suite, what failed
	kyua -v $3=$M test -k $2/Kyuafile > $G/kyua_$1.out 2>&1
	echo "$1: $(tail -1 $G/kyua_$1.out)"
	kyua report --results-filter=failed,broken 2>/dev/null | egrep -- "->" | head
}
cfg() {	# TAG BUDGET-SECTORS PRIORITY STAGE
	[ $2 = default ] || kenv kern.geom.kwqeli.budget_sectors=$2 >/dev/null
	[ $3 = default ] || kenv kern.kwq.bulk.priority=$3 >/dev/null
	kldload $M/kwq.ko && kldload $M/geom_kwqeli.ko || exit 1
	echo "== $1: priority $(sysctl -n kern.kwq.bulk.priority) budget_sectors $(sysctl -n kern.geom.kwqeli.budget_sectors) cap_prio $(sysctl -n kern.kwq.bulk.cap_prio)"
	kldunload geom_kwqeli; kldunload kwq
	[ $3 = default ] || suite kwq_$1 $G/kwq-tests test_suites.kwq.moddir
	kldload $M/kwq.ko || exit 1
	suite geli_$1 $G/geli-tests test_suites.kwqeli.moddir
	kldunload kwq
	# the tunable would pin kwqshard's budget_sectors; kwqbio sets the knob itself
	kenv -u kern.geom.kwqeli.budget_sectors >/dev/null 2>&1
	kldload $M/kwq.ko || exit 1
	gb $1 $4
	kldunload kwq
	[ $3 = default ] || kenv -u kern.kwq.bulk.priority >/dev/null
}
gb eli eli
cfg base default default kwqshard
cfg bio 0 default kwqbio
cfg prio default 55 kwqshard
cfg both 0 55 kwqbio
tail -4 $L
echo "modules: $(kldstat | grep -ciE 'kwq|kwqeli|nop'), md: $(mdconfig -l | wc -w), kenv: $(kenv | grep -c 'kern.kwq\|kern.geom.kwqeli')"
dmesg | egrep -i "witness|lock order|panic|assert|leaked" | head
grep -c "leaked memory" /var/log/messages
uptime
