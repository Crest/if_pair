#!/bin/sh
# epair(4) TSO capability patch (HANDOFF.txt Step D, CHOPPER.txt
# P4/P6d): the t_22 mirror on the L2 twin.  Requires a kernel whose
# if_epair carries patches/epair-tso.patch; SKIPs when the created
# epair does not advertise TSO4.  Asserts, on an epair at mtu 1500
# between two jails:
#
#   1. TSO4/TSO6 are advertised but DEFAULT-OFF (the forwarded-frame
#      stall analysis; see the patch header).
#   2. ifconfig tso on one side syncs to the peer (epair's TXCSUM
#      peer-sync convention, extended to the TSO bits).
#   3. ifconfig -txcsum clears TSO4 (TSO rides on the checksum
#      request bit).
#   4. With tso enabled both sides, bulk TCP crosses the pair in
#      chains far larger than the MTU (deliver-whole), and the
#      transfer completes.
#
# Throughput is logged, not asserted.  BENCH_SECS (default 5) per
# run, LOAD_P (default 4) connections.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

if ! kldstat -q -m if_epair; then
	must "load if_epair" kldload if_epair
	cleanup_push "kldunload if_epair"
fi

SECS=${BENCH_SECS:-5}
P=${LOAD_P:-4}

EPA=$(ifconfig epair create) || fail "'ifconfig epair create' failed"
case "$EPA" in
epair*a) ;;
*)	fail "unexpected name from create: '${EPA}'" ;;
esac
EPB="${EPA%a}b"
cleanup_push "ifconfig ${EPA} destroy"
log "created ${EPA} + ${EPB}"

# caps iface line: print the <...> list of the named line ("options"
# or "capabilities"); capabilities only appear with ifconfig -m.
caps() { ifconfig -m "$1" | awk -F"[<>]" "/$2=/ { print \$2; exit }"; }

case "$(caps "$EPA" capabilities)" in
*TSO4*) ;;
*)	log "SKIP: ${EPA} does not advertise TSO4 (unpatched if_epair)"
	ifconfig "$EPA" destroy
	exit 0 ;;
esac

case "$(caps "$EPA" options)" in
*TSO4*)	fail "TSO enabled by default (must ship off)" ;;
esac
log "ok: TSO advertised, default-off"

must "enable tso on ${EPA}" ifconfig "$EPA" tso
case "$(caps "$EPA" options)" in
*TSO4*) ;;
*)	fail "ifconfig tso did not enable TSO4" ;;
esac
case "$(caps "$EPB" options)" in
*TSO4*) ;;
*)	fail "TSO did not peer-sync to ${EPB}" ;;
esac
log "ok: tso enables and peer-syncs"

must "disable txcsum on ${EPA}" ifconfig "$EPA" -txcsum
case "$(caps "$EPA" options)" in
*TSO4*)	fail "-txcsum left TSO4 enabled (coupling broken)" ;;
esac
log "ok: -txcsum revokes TSO4"
must "restore txcsum+tso" ifconfig "$EPA" txcsum tso

A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1); J2=$(jname 2)
mkjail "$J1"
mkjail "$J2"
must "move ${EPA} into ${J1}" ifconfig "$EPA" vnet "$J1"
must "move ${EPB} into ${J2}" ifconfig "$EPB" vnet "$J2"
must "jail1 side" jexec "$J1" ifconfig "$EPA" inet "${A4}/24" mtu 1500 up
must "jail2 side" jexec "$J2" ifconfig "$EPB" inet "${B4}/24" mtu 1500 up
must "tso jail2 side" jexec "$J2" ifconfig "$EPB" tso
must_retry 5 "baseline ping" jexec "$J2" ping -q -o -c 3 -t 2 "$A4"

# jail2-side tx counters of its epair end.
ostats() {
	jexec "$J2" netstat --libxo json -bnI "$EPB" | tr ',{}' '\n\n\n' | \
	    awk -F: '
	    /"sent-packets"/ && !p { p = $2 }
	    /"sent-bytes"/ && !b { b = $2 }
	    END { print p+0, b+0 }'
}

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
must_retry 10 "iperf3 server listening" jexec "$J2" nc -z -w 1 "$A4" 5201

set -- $(ostats); p0=$1; b0=$2
out=$(jexec "$J2" timeout $((SECS + 30)) iperf3 -f g -c "$A4" \
    -P "$P" -t "$SECS" 2>&1) || fail "iperf3 run failed"
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
avg=$(( (b1 - b0) / (p1 - p0 + 1) ))
log "tso bulk: ${rate:-?} Gbit/s, avg tx packet ${avg} bytes"
[ "$avg" -gt 3000 ] || \
    fail "avg ${avg}: TSO chains are not crossing the epair whole"

log "ok: epair TSO deliver-whole at mtu 1500"
pass
