#!/bin/sh
# Throughput sweep: iperf3 between two jails with 1, 2, 4, ... 128
# parallel connections, 5 seconds each, reporting one line per run
# with the receiver-side average throughput plus the per-run deltas
# of the pair's drop counters (oqdrops: receive queue full; idrop:
# discarded on input, e.g. netisr backpressure; both sides summed)
# and of net.link.pair.batch_overruns - so a scaling falloff carries
# its own evidence of whether packet loss or tick overruns were
# involved.  A measurement harness more than a pass/fail test, but a
# run that does not complete (the historic starvation failure mode)
# still fails the sweep after the table is printed.
#
# Requires iperf3 (benchmarks/iperf3 package); skipped if missing.
# BENCH_SECS (default 5) sets the per-run duration; PAIR_MTU
# (default 16384, the driver default) sets both sides' MTU.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-5}
MTU=${PAIR_MTU:-16384}
A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1); J2=$(jname 2)
create_pair
mkjail "$J1"
mkjail "$J2"
must "move ${PAIRA} into ${J1}" ifconfig "$PAIRA" vnet "$J1"
must "move ${PAIRB} into ${J2}" ifconfig "$PAIRB" vnet "$J2"
must "mtu ${MTU} side a" jexec "$J1" ifconfig "$PAIRA" mtu "$MTU"
must "mtu ${MTU} side b" jexec "$J2" ifconfig "$PAIRB" mtu "$MTU"
must "address side a" jexec "$J1" ifconfig "$PAIRA" inet "${A4}/32" "$B4" up
must "address side b" jexec "$J2" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "baseline ping" jexec "$J1" ping -q -o -c 3 -t 2 "$B4"

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
sleep 1

failed=0
log "conns  Gbit/s  oqdrops   idrop  overruns  (${SECS}s runs, mtu ${MTU})"
for n in 1 2 4 8 16 32 64 128; do
	ov0=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || echo 0)
	set -- $(ifdrops "$J1" "$PAIRA"); ai0=$1; ao0=$2
	set -- $(ifdrops "$J2" "$PAIRB"); bi0=$1; bo0=$2
	if out=$(jexec "$J2" timeout $((SECS + 30)) iperf3 -f g -c "$A4" \
	    -P "$n" -t "$SECS" 2>&1); then
		rate=$(echo "$out" | awk '
		    /receiver/ {
			for (i = 1; i <= NF; i++)
				if ($i == "Gbits/sec") {
					r = $(i - 1)
					if ($0 ~ /^\[SUM\]/)
						s = r
				}
		    }
		    END { print (s != "" ? s : r) }')
		rate=${rate:-unparsed}
	else
		rate=FAILED
		failed=$((failed + 1))
	fi
	ov1=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || echo 0)
	set -- $(ifdrops "$J1" "$PAIRA"); ai1=$1; ao1=$2
	set -- $(ifdrops "$J2" "$PAIRB"); bi1=$1; bo1=$2
	log "$(printf '%5d  %6s  %7d  %6d  %8d' "$n" "$rate" \
	    "$((ao1 - ao0 + bo1 - bo0))" "$((ai1 - ai0 + bi1 - bi0))" \
	    "$((ov1 - ov0))")"
	sleep 1
done

if [ "$failed" -gt 0 ]; then
	fail "$failed of 8 benchmark runs did not complete"
fi
pass
