#!/bin/sh
# Several pairs of the same driver under load at once, over a range of
# receive queue depths: PAIRS pairs (default 8), each between two jails
# of its own, each carrying an iperf3 run of STREAMS parallel
# connections (default "1 8 32", i.e. 8, 64 and 256 connections in
# total with 8 pairs) for SECS seconds (default 10), all pairs
# concurrently; the whole set repeated for every depth in DEPTHS
# (default "4096", the driver default; e.g. "64 128 256 512 1024 2048
# 4096").  While the load runs, a ping crosses every pair; the
# round-trip time under load is reported next to the idle baseline.
# Per depth and load one line: total receiver throughput, the per-pair
# minimum, average and maximum, the RTT average and maximum over all
# pairs, and the output-queue drops (ENOBUFS at the depth) summed over
# the pairs; then the per-pair rates.  On the kwq driver the items per
# pass and passes per second over all pair queues are added.
#
# The depth is set per driver: kwq through the queues' own
# kern.kwq.net.queue.<name>.limit after creation, the taskqueue baseline
# through net.link.pairtq.qlimit before the pairs are created; the pairs
# are recreated for every depth on both drivers so the runs are alike.
#
# A measurement harness: it fails only if a run does not complete.
# Requires iperf3 (benchmarks/iperf3 package); skipped if missing.
# PAIR=pairtq PAIR_MOD=if_pair_tq runs it against the taskqueue
# baseline (lib.sh).
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

NP=${PAIRS:-8}
SECS=${BENCH_SECS:-10}
MTU=${PAIR_MTU:-16384}
STREAMS=${STREAMS:-"1 8 32"}
DEPTHS=${DEPTHS:-4096}
OUT=$(mktemp -d /tmp/${TEST}.XXXXXX)
cleanup_push "rm -rf $OUT"

# The baseline's depth knob is read at pair creation; restore it at the end.
if [ "$PAIR" != pair ]; then
	orig_qlimit=$(sysctl -n ${PAIR_OID}.qlimit 2>/dev/null) || orig_qlimit=""
	[ -n "$orig_qlimit" ] && cleanup_push "sysctl ${PAIR_OID}.qlimit=$orig_qlimit"
fi

rate_of() {	# iperf3 -f g output -> receiver Gbit/s (SUM line for -P > 1)
	awk '/receiver/ { for (i = 1; i <= NF; i++) if ($i == "Gbits/sec") { r = $(i - 1); if ($0 ~ /^\[SUM\]/) s = r } }
	    END { print (s != "" ? s : (r != "" ? r : "FAILED")) }' "$1"
}

kwq_sums() {	# "items passes" summed over all pair queues (kwq driver only)
	sysctl kern.kwq.net.queue 2>/dev/null | awk -v p="$PAIR" '
	    $1 ~ "^kern.kwq.net.queue." p "[0-9]+[ab].cpu.[0-9]+.items:" { it += $2 }
	    $1 ~ "^kern.kwq.net.queue." p "[0-9]+[ab].cpu.[0-9]+.passes:" { pa += $2 }
	    END { print it, pa }'
}

# Output-queue drops summed over both sides of every pair.
oqdrops_sum() {
	local i=1 s=0 a b
	while [ $i -le $NP ]; do
		eval "a=\$A_$i; b=\$B_$i"
		set -- $(ifdrops "$(jname s$i)" "$a"); s=$((s + $2))
		set -- $(ifdrops "$(jname c$i)" "$b"); s=$((s + $2))
		i=$((i + 1))
	done
	echo $s
}

# set_depth d: kwq's per-queue limit on every pair queue (after creation).
set_depth() {
	local i=1 a b
	[ "$PAIR" = pair ] || return 0
	while [ $i -le $NP ]; do
		eval "a=\$A_$i; b=\$B_$i"
		must "depth $1 on $a and $b" sh -c "sysctl -q kern.kwq.net.queue.$a.limit=$1 kern.kwq.net.queue.$b.limit=$1 >/dev/null"
		i=$((i + 1))
	done
}

setup_pairs() {
	local i=1 JS JC
	SERVERS=""
	while [ $i -le $NP ]; do
		create_pair
		eval "A_$i=\$PAIRA; B_$i=\$PAIRB"
		JS=$(jname s$i); JC=$(jname c$i)
		mkjail "$JS"
		mkjail "$JC"
		must "move ${PAIRA} into ${JS}" ifconfig "$PAIRA" vnet "$JS"
		must "move ${PAIRB} into ${JC}" ifconfig "$PAIRB" vnet "$JC"
		must "mtu ${MTU} pair $i" sh -c "jexec $JS ifconfig $PAIRA mtu $MTU && jexec $JC ifconfig $PAIRB mtu $MTU"
		must "address pair $i" sh -c "jexec $JS ifconfig $PAIRA inet 10.99.$i.1/32 10.99.$i.2 up && jexec $JC ifconfig $PAIRB inet 10.99.$i.2/32 10.99.$i.1 up"
		must_retry 5 "baseline ping pair $i" jexec "$JC" ping -q -o -c 2 -t 2 10.99.$i.1
		jexec "$JS" iperf3 -s >/dev/null 2>&1 &
		SERVERS="$SERVERS $!"
		cleanup_push "kill $!"
		i=$((i + 1))
	done
	sleep 1
}

# Explicit teardown between depths; the cleanup stack still covers a failure.
teardown_pairs() {
	local i=1 a
	kill $SERVERS 2>/dev/null
	while [ $i -le $NP ]; do
		jail -r "$(jname s$i)" >/dev/null 2>&1
		jail -r "$(jname c$i)" >/dev/null 2>&1
		i=$((i + 1))
	done
	sleep 1
	i=1
	while [ $i -le $NP ]; do
		eval "a=\$A_$i"
		ifconfig "$a" destroy >/dev/null 2>&1
		i=$((i + 1))
	done
	sleep 1
}

failed=0
for d in $DEPTHS; do
	if [ "$PAIR" != pair ]; then
		[ -n "$orig_qlimit" ] || fail "${PAIR_OID}.qlimit missing: the baseline needs the depth knob"
		must "depth $d for new pairs" sysctl -q ${PAIR_OID}.qlimit=$d
	fi
	setup_pairs
	set_depth $d

	# Idle RTT: 20 pings per pair, all pairs, report the worst average.
	i=1; idle=0
	while [ $i -le $NP ]; do
		a=$(jexec "$(jname c$i)" ping -q -c 20 -i 0.05 10.99.$i.1 2>/dev/null | awk -F/ '/round-trip/ { print $5 }')
		idle=$(echo "$idle $a" | awk '{ print ($2 > $1) ? $2 : $1 }')
		i=$((i + 1))
	done
	log "depth $d: $NP pairs of $PAIR, ${SECS}s runs, mtu $MTU; idle rtt (worst pair avg) ${idle} ms"

	for n in $STREAMS; do
		set -- $(kwq_sums); it0=${1:-0}; pa0=${2:-0}
		drops0=$(oqdrops_sum)
		pids=""	# clients and pings only: the servers never exit
		i=1
		while [ $i -le $NP ]; do
			jexec "$(jname c$i)" timeout $((SECS + 30)) iperf3 -f g -c 10.99.$i.1 -P "$n" -t "$SECS" > "$OUT/iperf.$i" 2>&1 &
			pids="$pids $!"
			i=$((i + 1))
		done
		sleep 2
		i=1
		while [ $i -le $NP ]; do
			jexec "$(jname c$i)" ping -q -c $(( (SECS - 4) * 5 )) -i 0.2 10.99.$i.1 > "$OUT/ping.$i" 2>&1 &
			pids="$pids $!"
			i=$((i + 1))
		done
		wait $pids
		rates=""; i=1
		while [ $i -le $NP ]; do
			r=$(rate_of "$OUT/iperf.$i")
			[ "$r" = FAILED ] && failed=$((failed + 1))
			rates="$rates $r"
			i=$((i + 1))
		done
		stats=$(echo "$rates" | awk '{ mn = 1e9; mx = 0; s = 0; c = 0; for (i = 1; i <= NF; i++) { if ($i == "FAILED") continue; s += $i; c++; if ($i < mn) mn = $i; if ($i > mx) mx = $i } printf("total %.1f Gbit/s; per pair min/avg/max %.1f/%.1f/%.1f", s, mn, (c ? s / c : 0), mx) }')
		# "round-trip min/avg/max/stddev = A/B/C/D ms": B is $(NF-3), C $(NF-2)
		rtt=$(cat "$OUT"/ping.* | awk -F'[/ ]' '/round-trip/ { a += $(NF-3); c++; if ($(NF-2) > mx) mx = $(NF-2) } END { if (c) printf("rtt avg %.3f ms max %.3f ms", a / c, mx); else print "rtt: no data" }')
		drops=$(( $(oqdrops_sum) - drops0 ))
		extra=""
		if sysctl -n kern.kwq.nqueues >/dev/null 2>&1; then
			set -- $(kwq_sums); it1=${1:-0}; pa1=${2:-0}
			[ $((pa1 - pa0)) -gt 0 ] && extra="; kwq items/pass $(( (it1 - it0) / (pa1 - pa0) )), passes/s $(( (pa1 - pa0) / SECS ))"
		fi
		log "$(printf 'depth %4d %3d' $d $((n * NP))) conns ($n per pair): $stats; $rtt; oqdrops $drops$extra"
		log "     per pair Gbit/s:$rates"
		sleep 1
	done
	teardown_pairs
done

if [ "$failed" -gt 0 ]; then
	fail "$failed iperf3 runs did not complete"
fi
pass
