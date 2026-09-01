#!/bin/sh
# Phase A in-kernel chopper: routed two-pair A/B (HANDOFF.txt Step C,
# CHOPPER.txt P6b).  Requires a kernel with tcp_tso_chop() wired into
# the forwarding balk sites (patches/software-tso-forwarding.patch);
# SKIPs on a stock kernel, where the chop run would stall the
# connection (the balk sites drop TSO chains that outsize the egress
# MTU and the sender never heals - the bug Phase A fixes).
#
#   jail1 == pairXb (tso ON,  mtu 1500) == ROUTER JAIL routes ==
#   pairYa (egress under test, mtu 1500) == jail2
#
# The router is its own vnet jail rather than the host vnet: a
# host firewall (pf on a07 defaults to "block drop in all")
# silently eats jail-initiated and forwarded traffic entering the
# host vnet, and this test must not touch firewall policy.  pf
# rulesets are vnet-scoped, so a fresh vnet jail forwards
# unfiltered while running the same kernel forwarding paths.
#
# Three runs, sender jail1, server jail2, all interfaces mtu 1500:
#
#   1. chop:      egress -tso           - the forwarding hop must
#      split every TSO chain; jail2's average rx packet is wire-size
#      and the transfer completes (per-segment header arithmetic
#      accepted end to end).
#   2. fast path: egress tso            - frames pass WHOLE (the
#      handler prefers the egress TSO grant over chopping); jail2's
#      average rx packet far exceeds the MTU.
#   3. sw-csum:   egress -tso -txcsum   - the chopper must compute
#      full checksums in software; jail2's TCP verifies them in
#      software: tcps_rcvbadsum stays ZERO and the transfer
#      completes.
#
# Throughput is logged, not asserted.  BENCH_SECS (default 5) per
# run, LOAD_P (default 4) connections.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi
if ! strings /boot/kernel/kernel 2>/dev/null | grep -q tcp_tso_chop; then
	log "SKIP: kernel lacks tcp_tso_chop (Phase A patch not installed)"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-5}
P=${LOAD_P:-4}

J1=$(jname 1); J2=$(jname 2); JR=$(jname r)
create_pair; P1A=$PAIRA; P1B=$PAIRB
create_pair; P2A=$PAIRA; P2B=$PAIRB

mkjail "$J1"
mkjail "$J2"
mkjail "$JR"
must "move ${P1B} into ${J1}" ifconfig "$P1B" vnet "$J1"
must "move ${P2B} into ${J2}" ifconfig "$P2B" vnet "$J2"
must "move ${P1A} into ${JR}" ifconfig "$P1A" vnet "$JR"
must "move ${P2A} into ${JR}" ifconfig "$P2A" vnet "$JR"

must "router side 1" jexec "$JR" ifconfig "$P1A" inet 10.1.0.1/32 10.1.0.2 \
    mtu 1500 tso up
must "router side 2" jexec "$JR" ifconfig "$P2A" inet 10.2.0.1/32 10.2.0.2 \
    mtu 1500 up
must "jail1 side" jexec "$J1" ifconfig "$P1B" inet 10.1.0.2/32 10.1.0.1 \
    mtu 1500 tso up
must "jail2 side" jexec "$J2" ifconfig "$P2B" inet 10.2.0.2/32 10.2.0.1 \
    mtu 1500 up
must "jail1 default route" jexec "$J1" route -q add default 10.1.0.1
must "jail2 default route" jexec "$J2" route -q add default 10.2.0.1

must "enable forwarding in router" \
    jexec "$JR" sysctl net.inet.ip.forwarding=1

must_retry 5 "routed ping" jexec "$J1" ping -q -o -c 3 -t 2 10.2.0.2

# jail2-side rx counters of its pair interface.
istats() {
	jexec "$J2" netstat --libxo json -bnI "$P2B" | tr ',{}' '\n\n\n' | \
	    awk -F: '
	    /"received-packets"/ && !p { p = $2 }
	    /"received-bytes"/ && !b { b = $2 }
	    END { print p+0, b+0 }'
}

badsum() {
	jexec "$J2" netstat -s -p tcp | awk '
	    /discarded for bad checksum/ { print $1; found = 1; exit }
	    END { if (!found) print 0 }'
}

jexec "$J2" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
must_retry 10 "iperf3 server listening" jexec "$J1" nc -z -w 1 10.2.0.2 5201

# run_avg label: one bulk run jail1 -> jail2; RUN_AVG = avg rx size.
run_avg() {
	# Save the label: the "set --" below clobbers $1.
	label=$1
	set -- $(istats); p0=$1; b0=$2
	out=$(jexec "$J1" timeout $((SECS + 30)) iperf3 -f g -c 10.2.0.2 \
	    -P "$P" -t "$SECS" 2>&1) || fail "iperf3 run ($label) failed"
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
	set -- $(istats); p1=$1; b1=$2
	RUN_AVG=$(( (b1 - b0) / (p1 - p0 + 1) ))
	log "$label: ${rate:-?} Gbit/s, avg rx packet ${RUN_AVG} bytes"
}

# Run 1: egress cannot split - the kernel chopper must.
must "egress tso off" jexec "$JR" ifconfig "$P2A" -tso
run_avg "chop (egress -tso)          "
[ "$RUN_AVG" -le 1600 ] || \
    fail "avg ${RUN_AVG} with egress -tso: forwarding hop did not chop"

# Run 2: egress CAN split - frames must pass whole (handler ordering).
must "egress tso on" jexec "$JR" ifconfig "$P2A" tso
run_avg "fast path (egress tso)      "
[ "$RUN_AVG" -gt 3000 ] || \
    fail "avg ${RUN_AVG} with egress tso: fast path is not deliver-whole"

# Run 3: no checksum offload on the egress - software checksums,
# verified in software by jail2's TCP.
must "egress tso+txcsum off" jexec "$JR" ifconfig "$P2A" -tso -txcsum
bs0=$(badsum)
run_avg "sw-csum (egress -tso -txcsum)"
bs1=$(badsum)
[ "$RUN_AVG" -le 1600 ] || \
    fail "avg ${RUN_AVG} with egress -tso -txcsum: did not chop"
log "jail2 tcps_rcvbadsum delta: $((bs1 - bs0))"
[ "$((bs1 - bs0))" -eq 0 ] || \
    fail "software checksums rejected by the receiver ($((bs1 - bs0)) bad)"

log "ok: kernel chopper splits, fast path passes whole, sw csums verify"
pass
