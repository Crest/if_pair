#!/bin/sh
# crypto_bench.sh - kwq_crypto.ko (the dummy BULK client, ../PLAN.txt P6d)
# against its thread baseline, GELI's own model: what BULK queues deliver
# and what several of them busy on one CPU leave for a user thread there.
# As root:
#
#   env MODDIR=DIR OUT_TSV=FILE sh crypto_bench.sh
#
# MODDIR holds kwq.ko and kwq_crypto.ko; VICTIM is tests/victim.c built
# (default: built from ../../tests/victim.c).  Rows: stage rep test
# metric value, tab-separated, as tests/compare.sh writes them.
#   all     one client queue, DEPTH_ALL requests in flight (default 4 x
#           ncpu): stage kwq submits with KWQ_CPU_ANY (stealable), stage
#           thread runs GELI's model, a PUSER thread per CPU bound to it
#   one<N>  N client queues (ONE_N, default "1 2 4 8") busy on CPU PCPU,
#           depth 16 each: stage kwq puts every queue there, not
#           stealable; stage thread binds one PUSER thread per queue
#           there; a victim timer and then a spin loop pinned to PCPU
#           run during the window
# Knobs: REPS (3), SECS (8), BYTES (65536), PCPU (2), DEPTH_ALL, ONE_N.
set -u
[ "$(id -u)" -eq 0 ] || { echo "crypto_bench.sh: needs root" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MODDIR:?MODDIR: the directory with kwq.ko and kwq_crypto.ko}"
: "${OUT_TSV:=crypto_bench.tsv}" "${REPS:=3}" "${SECS:=8}" "${BYTES:=65536}"
: "${PCPU:=2}" "${ONE_N:=1 2 4 8}" "${SETTINGS_LOG:=/dev/null}"
NCPU=$(sysctl -n hw.ncpu)
: "${DEPTH_ALL:=$((4 * NCPU))}"
WORK=$(mktemp -d /tmp/crypto_bench.XXXXXX) || exit 1
: > "$WORK/loaded"
note() { echo "$(date -u +%FT%TZ) crypto_bench: $*" >> "$SETTINGS_LOG"; }
row() { printf '%s\t%s\t%s\t%s\t%s\n' "$STAGE" "$REP" "$1" "$2" "$3" >> "$OUT_TSV"; }
cleanup() {
	trap - EXIT INT TERM HUP
	n=0
	while [ "$(sysctl -n kern.kwq_crypto.result_state 2>/dev/null)" = running ] && [ $n -lt 600 ]; do
		sleep 0.2; n=$((n + 1))
	done
	for f in $(tail -r "$WORK/loaded"); do kldunload "$f" && note "unloaded $f"; done
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM HUP
for m in kwq kwq_crypto; do
	kldstat -q -m $m && continue
	kldload "$MODDIR/$m.ko" || exit 1
	echo $m >> "$WORK/loaded"; note "loaded $MODDIR/$m.ko"
done
VICTIM=${VICTIM:-}
if [ -z "$VICTIM" ]; then
	cc -O2 -o "$WORK/victim" "$HERE/../../tests/victim.c" -lm || exit 1
	VICTIM=$WORK/victim
fi

kc_start() {	# knob=value ...
	for kv in "$@"; do sysctl -q "kern.kwq_crypto.$kv" >/dev/null || { echo "bad $kv" >&2; exit 1; }; done
	sysctl -q kern.kwq_crypto.run=1 >/dev/null || exit 1
}
kc_wait() {
	while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.2; done
	[ "$(sysctl -n kern.kwq_crypto.result_state)" = done ] || echo "kwq_crypto: $(sysctl -n kern.kwq_crypto.result_msg)" >&2
}
kc_rows() {	# TEST
	row "$1" mbps "$(sysctl -n kern.kwq_crypto.result_mbps_x100 | awk '{ printf("%.2f", $1 / 100) }')"
	for k in lat_p50_us lat_p99_us lat_p999_us lat_max_us cpus errors rejected; do
		row "$1" "$k" "$(sysctl -n kern.kwq_crypto.result_$k)"
	done
	row "$1" jain "$(sysctl -n kern.kwq_crypto.result_jain | awk '{ printf("%.3f", $1 / 1000) }')"
	sysctl -n kern.kwq_crypto.result_shares | tr ' ' '\n' | awk -F: '$2 != "" { v = $2 + 0; if (min == "" || v < min) min = v; if (v > max) max = v }
	    END { printf("q_min_mbps %.2f\nq_max_mbps %.2f\n", min, max) }' |
	while read -r k v; do row "$1" "$k" "$v"; done
}
probe_rows() {	# TEST PREFIX
	tr ' ' '\n' < "$WORK/p" | awk -F= -v p="$2" 'NF == 2 && $1 != "n" && $1 != "wall" && $1 != "cpu" { print p "_" $1, $2 }' |
	while read -r k v; do row "$1" "$k" "$v"; done
}
common="bytes=$BYTES sector=4096 verify=0 weight0=1 limit=0 prio=56"

echo "$(date +%T) driver check, then $REPS reps; PCPU $PCPU, ONE_N $ONE_N, DEPTH_ALL $DEPTH_ALL, ${BYTES} B requests, ${SECS} s"
REP=1
while [ "$REP" -le "$REPS" ]; do
	for STAGE in kwq thread; do
		echo "$(date +%T) rep $REP $STAGE all"
		if [ $STAGE = kwq ]; then
			kc_start $common mode=kwq nq=1 depth="$DEPTH_ALL" cpu=-1 stealable=1 secs="$SECS"
		else
			kc_start $common mode=thread nq=1 depth="$DEPTH_ALL" cpu=-1 threads=0 secs="$SECS"
		fi
		kc_wait; kc_rows all
		[ "$REP" = 1 ] && [ $STAGE = kwq ] && echo "     driver $(sysctl -n kern.kwq_crypto.result_driver)"
		for n in $ONE_N; do
			for what in timer spin; do
				echo "$(date +%T) rep $REP $STAGE one$n $what"
				if [ $STAGE = kwq ]; then
					kc_start $common mode=kwq nq="$n" depth=16 cpu="$PCPU" stealable=0 secs="$SECS"
				else
					kc_start $common mode=thread nq="$n" depth=16 cpu="$PCPU" threads=1 secs="$SECS"
				fi
				sleep 1
				cpuset -l "$PCPU" "$VICTIM" "$what" $((SECS - 3)) > "$WORK/p"
				kc_wait
				probe_rows "one$n" "${what}_pin"
				[ $what = timer ] && kc_rows "one$n"
			done
		done
	done
	REP=$((REP + 1))
done
echo "$(date +%T) done: $OUT_TSV"
