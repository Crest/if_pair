#!/bin/sh
# Lock-contention attribution for the big-iron scaling residual
# (NOTES.md 2026-08-25: the per-connection lock triangle).  Runs
# iperf3 with LOAD_P parallel connections (default 128) between two
# vnet jails at PAIR_MTU (default 65535) and captures FreeBSD's
# lockstat DTrace provider twice with identical windows: an IDLE
# BASELINE before the load starts, and the loaded window inside
# the run.  The lockstat probes are event-based (nothing cumulative
# to read before/after), so "delta" means loaded-window minus
# baseline-window: ambient contention (ZFS sync, NIC callouts,
# entropy) subtracts out and only load-attributable waiting is
# reported.  Three views:
#
#   time      - per probe flavor and lock name: milliseconds spent
#               spinning/blocking above baseline (adaptive/rw/sx/
#               spin flavors; arg1 is nanoseconds waited,
#               kern_lockstat.c); nonzero baselines are annotated
#   instances - per lock name: spins above baseline, distinct
#               contended addresses and the hottest instance's
#               share in the loaded window.  Many instances with a
#               low top share = per-connection ping-pong (only
#               batching/locality help); one dominant instance = a
#               global bottleneck (directly fixable, name it)
#   stacks    - top contended acquisition sites in the loaded
#               window (lock name plus 4 frames)
#
# The test asserts only that the load and both captures succeeded;
# the tables are the result, printed for the operator alongside the
# run's iperf3 throughput.  Requires iperf3 and dtrace with the
# lockstat provider; skipped when either is missing.  BENCH_SECS
# (default 15) is the iperf3 duration, PROF_SECS (default 10) each
# capture window, LOAD_P the connection count.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi
if ! command -v dtrace >/dev/null 2>&1; then
	log "SKIP: dtrace not installed"
	exit 0
fi
if ! dtrace -ql -n 'lockstat:::adaptive-spin' >/dev/null 2>&1; then
	log "SKIP: lockstat provider not available"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-15}
PROF=${PROF_SECS:-10}
P=${LOAD_P:-128}
MTU=${PAIR_MTU:-65535}

# The machine is NOT rebooted between experiments, and the knobs
# below are kernel-global and persist across runs (unlike driver
# tunables, which reset with the per-test module reload, and TCP
# state, which dies with the per-test vnets).  Log them so every
# saved capture is self-describing and comparable later.
log "host $(hostname) $(uname -r) ncpu $(sysctl -n hw.ncpu)"
for knob in net.inet.tcp.per_cpu_timers \
    debug.lock.delay_base debug.lock.delay_max; do
	log "knob ${knob}=$(sysctl -n "$knob" 2>/dev/null || echo absent)"
done
kldstat -q -m ipsec && log "knob ipsec.ko=loaded" || log "knob ipsec.ko=absent"

# capture_lockstat outfile: one PROF-second lockstat window, all
# four contention flavors.  Times normalize to milliseconds at
# print; ADDR keeps the raw lock address only so the summary can
# count distinct instances.
capture_lockstat() {
	dtrace -x aggsize=32m -qn "
	lockstat:::adaptive-spin,
	lockstat:::spin-spin,
	lockstat:::rw-spin,
	lockstat:::sx-spin
	{
		@time[probename,
		    stringof(((struct lock_object *)arg0)->lo_name)] =
		    sum(arg1);
		@stk[stringof(((struct lock_object *)arg0)->lo_name),
		    stack(4)] = count();
		@addr[stringof(((struct lock_object *)arg0)->lo_name),
		    arg0] = count();
	}
	lockstat:::adaptive-block,
	lockstat:::rw-block,
	lockstat:::sx-block
	{
		@time[probename,
		    stringof(((struct lock_object *)arg0)->lo_name)] =
		    sum(arg1);
	}
	tick-${PROF}s
	{
		normalize(@time, 1000000);
		printa(\"TIME %s %s %@d\n\", @time);
		printa(\"ADDR %s %x %@d\n\", @addr);
		trunc(@stk, 10);
		printa(@stk);
		exit(0);
	}" > "$1" 2> "$1.err"
}

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

OUT=$(mktemp -t ifp_lock) || fail "mktemp failed"
BASE="${OUT}.base"
cleanup_push "rm -f ${OUT} ${OUT}.err ${OUT}.iperf ${BASE} ${BASE}.err"

log "idle baseline lockstat for ${PROF}s (before any load)"
if ! capture_lockstat "$BASE"; then
	head -3 "${BASE}.err" >&2
	fail "baseline dtrace capture failed"
fi
log "ok: baseline captured"

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
sleep 1

log "iperf3 -P ${P} for ${SECS}s at mtu ${MTU}, lockstat for ${PROF}s inside"
jexec "$J2" timeout $((SECS + 30)) iperf3 -c "$A4" -P "$P" -t "$SECS" \
    > "${OUT}.iperf" 2>&1 &
CLI=$!
sleep 2

if ! capture_lockstat "$OUT"; then
	head -3 "${OUT}.err" >&2
	fail "loaded dtrace capture failed"
fi
log "ok: loaded capture finished"

wait "$CLI" || fail "iperf3 did not complete under capture load"
log "ok: iperf3 completed"

rate=$(awk '/SUM.*receiver/ { r = $(NF-2) " " $(NF-1) } END { print r }' \
    "${OUT}.iperf")
log "throughput: ${rate:-unparsed} (receiver SUM, -P ${P})"

if ! grep -q '^TIME' "$OUT"; then
	log "no contention events captured (idle lockstat window?)"
	head -3 "${OUT}.err" | while IFS= read -r line; do log "  !$line!"; done
	fail "lockstat captured nothing under load"
fi

# Lock names (lo_name) are free text and may contain spaces, so
# both summaries parse fields from the RIGHT (the count/ms value is
# always last) and join the name's words with underscores.  Both
# read the baseline first (NR == FNR) and report loaded minus
# baseline; locks whose waiting does not rise above baseline are
# dropped, which is what removes ambient ZFS/NIC/entropy noise.
log "-- wait time above idle baseline (top 15, ms) --"
awk '
    /^TIME/ {
	ms = $NF
	name = $3; for (i = 4; i < NF; i++) name = name "_" $i
	key = sprintf("%-15s %-24s", $2, name)
	if (NR == FNR) base[key] += ms
	else load[key] += ms
    }
    END {
	for (r = 1; r <= 15; r++) {
	    best = ""; bc = 0
	    for (k in load) {
		d = load[k] - base[k]
		if (d > bc) { bc = d; best = k }
	    }
	    if (best == "") break
	    if (base[best] > 0)
		printf "  %s %8d ms  (idle %d ms)\n", best, bc, base[best]
	    else
		printf "  %s %8d ms\n", best, bc
	    delete load[best]
	}
    }' "$BASE" "$OUT" | while IFS= read -r line; do log "$line"; done

log "-- contended instances per lock name, spins above baseline (top 10) --"
awk '
    /^ADDR/ {
	cnt = $NF
	name = $2; for (i = 3; i <= NF - 2; i++) name = name "_" $i
	if (NR == FNR) { bs[name] += cnt; next }
	d[name]++; t[name] += cnt; if (cnt > m[name]) m[name] = cnt
    }
    END {
	for (n in t) {
	    delta = t[n] - bs[n]
	    if (delta <= 0 || t[n] <= 0) continue
	    extra = (bs[n] > 0) ? sprintf("  (idle %d)", bs[n]) : ""
	    printf "%10d spins %6d instances  top %5.1f%%  %s%s\n",
		delta, d[n], m[n] * 100.0 / t[n], n, extra
	}
    }' "$BASE" "$OUT" | sort -rn | head -10 | \
    while IFS= read -r line; do log "  $line"; done

log "-- top contended acquisition stacks (loaded window) --"
awk '!/^(TIME|ADDR)/' "$OUT" | \
    while IFS= read -r line; do log "  $line"; done
pass
