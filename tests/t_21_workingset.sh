#!/bin/sh
# Causal test of the big-iron falloff mechanism (NOTES.md
# 2026-08-26, t_19 P=32 vs P=128): the hypothesis is that past the
# throughput peak the connections' socket buffers outgrow the
# system-level cache, demoting every copy to DRAM - a working-set
# effect, not a lock or CPU shortage (the machine profiles 0% idle
# at P=128 while moving LESS data than the half-idle P=32 peak).
#
# The test holds the connection count at LOAD_P (default 128 - the
# falloff regime) and sweeps the socket buffer size downward with
# iperf3 -w, which fixes SO_SNDBUF/SO_RCVBUF on both ends and
# disables autoscaling.  Interpretation:
#
#   throughput RECOVERS toward the peak as windows shrink
#     -> working-set confirmed: smaller buffers fit the cache
#   throughput only FALLS as windows shrink
#     -> refuted: small windows just starve the pipe; look
#        elsewhere for the falloff
#
# A P=32 default-window run leads the table as the peak reference.
# Each line also reports the pair's drop-counter deltas and
# net.link.pair.batch_overruns (as in t_17) plus the network's
# current mbuf allocation sampled mid-run (netstat -m, host-global
# zones) - the actual buffer footprint, so the working-set axis is
# measured rather than inferred from the -w value.
#
# A measurement harness more than a pass/fail test, but any run
# that does not complete still fails the sweep after the table is
# printed.  Requires iperf3; skipped if missing.  BENCH_SECS
# (default 5) per run; WINDOWS (default "default 4m 1m 256k 64k")
# is the sweep list, where "default" means autoscaling.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-5}
P=${LOAD_P:-128}
WINDOWS=${WINDOWS:-"default 4m 1m 256k 64k"}

# Kernel-global knobs that persist across runs; log for provenance
# as in t_20.
log "host $(hostname) $(uname -r) ncpu $(sysctl -n hw.ncpu)"
log "knob net.inet.tcp.per_cpu_timers=$(sysctl -n \
    net.inet.tcp.per_cpu_timers 2>/dev/null || echo absent)"
kldstat -q -m ipsec && log "knob ipsec.ko=loaded" || log "knob ipsec.ko=absent"

A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1); J2=$(jname 2)
create_pair
mkjail "$J1"
mkjail "$J2"
must "move ${PAIRA} into ${J1}" ifconfig "$PAIRA" vnet "$J1"
must "move ${PAIRB} into ${J2}" ifconfig "$PAIRB" vnet "$J2"
must "address side a" jexec "$J1" ifconfig "$PAIRA" inet "${A4}/32" "$B4" up
must "address side b" jexec "$J2" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "baseline ping" jexec "$J1" ping -q -o -c 3 -t 2 "$B4"

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
sleep 1

# netmem: current bytes allocated to the network, in KiB (first
# field of netstat -m's "current/cache/total" line).
netmem() {
	netstat -m | awk '/bytes allocated to network/ {
		split($1, a, "[K/]"); print a[1]; exit }'
}

# one_run conns window(- for default): prints the table line.
one_run() {
	n=$1; w=$2
	wflag=""
	[ "$w" != "default" ] && wflag="-w $w"
	ov0=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || echo 0)
	set -- $(ifdrops "$J1" "$PAIRA"); ai0=$1; ao0=$2
	set -- $(ifdrops "$J2" "$PAIRB"); bi0=$1; bo0=$2
	jexec "$J2" timeout $((SECS + 30)) iperf3 -f g -c "$A4" \
	    -P "$n" -t "$SECS" $wflag > "$TMPOUT" 2>&1 &
	RUN=$!
	sleep $((SECS / 2 + 1))
	mem=$(netmem)
	if wait "$RUN"; then
		rate=$(awk '
		    /receiver/ {
			for (i = 1; i <= NF; i++)
				if ($i == "Gbits/sec") {
					r = $(i - 1)
					if ($0 ~ /^\[SUM\]/)
						s = r
				}
		    }
		    END { print (s != "" ? s : r) }' "$TMPOUT")
		rate=${rate:-unparsed}
	else
		rate=FAILED
		failed=$((failed + 1))
	fi
	ov1=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || echo 0)
	set -- $(ifdrops "$J1" "$PAIRA"); ai1=$1; ao1=$2
	set -- $(ifdrops "$J2" "$PAIRB"); bi1=$1; bo1=$2
	log "$(printf '%5d  %7s  %6s  %9s  %7d  %6d  %8d' "$n" "$w" \
	    "$rate" "${mem:-?}" \
	    "$((ao1 - ao0 + bo1 - bo0))" "$((ai1 - ai0 + bi1 - bi0))" \
	    "$((ov1 - ov0))")"
	sleep 1
}

TMPOUT=$(mktemp -t ifp_ws) || fail "mktemp failed"
cleanup_push "rm -f ${TMPOUT}"

failed=0
log "conns   window  Gbit/s  netmemKiB  oqdrops   idrop  overruns  (${SECS}s runs)"
one_run 32 default
for w in $WINDOWS; do
	one_run "$P" "$w"
done

if [ "$failed" -gt 0 ]; then
	fail "$failed benchmark runs did not complete"
fi
pass
