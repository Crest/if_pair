#!/bin/sh
# Load test: iperf3 with hw.ncpu / 3 parallel connections between two
# jails - partial system load, the regime that starved interactive
# userspace on a 128-core server (see the priority-landscape section
# in NOTES.md; on 128 cores this yields ~42 connections, right in the
# reported failure range, while leaving two thirds of the machine
# idle).  Regression test for the worker yield budgets: before the
# fix the iperf3 control connection timed out, and a 1-second
# heartbeat loop stalled for many seconds because sleep(1) depends on
# the very callout timers a saturated worker starves.  After the fix
# both must hold.
#
# Requires iperf3 (benchmarks/iperf3 package); skipped if missing.
# LOAD_SECS (default 20) sets the duration, HEARTBEAT_MAX (default 5)
# the largest tolerated gap in seconds between host heartbeats.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

SECS=${LOAD_SECS:-20}
MAXGAP=${HEARTBEAT_MAX:-5}
NCPU=$(sysctl -n hw.ncpu)
P=$((NCPU / 3))
[ "$P" -lt 1 ] && P=1
log "hw.ncpu=${NCPU} -> ${P} parallel connections for ${SECS}s"

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

over0=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || echo 0)

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
sleep 1

# Host-side heartbeat: gaps in this record ARE the starvation
# symptom, since sleep(1) rides on the callout wheel.
HB=$(mktemp -t ifp_hb) || fail "mktemp failed"
cleanup_push "rm -f ${HB}"
( while :; do date +%s; sleep 1; done ) > "$HB" 2>&1 &
HBPID=$!

log "running iperf3 -P ${P} for ${SECS}s ..."
if ! out=$(jexec "$J2" timeout $((SECS + 60)) iperf3 -c "$A4" -P "$P" \
    -t "$SECS" 2>&1); then
	kill "$HBPID" 2>/dev/null
	echo "$out" | tail -5 >&2
	fail "iperf3 did not complete (the pre-yield failure mode)"
fi
kill "$HBPID" 2>/dev/null
log "ok: iperf3 completed"
echo "$out" | grep -E "SUM.*(sender|receiver)" | while IFS= read -r l; do
	log "  $l"
done

gap=$(awk 'NR > 1 && $1 - prev > max { max = $1 - prev }
    { prev = $1 } END { print max + 0 }' "$HB")
log "largest host heartbeat gap: ${gap}s (limit ${MAXGAP}s)"
if [ "$gap" -gt "$MAXGAP" ]; then
	fail "host heartbeats stalled for ${gap}s during the benchmark"
fi
log "ok: host stayed responsive"

over1=$(sysctl -n net.link.pair.batch_overruns 2>/dev/null || echo 0)
log "batch_overruns during the run: $((over1 - over0)) (informational)"
pass
