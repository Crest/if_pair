#!/bin/sh
# prof2_lab.sh - 2026-11-04: where kwq-geli's CPU per MiB goes under the
# bulk load (1 MiB sequential writes, one in flight per provider) on 128
# providers, with the share limit on (default) and off
# (kern.kwq.bulk.cap_permille=1000), against stock GELI on the same md(4)
# devices.  dtrace is started and its probes enabled before the load:
# under the load a starting dtrace waited out all of it (stall_lab.sh).
# Samples from 1.5 s to 6.5 s of each 8 s load.  As root on lab, under sh:
#
#   sh prof2_lab.sh DIR          # DIR: where bulk_deploy.sh put src/ and stage/
#
# Every md, module and knob is logged to the settings log with its undo.
D=${1:?DIR}
L=/home/crest/if_pair-scratch/settings_lab.log; O=$D/out/prof2; S=$D/stage
GK="env GEOM_LIBRARY_PATH=$S geom kwqeli"
mkdir -p "$O"; rm -f "$D/prof2.done" "$O/prof2.txt" "$O"/prof_*.txt
note() { echo "$(date -u +%FT%TZ) prof2: $*" >> "$L"; }
note "start, uptime $(uptime)"
for m in kwq g_kwqeli dtraceall; do
	kldstat -q -m $m && { note "$m loaded before the run: abort"; exit 1; }
done
[ -z "$(mdconfig -l)" ] || { note "md devices exist before the run: abort"; exit 1; }
cc -O2 -o "$O/iolat" "$D/src/kwq-geli/tests/iolat.c" -lpthread -lm || exit 1
MDS=""; KNOBS=""; DT=0
wait_workers() {	# until stock GELI's workers of these md devices have exited
	re="^g_eli\[[0-9]+\] ($(echo $MDS | tr ' ' '|'))(\.|\$)"
	ww=0
	while pgrep -S "$re" > /dev/null && [ $ww -lt 600 ]; do sleep 0.1; ww=$((ww + 1)); done
}
cleanup() {
	trap - EXIT INT TERM HUP
	for m in $MDS; do $GK detach -f $m.kweli 2>/dev/null; geli detach -f $m.eli 2>/dev/null; done; wait_workers
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
	touch "$D/prof2.done"
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

cpt() { sysctl -n kern.cp_time | awk '{ print $3 + $4, $1 + $2 + $3 + $4 + $5 }'; }
kldload dtraceall && DT=1 && note "loaded dtraceall" || exit 1
prof() {	# CASE DEV...: dtrace first, on the idle machine, then 8 s of the load
	c=$1; shift
	rm -f "$O/prof_$c.txt"
	# Samples from 4.5 s to 9.5 s after dtrace's BEGIN; the load runs from
	# 3 s to 11 s after dtrace is started, which takes well under 1.5 s idle.
	dtrace -q -n 'BEGIN { t0 = timestamp; }
	    profile-997 /arg0 && timestamp - t0 > 4500000000 && timestamp - t0 < 9500000000/ { @[func(arg0)] = count(); }
	    tick-100ms /timestamp - t0 > 9500000000/ { trunc(@, 60); printa("%-50a %@8d\n", @); exit(0); }' > "$O/prof_$c.txt" 2>&1 &
	dpid=$!
	sleep 3
	c0=$(cpt)
	"$O/iolat" -w -b 1048576 -q 1 -t 8 "$@" > "$O/io" 2>&1 || { note "iolat failed: $(cat "$O/io")"; exit 1; }
	c1=$(cpt)
	wait $dpid
	echo "case=$c $(cat "$O/io") sys=$(echo "$c0 $c1" | awk '{ printf("%.4f", ($3 - $1) / ($4 - $2)) }')" >> "$O/prof2.txt"
}
prof kwq_base $ALL
sysctl -q kern.kwq.bulk.cap_permille=1000 > /dev/null && note "set kern.kwq.bulk.cap_permille=1000"
prof kwq_nocap $ALL
sysctl -q $KNOBS > /dev/null && note "restored $KNOBS"
for m in $MDS; do $GK detach -f $m.kweli || exit 1; done
ALL=""
for m in $MDS; do
	geli onetime -e AES-XTS -l 256 -s 4096 $m || exit 1
	ALL="$ALL /dev/$m.eli"
done
note "attached stock GELI on the 128 md devices"
prof eli $ALL
for m in $MDS; do geli detach -f $m.eli; done
wait_workers
note "detached stock GELI, $(pgrep -S '^g_eli\[' | wc -l) workers left (swap's own included)"
