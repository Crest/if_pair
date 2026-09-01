#!/bin/sh
# gif(4) native software TSO (CHOPPER.txt P5, patches/gif-tso.patch):
# the tunnel-family demo adopter chops inner TSO chains with
# tcp_tso_chop() BEFORE encapsulation.  Topology: a gif tunnel
# between two vnet jails over an if_pair underlay (mtu 1500):
#
#   J1: gif0 (10.9.0.1) over pair0b (172.16.0.1) == pair ==
#   J2: gif0 (10.9.0.2) over pair0a (172.16.0.2)
#
# Asserts:
#   1. gif advertises TSO4/TSO6, DEFAULT-OFF; ifconfig tso enables.
#   2. Baseline (tso off) bulk TCP through the tunnel works.
#   3. With tso on both gifs, the transfer completes, the sender
#      gif's average tx packet stays <= gif mtu + slop (the chopper
#      really splits before encap), the receiver's TCP counts ZERO
#      bad checksums (the chopper's software checksums verify end
#      to end), and - when dtrace is available - tcp_tso_chop()
#      demonstrably fired during the run.
#
# Throughput is logged, not asserted.  SKIPs on a kernel whose gif
# lacks the TSO capability.  BENCH_SECS (default 5), LOAD_P
# (default 4).
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

if ! kldstat -q -m if_gif; then
	must "load if_gif" kldload if_gif
	cleanup_push "kldunload if_gif"
fi

SECS=${BENCH_SECS:-5}
P=${LOAD_P:-4}

U1=172.16.0.1; U2=172.16.0.2	# underlay (pair)
T1=10.9.0.1;   T2=10.9.0.2	# tunnel (gif)

J1=$(jname 1); J2=$(jname 2)
create_pair
mkjail "$J1"
mkjail "$J2"
must "move ${PAIRB} into ${J1}" ifconfig "$PAIRB" vnet "$J1"
must "move ${PAIRA} into ${J2}" ifconfig "$PAIRA" vnet "$J2"
must "underlay j1" jexec "$J1" ifconfig "$PAIRB" inet "$U1/32" "$U2" \
    mtu 1500 up
must "underlay j2" jexec "$J2" ifconfig "$PAIRA" inet "$U2/32" "$U1" \
    mtu 1500 up
must_retry 5 "underlay ping" jexec "$J1" ping -q -o -c 3 -t 2 "$U2"

G1=$(jexec "$J1" ifconfig gif create) || fail "gif create in ${J1} failed"
G2=$(jexec "$J2" ifconfig gif create) || fail "gif create in ${J2} failed"
must "tunnel j1" jexec "$J1" ifconfig "$G1" tunnel "$U1" "$U2"
must "tunnel j2" jexec "$J2" ifconfig "$G2" tunnel "$U2" "$U1"
must "gif addr j1" jexec "$J1" ifconfig "$G1" inet "$T1/32" "$T2" up
must "gif addr j2" jexec "$J2" ifconfig "$G2" inet "$T2/32" "$T1" up
must_retry 5 "tunnel ping" jexec "$J1" ping -q -o -c 3 -t 2 "$T2"

case "$(jexec "$J1" ifconfig -m "$G1" | awk -F'[<>]' '/capabilities=/ { print $2; exit }')" in
*TSO4*) ;;
*)	log "SKIP: ${G1} does not advertise TSO4 (unpatched gif)"
	exit 0 ;;
esac
case "$(jexec "$J1" ifconfig "$G1" | awk -F'[<>]' '/options=/ { print $2; exit }')" in
*TSO4*)	fail "gif TSO enabled by default (must ship off)" ;;
esac
log "ok: gif advertises TSO, default-off"

GIFMTU=$(jexec "$J1" ifconfig "$G1" | awk '/mtu/ { print $NF; exit }')

# Sender-side gif tx counters.
ostats() {
	jexec "$J1" netstat --libxo json -bnI "$G1" | tr ',{}' '\n\n\n' | \
	    awk -F: '
	    /"sent-packets"/ && !p { p = $2 }
	    /"sent-bytes"/ && !b { b = $2 }
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
must_retry 10 "iperf3 server listening" jexec "$J1" nc -z -w 1 "$T2" 5201

# run_avg label: one bulk run J1 -> J2 via the tunnel.
run_avg() {
	label=$1
	set -- $(ostats); p0=$1; b0=$2
	out=$(jexec "$J1" timeout $((SECS + 30)) iperf3 -f g -c "$T2" \
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
	set -- $(ostats); p1=$1; b1=$2
	RUN_AVG=$(( (b1 - b0) / (p1 - p0 + 1) ))
	log "$label: ${rate:-?} Gbit/s, avg gif tx packet ${RUN_AVG} bytes"
}

run_avg "baseline (tso off)"

must "tso on ${G1}" jexec "$J1" ifconfig "$G1" tso
must "tso on ${G2}" jexec "$J2" ifconfig "$G2" tso

CHOPS=
if command -v dtrace >/dev/null 2>&1; then
	dtrace -qn 'fbt::tcp_tso_chop:entry { @ = count(); }
	    tick-'"$((SECS + 2))"'s { printa("%@d", @); exit(0); }' \
	    > "${TMPDIR:-/tmp}/t26_chops.$$" 2>/dev/null &
	DTP=$!
	sleep 1
fi
bs0=$(badsum)
run_avg "gif tso (native chop)"
bs1=$(badsum)
if [ -n "${DTP:-}" ]; then
	wait "$DTP" 2>/dev/null
	CHOPS=$(cat "${TMPDIR:-/tmp}/t26_chops.$$" 2>/dev/null | tr -dc 0-9)
	rm -f "${TMPDIR:-/tmp}/t26_chops.$$"
fi

[ "$RUN_AVG" -le $((GIFMTU + 64)) ] || \
    fail "avg ${RUN_AVG} > gif mtu ${GIFMTU}: chains not chopped before encap"
log "jail2 tcps_rcvbadsum delta: $((bs1 - bs0))"
[ "$((bs1 - bs0))" -eq 0 ] || \
    fail "software checksums rejected by the receiver ($((bs1 - bs0)) bad)"
if [ -n "$CHOPS" ]; then
	log "tcp_tso_chop() calls during run: ${CHOPS}"
	[ "$CHOPS" -gt 0 ] || fail "TSO enabled but the chopper never ran"
else
	log "note: dtrace unavailable or empty; chop-count assert skipped"
fi

log "ok: gif chops TSO chains natively before encapsulation"
pass
