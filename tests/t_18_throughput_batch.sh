#!/bin/sh
# Extended throughput sweep: the t_17 connection sweep (1..128 in
# powers of two, 5 seconds each) repeated for net.link.pair.batch
# sizes 16, 64 and 256, ending in one pivot table of receiver-side
# averages - the batch-size tuning instrument.  The original batch
# size is restored on success.  Runs for roughly three minutes.
#
# Each run line carries the per-run deltas of the pair's drop
# counters (oqdrops, idrop; both sides summed) and of
# net.link.pair.batch_overruns; with a small batch the count budget
# should fire first (few overruns), with a large one the tick budget
# takes over.
#
# Requires iperf3 (benchmarks/iperf3 package); skipped if missing.
# BENCH_SECS (default 5) sets the per-run duration.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-5}
OID=net.link.pair.batch
orig=$(sysctl -n $OID) || fail "$OID does not exist"
cleanup_push "sysctl $OID=$orig"

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

RES=$(mktemp -t ifp_bench) || fail "mktemp failed"
cleanup_push "rm -f ${RES}"

failed=0
for b in 16 64 256; do
	must "set batch size to ${b}" sysctl "${OID}=${b}"
	for n in 1 2 4 8 16 32 64 128; do
		ov0=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || \
		    echo 0)
		set -- $(ifdrops "$J1" "$PAIRA"); ai0=$1; ao0=$2
		set -- $(ifdrops "$J2" "$PAIRB"); bi0=$1; bo0=$2
		if out=$(jexec "$J2" timeout $((SECS + 30)) iperf3 -f g \
		    -c "$A4" -P "$n" -t "$SECS" 2>&1); then
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
		ov1=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || \
		    echo 0)
		set -- $(ifdrops "$J1" "$PAIRA"); ai1=$1; ao1=$2
		set -- $(ifdrops "$J2" "$PAIRB"); bi1=$1; bo1=$2
		log "$(printf \
		    'batch %3d  conns %3d  %6s  oqdrops %-7d idrop %-6d overruns %d' \
		    "$b" "$n" "$rate" "$((ao1 - ao0 + bo1 - bo0))" \
		    "$((ai1 - ai0 + bi1 - bi0))" "$((ov1 - ov0))")"
		echo "$b $n $rate" >> "$RES"
		sleep 1
	done
done

log "conns  batch16  batch64  batch256  (Gbits/sec receiver average)"
awk '{ a[$1 "," $2] = $3 }
    END {
	split("1 2 4 8 16 32 64 128", ns, " ")
	for (i = 1; i <= 8; i++) {
		n = ns[i]
		printf "%5d  %7s  %7s  %8s\n", \
		    n, a["16," n], a["64," n], a["256," n]
	}
    }' "$RES" | while IFS= read -r row; do
	log "$row"
done

if [ "$failed" -gt 0 ]; then
	fail "$failed of 24 benchmark runs did not complete"
fi
pass
