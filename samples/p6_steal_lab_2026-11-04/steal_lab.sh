#!/bin/sh
# steal_lab.sh - 2026-11-04, crest's question: why kwq-geli falls behind
# stock GELI past two providers per CPU (BULK.md S7).  128 kwq-geli
# providers on swap md(4) devices of 32 MiB, geli_bench scale's two loads
# (1 MiB sequential writes, 4 KiB random reads, one request in flight per
# provider), with one BULK knob changed at a time: stealing off
# (kern.kwq.bulk.steal=0), a steal attempt probing 2 CPUs instead of 16
# (steal_scan=2), the share limit off (cap_permille=1000).  Then 16 of
# the 128 busy, stealing on and off, and a kernel profile of each.  As root
# on lab, under sh:
#
#   sh steal_lab.sh DIR          # DIR: where bulk_deploy.sh put src/ and stage/
#
# Every md, module and knob is logged to the settings log with its undo.
D=${1:?DIR}
L=/home/crest/if_pair-scratch/settings_lab.log; O=$D/out/steal; S=$D/stage
GK="env GEOM_LIBRARY_PATH=$S geom kwqeli"
mkdir -p "$O"; rm -f "$D/steal.done" "$O/steal.txt" "$O"/prof_*.txt
note() { echo "$(date -u +%FT%TZ) steal: $*" >> "$L"; }
note "start, uptime $(uptime)"
for m in kwq g_kwqeli dtraceall; do
	kldstat -q -m $m && { note "$m loaded before the run: abort"; exit 1; }
done
[ -z "$(mdconfig -l)" ] || { note "md devices exist before the run: abort"; exit 1; }
cc -O2 -o "$O/iolat" "$D/src/kwq-geli/tests/iolat.c" -lpthread -lm || exit 1
MDS=""; KNOBS=""; DT=0
cleanup() {
	trap - EXIT INT TERM HUP
	for m in $MDS; do $GK detach -f $m.kweli 2>/dev/null; done
	[ -n "$KNOBS" ] && sysctl $KNOBS > /dev/null && note "restored $KNOBS"
	kldstat -q -m g_kwqeli && kldunload geom_kwqeli && note "unloaded geom_kwqeli"
	kldstat -q -m kwq && kldunload kwq && note "unloaded kwq"
	for m in $MDS; do mdconfig -d -u $m && note "destroyed $m"; done
	if [ $DT = 1 ]; then
		kldunload dtraceall
		for k in fbt sdt profile systrace systrace_freebsd32 dtnfscl dtmalloc fasttrap dtrace_test dtaudit dtrace opensolaris; do
			kldunload $k 2> /dev/null
		done
		note "unloaded dtraceall and its modules"
	fi
	note "end, modules $(kldstat | grep -cE 'kwq|dtrace'), md $(mdconfig -l | wc -w), uptime $(uptime)"
	touch "$D/steal.done"
}
trap cleanup EXIT INT TERM HUP

for i in $(jot 128); do
	m=$(mdconfig -a -t swap -s 32m) || exit 1
	MDS="$MDS $m"; note "created $m (swap 32m)"
	dd if=/dev/zero of=/dev/$m bs=1m count=32 2> /dev/null || exit 1
done
kldload "$S/kwq.ko" && note "loaded kwq.ko" || exit 1
kldload "$S/geom_kwqeli.ko" && note "loaded geom_kwqeli.ko" || exit 1
for k in steal steal_scan cap_permille; do
	KNOBS="$KNOBS kern.kwq.bulk.$k=$(sysctl -n kern.kwq.bulk.$k)"
done
note "knobs before: $KNOBS"
ALL=""
for m in $MDS; do
	$GK onetime -e AES-XTS -l 256 -s 4096 $m || exit 1
	ALL="$ALL /dev/$m.kweli"
done
SIXTEEN=$(echo $ALL | tr ' ' '\n' | head -n 16 | tr '\n' ' ')

arm() {	# ARM: the knobs of that arm, from the defaults
	sysctl -q $KNOBS > /dev/null
	case $1 in
	base) ;;
	nosteal) sysctl -q kern.kwq.bulk.steal=0 > /dev/null ;;
	scan2) sysctl -q kern.kwq.bulk.steal_scan=2 > /dev/null ;;
	nocap) sysctl -q kern.kwq.bulk.cap_permille=1000 > /dev/null ;;
	esac
	note "arm $1: $(sysctl kern.kwq.bulk.steal kern.kwq.bulk.steal_scan kern.kwq.bulk.cap_permille | tr '\n' ' ')"
}
cpt() { sysctl -n kern.cp_time | awk '{ print $3 + $4, $1 + $2 + $3 + $4 + $5 }'; }
ctr() {	# steal_tries steals busy_ns, summed over the BULK workers
	sysctl kern.kwq.bulk.cpu | awk -F': ' '/\.steal_tries:/ { t += $2 } /\.steals:/ { s += $2 } /\.busy_ns:/ { b += $2 } END { printf("%d %d %d\n", t, s, b) }'
}
run() {	# ARM N LOAD IOLAT-OPTIONS DEV...
	a=$1; n=$2; ld=$3; o=$4; shift 4
	c0=$(cpt); k0=$(ctr); s0=$(sysctl -n vm.stats.sys.v_swtch)
	"$O/iolat" $o -t 8 "$@" > "$O/io" 2>&1 || { note "iolat failed: $(cat "$O/io")"; exit 1; }
	c1=$(cpt); k1=$(ctr); s1=$(sysctl -n vm.stats.sys.v_swtch)
	echo "arm=$a n=$n load=$ld $(cat "$O/io") $(echo "$c0 $c1 $k0 $k1 $s0 $s1" | awk '{
		printf("sys=%.4f steal_tries=%d steals=%d busy_ms=%d cs=%d", ($3 - $1) / ($4 - $2), $8 - $5, $9 - $6, ($10 - $7) / 1e6, $12 - $11) }')" >> "$O/steal.txt"
}
for rep in 1 2; do
	for a in base nosteal scan2 nocap; do
		arm $a
		run $a 128 small "-r -b 4096 -q 1" $ALL
		run $a 128 bulk "-w -b 1048576 -q 1" $ALL
	done
	for a in base nosteal; do
		arm $a
		run $a 16 small "-r -b 4096 -q 1" $SIXTEEN
		run $a 16 bulk "-w -b 1048576 -q 1" $SIXTEEN
	done
done

kldload dtraceall && DT=1 && note "loaded dtraceall" || exit 1
prof() {	# ARM LOAD IOLAT-OPTIONS: 5 s of kernel samples at 997 Hz, 128 busy
	arm $1
	"$O/iolat" $3 -t 8 $ALL > /dev/null 2>&1 &
	sleep 1.5
	dtrace -q -n 'profile-997 /arg0/ { @[func(arg0)] = count(); } tick-5s { trunc(@, 50); printa("%-50a %@8d\n", @); exit(0); }' > "$O/prof_$1_$2.txt" 2>&1
	wait
}
prof base small "-r -b 4096 -q 1"
prof nosteal small "-r -b 4096 -q 1"
prof base bulk "-w -b 1048576 -q 1"
prof nosteal bulk "-w -b 1048576 -q 1"
