#!/bin/sh
# Where does gif(4) spend its time, and what does TSO change?
# (Follow-up to t_26 and the NOTES.md route-caching HYPOTHESIS -
# this test replaces that inference with measurement.)
#
# t_26's topology (gif tunnel between two vnet jails over an
# if_pair underlay at mtu 1500) under a matrix of
# {tso off, tso on} x PROF_CONNS (default "1 4 16") iperf3 runs;
# each cell is profiled with the dtrace profile provider
# (t_19's pipeline: raw kernel PCs, %a formatting, idle
# excluded) and reported as throughput + subsystem buckets +
# top functions.  Buckets beyond t_19's set, chosen for the gif
# question:
#
#   route   fib/rib/nhop lookup machinery (the caching candidate)
#   gifenc  gif_transmit/in_gif_output/ECN - encapsulation proper
#   chop    tcp_tso_chop itself
#   cksum   checksum computation (chopper sw-csums + tcp rx)
#
# Asserts only that load and capture succeed; the distribution is
# the result.  BENCH_SECS (default 15) per cell, PROF_SECS
# (default 8) inside it, PROF_CONNS overrides the ladder.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi
if ! command -v dtrace >/dev/null 2>&1; then
	log "SKIP: dtrace not installed"
	exit 0
fi
dd if=/dev/zero of=/dev/null bs=1m >/dev/null 2>&1 &
DD=$!
smoke=$(dtrace -qn 'profile-497 /arg0 != 0/ { @[arg0] = count(); }
    tick-1s { printa("%a %@d\n", @); exit(0); }' 2>&1)
rc=$?
kill "$DD" 2>/dev/null
if [ "$rc" -ne 0 ] || ! echo "$smoke" | \
    awk 'NF == 2 && $2 ~ /^[0-9]+$/ { ok = 1 } END { exit(ok ? 0 : 1) }'
then
	log "SKIP: kernel-PC sampling not usable on this system"
	exit 0
fi

test_init

if ! kldstat -q -m if_gif; then
	must "load if_gif" kldload if_gif
	cleanup_push "kldunload if_gif"
fi

SECS=${BENCH_SECS:-15}
PROF=${PROF_SECS:-8}
CONNS=${PROF_CONNS:-"1 4 16"}

U1=172.16.0.1; U2=172.16.0.2
T1=10.9.0.1;   T2=10.9.0.2

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

jexec "$J2" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
must_retry 10 "iperf3 server listening" jexec "$J1" nc -z -w 1 "$T2" 5201

OUT=$(mktemp -t ifp_gprof) || fail "mktemp failed"
cleanup_push "rm -f ${OUT} ${OUT}.err ${OUT}.rate"

# cell mode conns: one profiled run; prints the report block.
cell() {
	mode=$1; conns=$2
	if [ "$mode" = "on" ]; then
		jexec "$J1" ifconfig "$G1" tso || fail "tso on ${G1}"
		jexec "$J2" ifconfig "$G2" tso || fail "tso on ${G2}"
	else
		jexec "$J1" ifconfig "$G1" -tso || fail "tso off ${G1}"
		jexec "$J2" ifconfig "$G2" -tso || fail "tso off ${G2}"
	fi
	jexec "$J1" timeout $((SECS + 30)) iperf3 -f g -c "$T2" \
	    -P "$conns" -t "$SECS" > "${OUT}.rate" 2>&1 &
	CLI=$!
	sleep 3
	if ! dtrace -x aggsize=16m -qn "
		profile-497
		/arg0 != 0/
		{ @f[arg0] = count(); }
		tick-${PROF}s { printa(\"%a %@d\n\", @f); exit(0); }" \
	    > "$OUT" 2> "${OUT}.err"; then
		head -3 "${OUT}.err" >&2
		fail "dtrace capture failed (tso=${mode} P=${conns})"
	fi
	wait "$CLI" || fail "iperf3 failed (tso=${mode} P=${conns})"
	rate=$(awk '
	    /receiver/ {
		for (i = 1; i <= NF; i++)
			if ($i == "Gbits/sec") {
				r = $(i - 1)
				if ($0 ~ /^\[SUM\]/)
					s = r
			}
	    }
	    END { print (s != "" ? s : r) }' "${OUT}.rate")
	log "=== tso=${mode} P=${conns}: ${rate:-?} Gbit/s ==="
	awk '
	    NF == 2 && $2 ~ /^[0-9]+$/ {
		total += $2
		if ($1 ~ /idle|mwait|_wfi|cpu_search/) { idle += $2; next }
		live += $2
		sub(/\+0x[0-9a-fA-F]+$/, "", $1)
		f[$1] += $2
		if ($1 ~ /lock|mtx|rmlock|rwlock|sxlock|turnstile|lock_delay|atomic/)
			b["locks"] += $2
		else if ($1 ~ /sched|tdq|ipi_|sleepq|wakeup/ ||
		    $1 ~ /setrunnable|choosethread|mi_switch|critical_exit/)
			b["sched"] += $2
		else if ($1 ~ /copy|bcopy|memcpy|bzero|memmove|uiomove/)
			b["copy"] += $2
		else if ($1 ~ /uma_|mb_|m_free|m_get|malloc|free/)
			b["alloc"] += $2
		else if ($1 ~ /fib[46]?_|rib_|rtalloc|rtfree|nhop_|rt_/)
			b["route"] += $2
		else if ($1 ~ /gif_|in_gif|in6_gif|ip_ecn|ip6_ecn|etherip/)
			b["gifenc"] += $2
		else if ($1 ~ /tcp_tso_chop/)
			b["chop"] += $2
		else if ($1 ~ /cksum/)
			b["cksum"] += $2
		else if ($1 ~ /tcp_|ip_|ip6_|udp_|in_|in6_|netisr/ ||
		    $1 ~ /pair_|sbappend|soreceive|sosend/)
			b["proto"] += $2
		else
			b["other"] += $2
	    }
	    END {
		if (live == 0) { print "NOSAMPLES"; exit 1 }
		printf "samples: %d total, %d non-idle (%.1f%% idle)\n",
		    total, live, idle * 100.0 / (total ? total : 1)
		n = split("locks sched copy alloc route gifenc chop cksum proto other",
		    names, " ")
		for (i = 1; i <= n; i++)
			printf "bucket %-6s %6.1f%%\n",
			    names[i], b[names[i]] * 100.0 / live
		for (r = 1; r <= 14; r++) {
			best = ""; bc = -1
			for (fn in f)
				if (f[fn] > bc) { bc = f[fn]; best = fn }
			if (best == "") break
			printf "top %2d  %-44s %6.1f%%\n",
			    r, best, bc * 100.0 / live
			delete f[best]
		}
	    }' "$OUT" || fail "no samples (tso=${mode} P=${conns})"
}

for p in $CONNS; do
	cell off "$p"
	cell on "$p"
done

log "ok: gif profile matrix complete"
pass
