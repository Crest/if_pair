#!/bin/sh
# geli_bench.sh - kwq-geli against stock GELI on md(4) providers: what the
# crypto delivers, what it costs, and what it does to everyone else on the
# machine (../../kwq/PLAN.txt P6f).  As root:
#
#   env MODDIR=DIR OUT_TSV=FILE sh geli_bench.sh [STAGE ...]
#
# MODDIR holds kwq.ko, geom_kwqeli.ko and geom_kwqeli.so; VICTIM is the
# probe binary (default: built from ../../tests/victim.c into WORK),
# IOLAT the load of scale and ssd (default: built from iolat.c).
# Stages (default: eli kwq):
#   eli         stock GELI, its default: a worker thread per CPU per
#               provider, bound, at PUSER
#   kwq         kwq-geli defaults: KWQ_CPU_ANY, stealable queues
#   kwqlocal    kwq-geli, every bio on the caller's CPU (cpu=-2), stealable
#   kwqnosteal  kwq-geli, KWQ_CPU_ANY, not stealable
#   kwqgossip   kwq, KWQ_CPU_ANY, stealable, BULK placement by gossip
#               (kern.kwq.bulk.place=1, ../../kwq/SPEC-p6g.md S2)
#   kwqshard    the same under P8, the sharded idle mask and the donor
#               line (kern.kwq.bulk.place=2); kwq, kwqlocal and
#               kwqnosteal run today's placement (place=0)
#   nop         no encryption: the load on the gnop providers themselves
#               (ssd only; the other tests skip it)
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
#   scale   what GELI's worker per CPU per provider costs the machine as
#           the providers grow, against kwq-geli's queue per provider:
#           for each N of SCALE_NS, N providers on md(4) devices of
#           SCALE_MDSIZE MB are attached (attach_ms, threads_added,
#           procs_added, malloc_kb_added: the GELI, crypto, kwq and
#           sysctl OID malloc types (kwq's queues have per-CPU sysctl
#           nodes), kmem_kb_added: those and a process, thread and
#           kernel stack at UMA's item sizes per worker added),
#           left idle for IDLE_S (idle_sys, idle_cs_s), the process
#           table read (ps_us: ps -axH, what top(1) and friends pay), then
#           loaded twice with iolat, one request in flight per provider:
#           bulk, 1 MiB sequential writes, and small, 4 KiB reads at
#           random offsets; and detached (detach_ms, until GELI's workers
#           have exited).  Per load: iolat's rates and latency
#           percentiles, the machine's sys+intr share, context switches
#           per second and per request, the crypto workers' voluntary
#           switches per request (wakeups_per_op: GELI's of these
#           providers, kwq's BULK workers), and the unpinned timer, spin
#           and exec probes beside it.  scale0 is the probes alone.  An N
#           whose GELI workers would pass 80 % of kern.maxproc is skipped
#   ssd     a performance model of SATA SSDs: every md(4) device of SSD_N
#           (SSD_MDSIZE MB) under a gnop(8) provider that holds a request
#           until a later clock tick (gnop create -d 1: the next tick, 0
#           to 1 ms at hz=1000) with probability PROB percent, and the
#           stage's provider on that.  At a fixed queue depth the delay
#           sets the rate, as a drive's service time does.  The defaults
#           aim at a SATA SSD's two datasheet figures: seqread and
#           seqwrite, 128 KiB requests, 4 in flight, each held a tick:
#           4 x 128 KiB per ms, 524 MB/s (SATA 3: 530 to 560); randread,
#           4 KiB at random offsets, 32 in flight, a third of them held:
#           about 3 requests per ms each, 96000/s (SATA 3 at QD32: 90000
#           to 98000).  The model has the rate and the mean latency of a
#           drive, not its latency distribution: a request is either held
#           to a tick or served at memory speed, and hz sets the time
#           scale (10 ms ticks at hz=100: a tenth of the rate).  Stage nop
#           measures the model itself.  Rows as for scale's loads, per
#           load of SSD_LOADS (seqread seqwrite randread; also randwrite)
# Knobs: REPS (3), SECS (10), MB (256, at most MDSIZE), MDSIZE (MB of each
# md, 512; swap-backed, so only what is written takes memory, unwired),
# PASS_MB (32: the load's writers rewrite this much per dd), P, PCPU (1),
# ONECPU_N (4), SCALE_NS ("1 2 4 8 16 32 64"), SCALE_MDSIZE (32), IDLE_S
# (3), PROBE_S (3: each probe of scale and ssd), SSD_N (4), SSD_MDSIZE
# (256), SSD_LOADS, SSD_SEQ_BS (131072), SSD_SEQ_QD (4), SSD_SEQ_PROB
# (100), SSD_RAND_QD (32), SSD_RAND_PROB (34), OUT_TSV, SETTINGS_LOG,
# WORK.  scale's and ssd's md devices are written whole once when made,
# so that reads find their pages.
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
: "${SCALE_NS:=1 2 4 8 16 32 64}" "${SCALE_MDSIZE:=32}" "${IDLE_S:=3}" "${PROBE_S:=3}"
: "${SSD_N:=4}" "${SSD_MDSIZE:=256}" "${SSD_LOADS:=seqread seqwrite randread}"
: "${SSD_SEQ_BS:=131072}" "${SSD_SEQ_QD:=4}" "${SSD_SEQ_PROB:=100}"
: "${SSD_RAND_QD:=32}" "${SSD_RAND_PROB:=34}"
STAGES=${*:-eli kwq}
GK="env GEOM_LIBRARY_PATH=$MODDIR geom kwqeli"

mkdir -p "$WORK" || exit 1
: > "$WORK/mds"; : > "$WORK/knobs"; : > "$WORK/loaded"; : > "$WORK/nops"
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
		grep -qx "$m" "$WORK/nops" || continue
		$GK detach -f "$m.nop.kweli" 2>/dev/null
		geli detach -f "$m.nop.eli" 2>/dev/null
	done
	wait_workers $(cat "$WORK/mds")
	sleep 0.5
}
# Stock GELI's workers ("g_eli[N] mdM", a kernel process each) exit
# after the detach returns, 64 providers x 128 CPUs on a07 taking
# seconds.  The next attach reuses the md names: a worker still
# exiting would be bound and counted with the new ones.  The name is
# cut at MAXCOMLEN (19), so "g_eli[127] md100.nop" ends in ".no".
wait_workers() {	# MD...: until no GELI worker of a provider on them is left
	[ $# -gt 0 ] || return 0
	re="^g_eli\[[0-9]+\] ($(echo "$@" | tr ' ' '|'))(\.|\$)"
	ww=0
	while pgrep -S "$re" >/dev/null && [ $ww -lt 600 ]; do
		sleep 0.1; ww=$((ww + 1))
	done
}
detach_devs() {	# DEV...: detach these providers and wait for their workers
	mds=""
	for d in "$@"; do
		case $d in
		*.eli) geli detach -f "$d" || echo "geli detach $d failed" >&2 ;;
		*.kweli) $GK detach -f "$d" || echo "kwqeli detach $d failed" >&2 ;;
		*) continue ;;
		esac
		mds="$mds ${d%%.*}"
	done
	wait_workers $mds
}
cleanup() {
	trap - EXIT INT TERM HUP
	touch "$WORK/stop"; wait
	detach_all
	for m in $(cat "$WORK/nops"); do gnop destroy -f "$m.nop" && note "destroyed $m.nop"; done
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
IOLAT=${IOLAT:-}
case " $TESTS " in *" scale "*|*" ssd "*)
	if [ -z "$IOLAT" ]; then
		cc -O2 -o "$WORK/iolat" "$HERE/iolat.c" -lpthread -lm || exit 1
		IOLAT=$WORK/iolat
	fi ;;
esac

# P md devices for tput/resp, ONECPU_N more for onecpu
NMD=0
case " $TESTS " in *" tput "*|*" resp "*|*" onecpu "*) NMD=$P ;; esac
case " $TESTS " in *" onecpu "*) [ "$ONECPU_N" -gt "$NMD" ] && NMD=$ONECPU_N ;; esac
i=0
while [ $i -lt "$NMD" ]; do
	m=$(mdconfig -a -t swap -s "${MDSIZE}m") || exit 1
	echo "$m" >> "$WORK/mds"; note "created $m (swap ${MDSIZE}m)"
	i=$((i + 1))
done
MDS=$(cat "$WORK/mds")
md_set() {	# FILE N MB: N more md devices of MB, written whole once
	j=0
	while [ $j -lt "$2" ]; do
		m=$(mdconfig -a -t swap -s "${3}m") || exit 1
		echo "$m" >> "$WORK/mds"; echo "$m" >> "$1"; note "created $m (swap ${3}m)"
		dd if=/dev/zero of="/dev/$m" bs=1m count="$3" 2>/dev/null || exit 1
		j=$((j + 1))
	done
}
: > "$WORK/mds_scale"; : > "$WORK/mds_ssd"
case " $TESTS " in *" scale "*)
	smax=0; for n in $SCALE_NS; do [ "$n" -gt $smax ] && smax=$n; done
	md_set "$WORK/mds_scale" $smax "$SCALE_MDSIZE" ;;
esac
case " $TESTS " in *" ssd "*)
	md_set "$WORK/mds_ssd" "$SSD_N" "$SSD_MDSIZE"
	load geom_nop g_nop
	for m in $(cat "$WORK/mds_ssd"); do
		gnop create -d 1 -q "$SSD_SEQ_PROB" -x "$SSD_SEQ_PROB" "$m" || exit 1
		echo "$m" >> "$WORK/nops"; note "created $m.nop (gnop -d 1)"
	done ;;
esac
first() { echo "$MDS" | head -n "$1" | tr '\n' ' '; }

cp_sys() {	# kern.cp_time summed over CPUs: "sys+intr total"
	sysctl -n kern.cp_time | awk '{ s = $3 + $4; t = $1 + $2 + $3 + $4 + $5; print s, t }'
}
share() { awk -v a="$1" -v b="$2" 'BEGIN { split(a, x, " "); split(b, y, " "); d = y[2] - x[2]; printf("%.4f", d > 0 ? (y[1] - x[1]) / d : 0) }'; }
now_ns() { date +%s%N; }
swtch() { sysctl -n vm.stats.sys.v_swtch; }
cs_rate() { awk -v a="$1" -v b="$2" -v dt="$3" 'BEGIN { printf("%.0f", dt > 0 ? (b - a) / (dt / 1e9) : 0) }'; }

# the stage's provider on each of PROVIDER...: prints their device names
attach_on() {	# PROVIDER...
	devs=""
	for m in "$@"; do
		case $STAGE in
		nop) devs="$devs $m" ;;
		eli) geli onetime -e AES-XTS -l 256 -s 4096 "$m" || exit 1; devs="$devs $m.eli" ;;
		*) $GK onetime -e AES-XTS -l 256 -s 4096 "$m" || exit 1; devs="$devs $m.kweli" ;;
		esac
	done
	echo "$devs"
}
attach() { attach_on $(first "$1"); }	# N: on the first N md devices

stage_setup() {	# the stage's modules and knobs
	case $STAGE in
	nop) ;;
	eli) load geom_eli g_eli ;;
	*) load "$MODDIR/kwq.ko" kwq; load "$MODDIR/geom_kwqeli.ko" g_kwqeli ;;
	esac
	case $STAGE in
	kwq) knob kern.geom.kwqeli.cpu -1; knob kern.geom.kwqeli.stealable 1 ;;
	kwqlocal) knob kern.geom.kwqeli.cpu -2; knob kern.geom.kwqeli.stealable 1 ;;
	kwqnosteal) knob kern.geom.kwqeli.cpu -1; knob kern.geom.kwqeli.stealable 0 ;;
	kwqgossip|kwqshard) knob kern.geom.kwqeli.cpu -1; knob kern.geom.kwqeli.stealable 1 ;;
	esac
	# BULK placement (kwq.ko of P6g.2 and later): stages on one boot each set theirs
	case $STAGE in
	nop|eli) ;;
	kwqgossip) knob kern.kwq.bulk.place 1 ;;
	kwqshard) knob kern.kwq.bulk.place 2 ;;
	*) sysctl -q kern.kwq.bulk.place >/dev/null 2>&1 && knob kern.kwq.bulk.place 0 ;;
	esac
}

kwq_rows() {	# TEST before-file after-file: kwq's BULK counters over the test
	[ "$STAGE" = eli ] && return 0
	paste "$2" "$3" | awk 'BEGIN { split("kwq_steals kwq_cap_demotions kwq_passes kwq_handoffs kwq_pulls", nm, " ") }
	    { print nm[NR], $2 - $1 }' |
	while read -r k v; do row "$1" "$k" "$v"; done
}
kwq_snap() {	# steals, cap_demotions, passes summed over the BULK workers
	sysctl -a kern.kwq.bulk.cpu 2>/dev/null | awk -F': ' '
	    /\.steals: / { s += $2 } /\.cap_demotions: / { c += $2 } /\.passes: / { p += $2 }
	    /\.handoffs: / { h += $2 } /\.pulls: / { u += $2 }
	    END { print s + 0; print c + 0; print p + 0; print h + 0; print u + 0 }'
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
	c0=$(cp_sys); kwq_snap > "$WORK/k0"; s0=$(swtch); t0=$(now_ns)
	for d in $devs; do dd if=/dev/zero of="/dev/$d" bs=1m count="$MB" 2> "$WORK/l.$d" & done
	wait
	t1=$(now_ns); s1=$(swtch); c1=$(cp_sys); kwq_snap > "$WORK/k1"
	row tput write_cs_s "$(cs_rate "$s0" "$s1" "$((t1 - t0))")"
	row tput write_mbps "$(awk -v n="$P" -v mb="$MB" -v dt="$((t1 - t0))" 'BEGIN { printf("%.1f", n * mb * 1048576 / 1e6 / (dt / 1e9)) }')"
	row tput write_sys "$(share "$c0" "$c1")"
	rates write $devs | grep -v "^write_mbps" | while read -r k v; do row tput "$k" "$v"; done
	kwq_rows tput "$WORK/k0" "$WORK/k1"
	c0=$(cp_sys); s0=$(swtch); t0=$(now_ns)
	for d in $devs; do dd if="/dev/$d" of=/dev/null bs=1m count="$MB" 2>/dev/null & done
	wait
	t1=$(now_ns); s1=$(swtch); c1=$(cp_sys)
	row tput read_cs_s "$(cs_rate "$s0" "$s1" "$((t1 - t0))")"
	row tput read_mbps "$(awk -v n="$P" -v mb="$MB" -v dt="$((t1 - t0))" 'BEGIN { printf("%.1f", n * mb * 1048576 / 1e6 / (dt / 1e9)) }')"
	row tput read_sys "$(share "$c0" "$c1")"
	detach_all
}

# One probe, timed from outside: a probe the load keeps from running
# reports nothing of the wait before its first instruction, so the
# start delay (the outside wall time beyond the probe's own) is a row of
# its own, and the spin share is also given over the outside wall time.
probe() {	# TEST PREFIX VICTIM-ARGS...
	t=$1; pfx=$2; shift 2
	p0=$(now_ns)
	"$@" > "$WORK/p" || return 0
	p1=$(now_ns)
	probe_rows "$t" "$pfx"
	tr ' ' '\n' < "$WORK/p" | awk -F= -v dt="$((p1 - p0))" -v pfx="$pfx" '
	    $1 == "wall" { w = $2 } $1 == "cpu" { c = $2 } $1 == "n" { n = $2 }
	    END { o = dt / 1e9
		if (w != "") { printf("%s_start_delay_ms %.0f\n%s_share_outside %.4f\n", pfx, (o - w) * 1000, pfx, o > 0 ? c / o : 0) }
		else if (n != "") printf("%s_elapsed_ms %.0f\n", pfx, o * 1000) }' |
	while read -r k v; do row "$t" "$k" "$v"; done
}
probes() {	# TEST: the victim probes, rows prefixed by what was measured
	ps=$(( SECS / 2 )); [ $ps -lt 2 ] && ps=2
	probe "$1" timer "$VICTIM" timer "$ps"
	probe "$1" timer_pin cpuset -l "$PCPU" "$VICTIM" timer "$ps"
	probe "$1" spin "$VICTIM" spin "$ps"
	probe "$1" spin_pin cpuset -l "$PCPU" "$VICTIM" spin "$ps"
	probe "$1" exec "$VICTIM" exec 100 10
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
		n=0
		for d in $devs; do
			n=$((n + $(pgrep -S "^g_eli\[[0-9]+\] ${d%.eli}\$" | wc -l)))
		done
		row onecpu eli_workers "$n"	# ours only, not the system's own
	fi
	# The probe first and the load half a second later, stopped after
	# the probe exits: a probe started under a load that holds the CPU
	# whole cannot run until the load ends (stock GELI's bound workers
	# can), and would then measure an idle CPU.
	for what in timer spin; do
		cpuset -l "$PCPU" "$VICTIM" "$what" $(( SECS - 2 )) > "$WORK/p" &
		vpid=$!
		sleep 0.5
		load_start $devs
		wait $vpid
		load_stop
		probe_rows onecpu "${what}_pin"
		[ $what = spin ] && row onecpu spin_pin_share_loaded "$(tr ' ' '\n' < "$WORK/p" | awk -F= '$1 == "wall" { w = $2 } $1 == "cpu" { c = $2 }
		    END { printf("%.4f", w > 0.5 ? (c - 0.5 < 0 ? 0 : c - 0.5) / (w - 0.5) : 0) }')"
		rates "$what" $devs | while read -r k v; do row onecpu "$k" "$v"; done
	done
	detach_all
	case $STAGE in
	eli) sysctl -q kern.geom.eli.threads=$(awk -F= '/^kern.geom.eli.threads=/ { print $2 }' "$WORK/knobs") >/dev/null ;;
	esac
}

# What the machine holds and does for the providers
nthreads() { ps -axH -o lwp= | wc -l | tr -d ' '; }
# A process freed to UMA keeps its thread and kernel stack, so the
# THREAD and kstack_cache zones' counts miss workers made from cached
# processes: a worker is priced at the zones' item sizes instead, from
# the PROC zone's count and the threads ps sees.
zitem() { vmstat -z | awk -F'[:,] *' -v z="$1" '$1 == z && !s++ { print $'"$2"' }'; }	# ZONE FIELD
procs_used() { zitem PROC 4; }
malloc_b() {	# bytes of the GELI, crypto, kwq and sysctl OID malloc types
	vmstat -m | awk '{ t = $1; for (i = 2; i <= NF - 4; i++) t = t " " $i }
	    NF >= 5 && t ~ /eli|crypto|aesni|ossl|armv8|kwq|sysctloid/ { b += $(NF - 2) } END { print b + 0 }'
}
wk_snap() {	# voluntary switches of the stage's crypto workers (ps prints
		# no tdname for a process of one thread, as GELI's are)
	case $STAGE in
	nop) echo 0 ;;
	eli) ps -axH -o nvcsw= -o comm= | awk '/ g_eli\[[0-9]+\] md[0-9]/ { s += $1 } END { print s + 0 }' ;;
	*) ps -axH -o nvcsw= -o tdname= | awk '$2 ~ /^kwq_bulk\// { s += $1 } END { print s + 0 }' ;;
	esac
}
ps_us() {	# the median of three reads of the whole process table
	for k in 1 2 3; do
		a=$(now_ns); ps -axH -o pid,lwp,state,nvcsw,tdname > /dev/null; b=$(now_ns)
		echo $(( (b - a) / 1000 ))
	done | sort -n | sed -n 2p
}
probes_sys() {	# TEST PREFIX: the unpinned probes: the machine as its users see it
	probe "$1" "$2_timer" "$VICTIM" timer "$PROBE_S"
	probe "$1" "$2_spin" "$VICTIM" spin "$PROBE_S"
	probe "$1" "$2_exec" "$VICTIM" exec 50 10
}
# iolat on DEV... with the probes beside it; rows TEST PREFIX_*.  Its
# names are its own: probe() sets t and pfx.
io_load() {	# TEST PREFIX IOLAT-OPTIONS DEV...
	lt=$1; lp=$2; lo=$3; shift 3
	w0=$(wk_snap); c0=$(cp_sys); s0=$(swtch); t0=$(now_ns)
	$IOLAT $lo -t $((2 * PROBE_S + 5)) $(for d in "$@"; do echo "/dev/$d"; done) > "$WORK/io" 2> "$WORK/io.err" &
	ipid=$!
	sleep 0.5
	probes_sys "$lt" "$lp"
	wait $ipid || { echo "$lt $lp: iolat failed: $(cat "$WORK/io.err")" >&2; exit 1; }
	t1=$(now_ns); s1=$(swtch); c1=$(cp_sys); w1=$(wk_snap)
	ops=$(tr ' ' '\n' < "$WORK/io" | awk -F= '$1 == "ops" { print $2 }')
	tr ' ' '\n' < "$WORK/io" | awk -F= -v p="$lp" 'NF == 2 && $1 != "ops" { print p "_" $1, $2 }' |
	while read -r k v; do row "$lt" "$k" "$v"; done
	row "$lt" "${lp}_sys" "$(share "$c0" "$c1")"
	row "$lt" "${lp}_cs_s" "$(cs_rate "$s0" "$s1" "$((t1 - t0))")"
	[ "${ops:-0}" -gt 0 ] || return 0
	row "$lt" "${lp}_cs_per_op" "$(awk -v a="$s0" -v b="$s1" -v n="$ops" 'BEGIN { printf("%.2f", (b - a) / n) }')"
	[ "$STAGE" = nop ] || row "$lt" "${lp}_wakeups_per_op" "$(awk -v a="$w0" -v b="$w1" -v n="$ops" 'BEGIN { printf("%.2f", (b - a) / n) }')"
}

t_scale() {
	ZPROC=$(zitem PROC 2); ZTHREAD=$(zitem THREAD 2); ZKSTACK=$(zitem kstack_cache 2)
	probes_sys scale0 idle
	for n in $SCALE_NS; do
		t=scale$n	# probe() sets t to the same
		if [ "$STAGE" = eli ]; then
			np=$(ps -ax -o pid= | wc -l); mp=$(sysctl -n kern.maxproc)
			if [ $((np + n * NCPU)) -gt $((mp * 8 / 10)) ]; then
				log "$t: $((n * NCPU)) GELI workers would pass 80 % of kern.maxproc $mp; skipped"
				row "$t" skipped_maxproc 1
				continue
			fi
		fi
		h0=$(nthreads); p0=$(procs_used); b0=$(malloc_b); a0=$(now_ns)
		devs=$(attach_on $(head -n "$n" "$WORK/mds_scale"))
		a1=$(now_ns); h1=$(nthreads); p1=$(procs_used); b1=$(malloc_b)
		[ "$(echo $devs | wc -w)" -eq "$n" ] || { echo "$t: attach failed" >&2; exit 1; }
		row "$t" attach_ms $(( (a1 - a0) / 1000000 ))
		row "$t" threads_added $((h1 - h0))
		row "$t" procs_added $((p1 - p0))
		row "$t" malloc_kb_added $(( (b1 - b0) / 1024 ))
		row "$t" kmem_kb_added $(( (b1 - b0 + (p1 - p0) * ZPROC + (h1 - h0) * (ZTHREAD + ZKSTACK)) / 1024 ))
		c0=$(cp_sys); s0=$(swtch); i0=$(now_ns)
		sleep "$IDLE_S"
		i1=$(now_ns); s1=$(swtch); c1=$(cp_sys)
		row "$t" idle_sys "$(share "$c0" "$c1")"
		row "$t" idle_cs_s "$(cs_rate "$s0" "$s1" "$((i1 - i0))")"
		row "$t" ps_us "$(ps_us)"
		io_load "$t" bulk "-w -b 1048576 -q 1" $devs
		io_load "$t" small "-r -b 4096 -q 1" $devs
		d0=$(now_ns)
		detach_devs $devs
		d1=$(now_ns)
		row "$t" detach_ms $(( (d1 - d0) / 1000000 ))
		sleep 0.5
	done
}

t_ssd() {
	devs=$(attach_on $(sed 's/$/.nop/' "$WORK/mds_ssd"))
	[ "$(echo $devs | wc -w)" -eq "$SSD_N" ] || { echo "ssd: attach failed" >&2; exit 1; }
	row ssd model_tick_us $(( 1000000 / $(sysctl -n kern.hz) ))
	for l in $SSD_LOADS; do
		case $l in
		seqread) p=$SSD_SEQ_PROB; o="-b $SSD_SEQ_BS -q $SSD_SEQ_QD" ;;
		seqwrite) p=$SSD_SEQ_PROB; o="-w -b $SSD_SEQ_BS -q $SSD_SEQ_QD" ;;
		randread) p=$SSD_RAND_PROB; o="-r -b 4096 -q $SSD_RAND_QD" ;;
		randwrite) p=$SSD_RAND_PROB; o="-w -r -b 4096 -q $SSD_RAND_QD" ;;
		*) echo "ssd: unknown load $l" >&2; exit 1 ;;
		esac
		for m in $(cat "$WORK/mds_ssd"); do
			gnop configure -q "$p" -x "$p" "$m.nop" || exit 1
		done
		io_load ssd "$l" "$o" $devs
	done
	detach_devs $devs
}

log "stages: $STAGES; tests: $TESTS; P $P, ONECPU_N $ONECPU_N on CPU $PCPU, ${MB} MB/writer, SECS $SECS, REPS $REPS"
REP=1
while [ $REP -le "$REPS" ]; do
	case " $TESTS " in *" resp "*) STAGE=idle; log "rep $REP idle probes"; probes resp ;; esac
	for STAGE in $STAGES; do
		stage_setup
		for t in $TESTS; do
			case $STAGE:$t in nop:ssd) ;; nop:*) continue ;; esac
			log "rep $REP $STAGE $t"
			"t_$t"
		done
	done
	REP=$((REP + 1))
done
log "done: $OUT_TSV"
