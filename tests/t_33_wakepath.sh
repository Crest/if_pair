#!/bin/sh
# Where the time goes between an enqueue that must wake the worker and
# the handler running, for the driver selected by PAIR (if_pair on kwq, or
# PAIR=pairtq PAIR_MOD=if_pair_tq for the taskqueue baseline), under a
# ping flood between two jails (one packet per wakeup).  Four segments,
# averaged in nanoseconds with the counts, and the distributions of the
# last two:
#   A  enqueue entry -> the wake call (the producer's work before it
#      wakes anyone: list append, doorbell, placement)
#   B  wake call -> the thread made runnable (sleepq, setrunnable)
#   C  runnable -> the worker on CPU (run queue, IPI, idle exit, switch)
#   D  on CPU -> the handler entered (the worker's own path to the work)
# plus the enqueue's total with and without a wake.  fbt probes add
# ~0.15 us to each boundary they stamp.  A measurement harness.
. "$(dirname "$0")/lib.sh"

test_init
SECS=${SECS:-5}
case "$PAIR" in
pair)	ENQ=kwq_enqueue; ENQRET=kwq_enqueue; WAKE=wakeup_one; MOD=if_pair; HANDLER=pair_handler ;;
pairtq)	ENQ=taskqueue_enqueue; ENQRET=taskqueue_enqueue_locked; WAKE=wakeup_any; MOD=if_pair_tq; HANDLER=pair_task_deferred ;;
*)	fail "unknown PAIR $PAIR" ;;
esac

create_pair
JS=$(jname s1); JC=$(jname c1)
mkjail "$JS"; mkjail "$JC"
must "move $PAIRA" ifconfig "$PAIRA" vnet "$JS"
must "move $PAIRB" ifconfig "$PAIRB" vnet "$JC"
must "addresses" sh -c "jexec $JS ifconfig $PAIRA inet 10.99.3.1/32 10.99.3.2 up && jexec $JC ifconfig $PAIRB inet 10.99.3.2/32 10.99.3.1 up"
must_retry 5 "reachable" jexec "$JC" ping -c 1 -t 1 10.99.3.1
must "icmplim in $JS" jexec "$JS" sysctl -q net.inet.icmp.icmplim=1000000
if [ "$PAIR" = pair ] && sysctl -n kern.kwq.net.idle_spin_us >/dev/null 2>&1; then
	# A kwq with the idle poll (kwq-spin-experiment): measure the sleeping worker.
	orig_spin=$(sysctl -n kern.kwq.net.idle_spin_us); cleanup_push "sysctl -q kern.kwq.net.idle_spin_us=$orig_spin"
	must "idle_spin_us=0" sysctl -q kern.kwq.net.idle_spin_us=0
fi
TMPD=$(mktemp -d /tmp/${TEST}.XXXXXX); cleanup_push "rm -rf $TMPD"
# A function that ends in a tail call has no fbt return probe; then the
# enqueue totals are skipped.
dtrace -l -n "fbt::$ENQRET:return" >/dev/null 2>&1 || ENQRET=""

cat > "$TMPD/wp.d" <<DEOF
#pragma D option quiet
fbt::$ENQ:entry { self->e = timestamp; self->w = 0; }
fbt::$WAKE:entry /self->e/ { self->w = timestamp; @s["A enqueue entry -> wake call"] = avg(self->w - self->e); @n["A enqueue entry -> wake call"] = count(); }
sched:::wakeup /self->w/ { wk[(uintptr_t)args[0]] = timestamp; @s["B wake call -> runnable"] = avg(timestamp - self->w); @n["B wake call -> runnable"] = count(); }
${ENQRET:+fbt::$ENQRET:return /self->e/
{
	this->k = self->w ? "enqueue total, woke the worker" : "enqueue total, no wake";
	@s[this->k] = avg(timestamp - self->e); @n[this->k] = count();
	self->e = 0; self->w = 0;
\}}
fbt:$MOD:$HANDLER:entry /self->e/ { self->e = 0; self->w = 0; }	/* the producer ran the handler itself: no wake path */
sched:::on-cpu /wk[(uintptr_t)curthread] != 0/
{
	this->t = wk[(uintptr_t)curthread]; wk[(uintptr_t)curthread] = 0;
	@s["C runnable -> worker on CPU"] = avg(timestamp - this->t); @n["C runnable -> worker on CPU"] = count();
	@qc["C runnable -> worker on CPU, ns"] = quantize(timestamp - this->t);
	self->on = timestamp;
}
fbt:$MOD:$HANDLER:entry /self->on/
{
	@s["D on CPU -> handler entry"] = avg(timestamp - self->on); @n["D on CPU -> handler entry"] = count();
	@qd["D on CPU -> handler entry, ns"] = quantize(timestamp - self->on);
	self->on = 0;
}
END { printa("  %-34s %@8d %@8d\n", @s, @n); printa(@qc); printa(@qd); }
DEOF

dtrace -s "$TMPD/wp.d" -c "sleep $SECS" > "$TMPD/out" 2> "$TMPD/err" &
dpid=$!
sleep 0.5
jexec "$JC" ping -q -f 10.99.3.1 > /dev/null 2>&1 &
tpid=$!
wait $dpid
kill $tpid 2>/dev/null; wait $tpid 2>/dev/null
grep -q "^  [A-D] " "$TMPD/out" || fail "dtrace: $(head -3 "$TMPD/err" | tr '\n' ' ')"
log "$PAIR ($ENQ -> $WAKE -> $HANDLER), ns avg and count:"
grep -E "^  [A-Za-z]" "$TMPD/out" | sed 's/^/    /'
[ -n "$OUT_DIR" ] && cp "$TMPD/out" "$OUT_DIR/wakepath.$PAIR.txt"
pass
