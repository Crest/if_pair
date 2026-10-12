#!/bin/sh
# What waking the worker costs the producer, by the worker's state at
# that moment (SCHED.md S16): fbt on kwq_enqueue() entry and return, with
# the kwq:::doorbell probe saying what the call found: "live" (the list
# was non-idle, nothing rung), "rang, awake" (ring placement under the
# worker lock), "rang, polling" (the same plus the store the poll sees),
# "rang, asleep" (plus wakeup_one(): sleep queue, run queue, IPI).  So
# "asleep" minus "awake" is the wakeup itself.  From kwq:::pass-start,
# the doorbell-to-first-pass latency by the state the doorbell found.
# OUT_DIR keeps the full dtrace output per run.  Traffic: a ping flood between the jails (one item per
# doorbell) and a TCP flow (batches), each for SECS seconds, under the
# idle poll settings in SPINS ("0 50") with the hardware wait in HWS
# ("1 0" where available).  Costs include the fbt probes' own overhead
# (two timestamps, ~0.3 us of the enqueue total).  A measurement harness.
. "$(dirname "$0")/lib.sh"

test_init
[ "$PAIR" = pair ] || fail "kwq only"
command -v iperf3 >/dev/null 2>&1 || { log "SKIP: iperf3 not installed"; exit 0; }

SECS=${SECS:-5}
SPINS=${SPINS:-"0 50"}
HWS=${HWS:-"1 0"}

create_pair
JS=$(jname s1); JC=$(jname c1)
mkjail "$JS"; mkjail "$JC"
must "move $PAIRA" ifconfig "$PAIRA" vnet "$JS"
must "move $PAIRB" ifconfig "$PAIRB" vnet "$JC"
must "addresses" sh -c "jexec $JS ifconfig $PAIRA inet 10.99.2.1/32 10.99.2.2 up && jexec $JC ifconfig $PAIRB inet 10.99.2.2/32 10.99.2.1 up"
must_retry 5 "reachable" jexec "$JC" ping -c 1 -t 1 10.99.2.1
must "icmplim in $JS" jexec "$JS" sysctl -q net.inet.icmp.icmplim=1000000
must "iperf3 server" jexec "$JS" iperf3 -s -D
HAVE_POLL=0
if sysctl -n kern.kwq.net.idle_spin_us >/dev/null 2>&1; then
	HAVE_POLL=1
	orig_spin=$(sysctl -n kern.kwq.net.idle_spin_us); cleanup_push "sysctl -q kern.kwq.net.idle_spin_us=$orig_spin"
	orig_hw=$(sysctl -n kern.kwq.poll_hw); cleanup_push "sysctl -q kern.kwq.poll_hw=$orig_hw"
	log "poll offered: $(sysctl -n kern.kwq.poll_mode), period $(sysctl -n kern.kwq.poll_period_ns) ns"
fi
TMPD=$(mktemp -d /tmp/${TEST}.XXXXXX); cleanup_push "rm -rf $TMPD"

cat > "$TMPD/db.d" <<'DEOF'
#pragma D option quiet
fbt:kwq:kwq_enqueue:entry { self->e0 = timestamp; self->st = "live, no doorbell"; }
kwq:::doorbell /self->e0/ { self->st = arg2 == 2 ? "rang, asleep" : arg2 == 1 ? "rang, polling" : "rang, awake"; dbst[arg1] = self->st; }
fbt:kwq:kwq_enqueue:return /self->e0/
{
	@en[self->st] = avg(timestamp - self->e0);
	@enq[self->st] = quantize(timestamp - self->e0);
	@enn[self->st] = count();
	self->e0 = 0;
}
kwq:::pass-start /arg3 > 0 && dbst[arg1] != 0/ { @lat[dbst[arg1]] = avg(arg3); @latq[dbst[arg1]] = quantize(arg3); @latn[dbst[arg1]] = count(); }
END
{
	printf("--enqueue ns by what it found (avg, n)\n"); printa("  %-22s %@8d %@8d\n", @en, @enn);
	printf("--doorbell-to-first-pass ns by state (avg, n)\n"); printa("  %-22s %@8d %@8d\n", @lat, @latn);
	printf("--enqueue ns distribution\n"); printa(@enq);
	printf("--doorbell-to-first-pass ns distribution\n"); printa(@latq);
}
DEOF

run() {	# label, then the traffic command runs for SECS
	label=$1; shift
	dtrace -s "$TMPD/db.d" -c "sleep $SECS" > "$TMPD/out" 2> "$TMPD/err" &
	dpid=$!
	sleep 0.5
	"$@" > /dev/null 2>&1 &
	tpid=$!
	wait $dpid
	kill $tpid 2>/dev/null; wait $tpid 2>/dev/null
	if ! grep -q "^--enqueue ns by what it found" "$TMPD/out"; then
		log "$label: dtrace failed: $(head -2 "$TMPD/err" | tr '\n' ' ')"; failed=$((failed + 1)); return
	fi
	log "$label:"
	awk '/^--enqueue ns by what/ { print "      enqueue ns by what it found (avg, n):"; p = 1; next } /^--doorbell-to-first-pass ns by state/ { print "      doorbell-to-first-pass ns (avg, n):"; p = 1; next } /^--/ { p = 0 } p && /^  [a-z]/ && NF >= 3 { printf("    %s\n", $0) }' "$TMPD/out"
	[ -n "$OUT_DIR" ] && cp "$TMPD/out" "$OUT_DIR/$(echo "$label" | tr -c 'a-z0-9=\n' '_').txt"
}

failed=0
if [ "$HAVE_POLL" = 1 ]; then
	mode=$(sysctl -n kern.kwq.poll_mode | awk '{ print $1 }')
	[ "$mode" = spin ] && HWS="0"
else
	HWS="-"; SPINS="-"
fi
for hw in $HWS; do
	[ "$hw" = "-" ] || must "poll_hw=$hw" sysctl -q kern.kwq.poll_hw="$hw"
	for sp in $SPINS; do
		if [ "$sp" = "-" ]; then
			tag="$PAIR"
		else
			must "idle_spin_us=$sp" sysctl -q kern.kwq.net.idle_spin_us="$sp"
			tag="idle_spin_us=$sp poll_hw=$hw ($(sysctl -n kern.kwq.poll_mode))"
		fi
		run "flood, $tag" jexec "$JC" ping -q -f 10.99.2.1
		run "tcp, $tag" jexec "$JC" iperf3 -c 10.99.2.1 -t $((SECS + 2))
	done
	[ "$HAVE_POLL" = 1 ] && [ "$mode" = spin ] && break
done
[ "$failed" -gt 0 ] && fail "$failed runs did not complete"
pass
