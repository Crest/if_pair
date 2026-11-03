#!/bin/sh
# geli_bench.sh - kwq-geli against stock GELI on md(4) providers: what the
# crypto delivers, what it costs, and what it does to everyone else on the
# machine (../../kwq/PLAN.txt P6f).  As root:
#
#   env MODDIR=DIR OUT_TSV=FILE sh geli_bench.sh [STAGE ...]
#
# MODDIR holds kwq.ko, geom_kwqeli.ko and geom_kwqeli.so; VICTIM is the
# probe binary (default: built from ../../tests/victim.c into WORK).
# Stages (default: eli kwq):
#   eli         stock GELI, its default: a worker thread per CPU per
#               provider, bound, at PUSER
#   kwq         kwq-geli defaults: KWQ_CPU_ANY, stealable queues
#   kwqlocal    kwq-geli, every bio on the caller's CPU (cpu=-2), stealable
#   kwqnosteal  kwq-geli, KWQ_CPU_ANY, not stealable
# Tests (TESTS, default "tput resp onecpu"):
#   tput    P providers (default ncpu), one writer each, then one reader
#           each, MB per writer: MB/s in total and per provider, the
#           machine's sys+intr share
#   resp    the P writers run for the probes' length while victim probes
#           measure the rest of the machine: a 1 kHz timer's lateness and
#           a CPU-bound loop's share, unpinned and pinned to PCPU, and
#           fork+exec latency; stage "idle" is the probes on a quiet
#           machine, once per repetition
#   onecpu  ONECPU_N providers whose crypto all runs on CPU PCPU, written
#           at once for SECS: per-provider MB/s and Jain's index, with the
#           timer and then the spin probe pinned to PCPU.  kwq-geli queues
#           every bio to PCPU, not stealable; stock GELI gets one worker
#           per provider (kern.geom.eli.threads=1) with its affinity set
#           to PCPU by cpuset(1)
# Knobs: REPS (3), SECS (10), MB (256, at most MDSIZE), MDSIZE (MB of each
# md, 512; swap-backed, so only what is written takes memory, unwired),
# PASS_MB (32: the load's writers rewrite this much per dd), P, PCPU (1),
# ONECPU_N (4), OUT_TSV, SETTINGS_LOG, WORK.
# Rows: stage rep test metric value, tab-separated (as tests/compare.sh).
# Every md device, provider, module and knob this script creates, loads
# or sets is removed, unloaded or put back at exit.

set -u
[ "$(id -u)" -eq 0 ] || { echo "geli_bench.sh: needs root" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MODDIR:?MODDIR: the directory with kwq.ko, geom_kwqeli.ko, geom_kwqeli.so}"
: "${OUT_TSV:=geli_bench.tsv}"
: "${REPS:=3}" "${SECS:=10}" "${MB:=256}" "${MDSIZE:=512}"
: "${PCPU:=1}" "${ONECPU_N:=4}" "${TESTS:=tput resp onecpu}" "${PASS_MB:=32}"
: "${WORK:=/tmp/geli_bench.$$}"
: "${SETTINGS_LOG:=/dev/null}"
NCPU=$(sysctl -n hw.ncpu)
: "${P:=$NCPU}"
STAGES=${*:-eli kwq}
GK="env GEOM_LIBRARY_PATH=$MODDIR geom kwqeli"

mkdir -p "$WORK" || exit 1
: > "$WORK/mds"; : > "$WORK/knobs"; : > "$WORK/loaded"
note() { echo "$(date -u +%FT%TZ) geli_bench: $*" >> "$SETTINGS_LOG"; }
log() { echo "$(date +%T) $*"; }
row() { printf '%s\t%s\t%s\t%s\t%s\n' "$STAGE" "$REP" "$1" "$2" "$3" >> "$OUT_TSV"; }
knob() {	# NAME VALUE: set, remembering the old value once
	grep -q "^$1=" "$WORK/knobs" || echo "$1=$(sysctl -n "$1")" >> "$WORK/knobs"
	sysctl -q "$1=$2" >/dev/null || { echo "sysctl $1=$2 failed" >&2; exit 1; }
	note "set $1=$2"
}
load() {	# FILE-or-NAME MODULE
	kldstat -q -m "$2" && return 0
	kldload "$1" || { echo "cannot load $1" >&2; exit 1; }
	basename "$1" .ko >> "$WORK/loaded"
	note "loaded $1"
}
detach_all() {
	for m in $(cat "$WORK/mds"); do
		$GK detach -f "$m.kweli" 2>/dev/null
		geli detach -f "$m.eli" 2>/dev/null
	done
	sleep 0.5
}
cleanup() {
	trap - EXIT INT TERM HUP
	touch "$WORK/stop"; wait
	detach_all
	for m in $(cat "$WORK/mds"); do mdconfig -d -u "$m" 2>/dev/null && note "destroyed $m"; done
	[ -s "$WORK/knobs" ] && sysctl -q $(cat "$WORK/knobs") >/dev/null && note "restored $(tr '\n' ' ' < "$WORK/knobs")"
	for f in $(tail -r "$WORK/loaded"); do
		kldunload "$f" && note "unloaded $f"
	done
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM HUP

VICTIM=${VICTIM:-}
if [ -z "$VICTIM" ]; then
	cc -O2 -o "$WORK/victim" "$HERE/../../tests/victim.c" -lm || exit 1
	VICTIM=$WORK/victim
fi

# P md devices for tput/resp, ONECPU_N more for onecpu
NMD=$P
[ "$ONECPU_N" -gt "$NMD" ] && NMD=$ONECPU_N
i=0
while [ $i -lt "$NMD" ]; do
	m=$(mdconfig -a -t swap -s "${MDSIZE}m") || exit 1
	echo "$m" >> "$WORK/mds"; note "created $m (swap ${MDSIZE}m)"
	i=$((i + 1))
done
MDS=$(cat "$WORK/mds")
first() { echo "$MDS" | head -n "$1" | tr '\n' ' '; }

cp_sys() {	# kern.cp_time summed over CPUs: "sys+intr total"
	sysctl -n kern.cp_time | awk '{ s = $3 + $4; t = $1 + $2 + $3 + $4 + $5; print s, t }'
}
share() { awk -v a="$1" -v b="$2" 'BEGIN { split(a, x, " "); split(b, y, " "); d = y[2] - x[2]; printf("%.4f", d > 0 ? (y[1] - x[1]) / d : 0) }'; }
now_ns() { date +%s%N; }

# attach N providers of the stage: prints their device names
attach() {	# N
	devs=""
	for m in $(first "$1"); do
		case $STAGE in
		eli) geli onetime -e AES-XTS -l 256 -s 4096 "$m" || exit 1; devs="$devs $m.eli" ;;
		*) $GK onetime -e AES-XTS -l 256 -s 4096 "$m" || exit 1; devs="$devs $m.kweli" ;;
		esac
	done
	echo "$devs"
}

stage_setup() {	# the stage's modules and knobs
	case $STAGE in
	eli) load geom_eli g_eli ;;
	*) load "$MODDIR/kwq.ko" kwq; load "$MODDIR/geom_kwqeli.ko" g_kwqeli ;;
	esac
	case $STAGE in
	kwq) knob kern.geom.kwqeli.cpu -1; knob kern.geom.kwqeli.stealable 1 ;;
	kwqlocal) knob kern.geom.kwqeli.cpu -2; knob kern.geom.kwqeli.stealable 1 ;;
	kwqnosteal) knob kern.geom.kwqeli.cpu -1; knob kern.geom.kwqeli.stealable 0 ;;
	esac
}

kwq_rows() {	# TEST before-file after-file: kwq's BULK counters over the test
	[ "$STAGE" = eli ] && return 0
	paste "$2" "$3" | awk 'BEGIN { split("kwq_steals kwq_cap_demotions kwq_passes", nm, " ") }
	    { print nm[NR], $2 - $1 }' |
	while read -r k v; do row "$1" "$k" "$v"; done
}
kwq_snap() {	# steals, cap_demotions, passes summed over the BULK workers
	sysctl -a kern.kwq.bulk.cpu 2>/dev/null | awk -F': ' '
	    /\.steals: / { s += $2 } /\.cap_demotions: / { c += $2 } /\.passes: / { p += $2 }
	    END { print s + 0; print c + 0; print p + 0 }'
}

# The load: rewrite PASS_MB of the provider until $WORK/stop appears;
# dd's reports of the passes go to FILE.
writer() {	# DEV FILE
	while [ ! -f "$WORK/stop" ]; do
		dd if=/dev/zero of="/dev/$1" bs=1m count="$PASS_MB" 2>> "$2" || break
	done
}
load_start() {	# DEV...
	rm -f "$WORK/stop"
	for d in "$@"; do : > "$WORK/l.$d"; writer "$d" "$WORK/l.$d" & done
}
load_stop() { touch "$WORK/stop"; wait; }
dd_rate() {	# dd's reports -> "bytes secs", summed over the passes
	awk '/bytes transferred/ { b += $1; t += $5 } END { print b + 0, t + 0 }' "$1"
}
rates() {	# PREFIX DEV...: total, slowest and fastest provider's MB/s, Jain
	p=$1; shift
	for d in "$@"; do dd_rate "$WORK/l.$d"; done |
	    awk -v p="$p" '$2 > 0 { r = $1 / $2 / 1e6; s += r; ss += r * r; n++; if (min == "" || r < min) min = r; if (r > max) max = r }
	    END { printf("%s_mbps %.1f\n%s_prov_min %.1f\n%s_prov_max %.1f\n%s_jain %.4f\n", p, s, p, min, p, max, p, ss > 0 ? s * s / (n * ss) : 0) }'
}

t_tput() {
	devs=$(attach "$P")
	c0=$(cp_sys); kwq_snap > "$WORK/k0"; t0=$(now_ns)
	for d in $devs; do dd if=/dev/zero of="/dev/$d" bs=1m count="$MB" 2> "$WORK/l.$d" & done
	wait
	t1=$(now_ns); c1=$(cp_sys); kwq_snap > "$WORK/k1"
	row tput write_mbps "$(awk -v n="$P" -v mb="$MB" -v dt="$((t1 - t0))" 'BEGIN { printf("%.1f", n * mb * 1048576 / 1e6 / (dt / 1e9)) }')"
	row tput write_sys "$(share "$c0" "$c1")"
	rates write $devs | grep -v "^write_mbps" | while read -r k v; do row tput "$k" "$v"; done
	kwq_rows tput "$WORK/k0" "$WORK/k1"
	c0=$(cp_sys); t0=$(now_ns)
	for d in $devs; do dd if="/dev/$d" of=/dev/null bs=1m count="$MB" 2>/dev/null & done
	wait
	t1=$(now_ns); c1=$(cp_sys)
	row tput read_mbps "$(awk -v n="$P" -v mb="$MB" -v dt="$((t1 - t0))" 'BEGIN { printf("%.1f", n * mb * 1048576 / 1e6 / (dt / 1e9)) }')"
	row tput read_sys "$(share "$c0" "$c1")"
	detach_all
}

probes() {	# TEST: the victim probes, rows prefixed by what was measured
	ps=$(( SECS / 2 )); [ $ps -lt 2 ] && ps=2
	"$VICTIM" timer "$ps" > "$WORK/p" && probe_rows "$1" timer
	cpuset -l "$PCPU" "$VICTIM" timer "$ps" > "$WORK/p" && probe_rows "$1" timer_pin
	"$VICTIM" spin "$ps" > "$WORK/p" && probe_rows "$1" spin
	cpuset -l "$PCPU" "$VICTIM" spin "$ps" > "$WORK/p" && probe_rows "$1" spin_pin
	"$VICTIM" exec 100 10 > "$WORK/p" && probe_rows "$1" exec
}
probe_rows() {	# TEST PREFIX: key=value pairs of the probe's line
	tr ' ' '\n' < "$WORK/p" | awk -F= -v p="$2" 'NF == 2 && $1 != "n" && $1 != "wall" && $1 != "cpu" { print p "_" $1, $2 }' |
	while read -r k v; do row "$1" "$k" "$v"; done
}

t_resp() {
	devs=$(attach "$P")
	load_start $devs
	sleep 1
	c0=$(cp_sys); kwq_snap > "$WORK/k0"
	probes resp
	c1=$(cp_sys); kwq_snap > "$WORK/k1"
	load_stop
	row resp load_sys "$(share "$c0" "$c1")"
	rates load $devs | while read -r k v; do row resp "$k" "$v"; done
	kwq_rows resp "$WORK/k0" "$WORK/k1"
	detach_all
}

t_onecpu() {
	case $STAGE in
	eli) knob kern.geom.eli.threads 1 ;;
	*) knob kern.geom.kwqeli.cpu "$PCPU"; knob kern.geom.kwqeli.stealable 0 ;;
	esac
	devs=$(attach "$ONECPU_N")
	if [ "$STAGE" = eli ]; then
		# GELI's workers are kernel processes "g_eli[N] <provider>":
		# pgrep needs -S to see them; one each with threads=1
		for d in $devs; do
			for pid in $(pgrep -S "^g_eli\[[0-9]+\] ${d%.eli}\$"); do
				cpuset -l "$PCPU" -p "$pid" || exit 1
			done
		done
		row onecpu eli_workers "$(pgrep -S "^g_eli\[" | wc -l | tr -d ' ')"
	fi
	for what in timer spin; do
		load_start $devs
		sleep 1
		cpuset -l "$PCPU" "$VICTIM" "$what" $(( SECS - 2 )) > "$WORK/p" && probe_rows onecpu "${what}_pin"
		load_stop
		rates "$what" $devs | while read -r k v; do row onecpu "$k" "$v"; done
	done
	detach_all
	case $STAGE in
	eli) sysctl -q kern.geom.eli.threads=$(awk -F= '/^kern.geom.eli.threads=/ { print $2 }' "$WORK/knobs") >/dev/null ;;
	esac
}

log "stages: $STAGES; tests: $TESTS; P $P, ONECPU_N $ONECPU_N on CPU $PCPU, ${MB} MB/writer, SECS $SECS, REPS $REPS"
REP=1
while [ $REP -le "$REPS" ]; do
	case " $TESTS " in *" resp "*) STAGE=idle; log "rep $REP idle probes"; probes resp ;; esac
	for STAGE in $STAGES; do
		stage_setup
		for t in $TESTS; do
			log "rep $REP $STAGE $t"
			"t_$t"
		done
	done
	REP=$((REP + 1))
done
log "done: $OUT_TSV"
