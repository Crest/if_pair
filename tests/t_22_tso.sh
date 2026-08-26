#!/bin/sh
# TSO functional test (TSO.txt section 6.4).  Uses the pair-local
# case, which works on stock kernels: two vnet jails, pair at MTU
# 1500 - the wire-MTU scenario TSO exists to rescue.  Asserts:
#
#   1. TSO4/TSO6 are advertised but DISABLED by default.
#   2. ifconfig tso enables both; disabling the matching txcsum
#      also clears the TSO capability (driver convention).
#   3. Deliver-whole actually happens: with TSO enabled, the
#      average transmitted packet size of bulk TCP far exceeds
#      the 1500-byte MTU (asserted > 3000 somewhere in the sweep;
#      typically it approaches the 64 KB chain limit), while the
#      tso-off baseline can never exceed the MTU.
#
# The benchmark half is a t_17-style connection sweep (CONNS,
# default 1..128 in powers of two), run twice - TSO off, then TSO
# on - reporting throughput and average transmitted packet size
# per point.  Throughput is informational, not asserted (VM
# timing is too noisy for ratio assertions); the packet-size
# columns are the functional evidence and double as the
# measurement of what TSO recovers at wire MTU per NOTES.md's
# 2026-08-26 MTU grid.
#
# Counters are read via netstat --libxo json inside the sending
# jail (column positions in plain netstat output shift with blank
# fields; the JSON keys do not).  Requires iperf3; skipped if
# missing.  BENCH_SECS (default 5) per run.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-5}
CONNS=${CONNS:-"1 2 4 8 16 32 64 128"}

A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1); J2=$(jname 2)
create_pair
mkjail "$J1"
mkjail "$J2"
must "move ${PAIRA} into ${J1}" ifconfig "$PAIRA" vnet "$J1"
must "move ${PAIRB} into ${J2}" ifconfig "$PAIRB" vnet "$J2"
must "mtu 1500 side a" jexec "$J1" ifconfig "$PAIRA" mtu 1500
must "mtu 1500 side b" jexec "$J2" ifconfig "$PAIRB" mtu 1500
must "address side a" jexec "$J1" ifconfig "$PAIRA" inet "${A4}/32" "$B4" up
must "address side b" jexec "$J2" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "baseline ping" jexec "$J1" ping -q -o -c 3 -t 2 "$B4"

# ifflags jail ifp: the interface's enabled-options string.
ifflags() {
	jexec "$1" ifconfig "$2" | awk -F'[<>]' '/options=/ { print $2; exit }'
}

flags=$(ifflags "$J2" "$PAIRB")
case "$flags" in
*TSO*) fail "TSO enabled by default: <$flags>" ;;
esac
log "ok: TSO off by default <$flags>"

must "enable tso side a" jexec "$J1" ifconfig "$PAIRA" tso
must "enable tso side b" jexec "$J2" ifconfig "$PAIRB" tso
flags=$(ifflags "$J2" "$PAIRB")
case "$flags" in
*TSO4*) ;;
*) fail "tso did not enable TSO4: <$flags>" ;;
esac
case "$flags" in
*TSO6*) ;;
*) fail "tso did not enable TSO6: <$flags>" ;;
esac
log "ok: tso enables TSO4+TSO6"

# Convention: TSO rides on the matching checksum offload.
must "disable txcsum side b" jexec "$J2" ifconfig "$PAIRB" -txcsum
flags=$(ifflags "$J2" "$PAIRB")
case "$flags" in
*TSO4*) fail "TSO4 survived -txcsum: <$flags>" ;;
esac
case "$flags" in
*TSO6*) ;;
*) fail "TSO6 lost to -txcsum (only TSO4 should be): <$flags>" ;;
esac
log "ok: -txcsum clears TSO4, keeps TSO6"
must "restore txcsum+tso side b" jexec "$J2" ifconfig "$PAIRB" txcsum tso

# ostats jail ifp: "opkts obytes" from the link-level row.
ostats() {
	jexec "$1" netstat --libxo json -bnI "$2" | tr ',{}' '\n\n\n' | awk -F: '
	    /"sent-packets"/ && !p { p = $2 }
	    /"sent-bytes"/ && !b { b = $2 }
	    END { print p+0, b+0 }'
}

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
sleep 1

# run_avg conns: one bulk run; leaves the run's average transmitted
# packet size in RUN_AVG and logs a t_17-style table line.  Called
# directly (not in a command substitution) so log output reaches
# the operator and fail() terminates the test.
run_avg() {
	# Save the argument: the "set --" below clobbers $1.
	conns=$1
	set -- $(ostats "$J2" "$PAIRB"); p0=$1; b0=$2
	out=$(jexec "$J2" timeout $((SECS + 30)) iperf3 -f g -c "$A4" \
	    -P "$conns" -t "$SECS" 2>&1) || fail "iperf3 run (-P $conns) failed"
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
	set -- $(ostats "$J2" "$PAIRB"); p1=$1; b1=$2
	RUN_AVG=$(( (b1 - b0) / (p1 - p0 + 1) ))
	log "$(printf '%5d  %6s  %12d' "$conns" "${rate:-?}" "$RUN_AVG")"
}

# sweep: full connection sweep; leaves the largest per-run average
# transmitted packet size in SWEEP_MAX.
sweep() {
	SWEEP_MAX=0
	log "conns  Gbit/s  avg-tx-bytes  ($1, ${SECS}s runs, mtu 1500)"
	for n in $CONNS; do
		run_avg "$n"
		[ "$RUN_AVG" -gt "$SWEEP_MAX" ] && SWEEP_MAX=$RUN_AVG
		sleep 1
	done
	return 0
}

must "disable tso for baseline, side b" jexec "$J2" ifconfig "$PAIRB" -tso
sweep "tso off"; base_max=$SWEEP_MAX
must "re-enable tso side b" jexec "$J2" ifconfig "$PAIRB" tso
sweep "tso on"; tso_max=$SWEEP_MAX

[ "$base_max" -le 1500 ] || fail "baseline avg $base_max exceeds the MTU?"
[ "$tso_max" -gt 3000 ] || \
    fail "avg tx packet $tso_max bytes with TSO on: frames are not crossing whole"
log "ok: deliver-whole confirmed ($base_max -> $tso_max bytes/packet at mtu 1500)"
pass
