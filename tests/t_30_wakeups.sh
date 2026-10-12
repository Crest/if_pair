#!/bin/sh
# Single-flow wake-up and system-impact profile.  One pair between two
# jails carries an iperf3 run of CONNS connections (default 1) for SECS
# seconds (default 10), once for each value of net.inet.tcp.per_cpu_timers
# in TIMERS (default "0 1"; the original value is restored), and in three
# modes: clean; spinner, a CPU-bound process pinned to the worker's CPU
# whose CPU share says what userland is left there; and timer, a 1 kHz
# timer program (lateness.c) pinned there alone at realtime priority, so
# that only kernel threads can delay it, reporting how late its wake-ups
# are, with the same program run again on that CPU after the run, idle,
# as the baseline.  The two victims are kept apart: a woken
# timeshare thread does not preempt a running one under ULE, so a spinner
# on the same CPU would add its whole slice to the timer's lateness.
#
# Per run one line: receiver Gbit/s; the worker CPU; the worker thread's
# CPU seconds per second; kwq's own counters for the pair (passes/s,
# items per pass, wakeups/s, yields/s, busy share); from dtrace(1), for
# either driver: wake-ups/s and switches/s of the worker thread, its
# sleeps/s with the distribution of the sleep gaps (the share below 20,
# 50, 100 and 500 us is what a bounded idle spin would catch) and its
# blocks on contended sleep mutexes and yields, each with how long they
# kept it off the CPU and whom the CPU went to.  A departure with
# TDI_SLEEPING set is a sleep, with TDI_LOCK a lock block; any other is a
# yield or a preemption (sched:::sleep itself fires on every switch away
# on this kernel).  From
# top(1): that CPU's idle share; from vmstat(8): system context
# switches/s.  With PMC=1 (hwpmc, a07) clean runs add counting of cycles
# and instructions on the worker CPU and the last one a sampling profile
# of it; with LOCKSTAT=1 a lock profile.  LOCKSTAT=1 is not for
# comparisons: its kernel-wide lock probes cost the single flow about
# 15 % on a07 (every run with it landed at 29-32 Gbit/s, without it at
# 33-38; NOTES.md 2026-10-09), so take the lock profile in a run of its
# own.
#
# REPS repeats every (timers, mode) run; the log names the worker's CPU
# and the CPUs the two iperf3 processes ran on, because single-flow
# results are bimodal with placement (NOTES.md 2026-10-09).
# Each connection hashes to a CPU of its own, so the worker is found per
# run: for kwq the worker whose pass counter moved most during one second
# of the lead-in, for the baseline the pool thread that went on CPU most
# often (sched probes); the worker's and the spinner's CPU shares come
# from profile sampling, because a polling worker may never switch out
# (ps(1) reports no CPU time for kernel threads and its %cpu decays too
# slowly to tell one run from the last).
# A measurement harness: it fails only if a run does not complete.  PAIR=pairtq PAIR_MOD=if_pair_tq runs the taskqueue baseline
# (lib.sh).  Requires iperf3; skipped if missing.
. "$(dirname "$0")/lib.sh"

command -v iperf3 >/dev/null 2>&1 || { log "SKIP: iperf3 not installed"; exit 0; }
test_init

SECS=${BENCH_SECS:-10}
CONNS=${CONNS:-1}
TIMERS=${TIMERS:-"0 1"}
MODES=${MODES:-"clean spinner timer"}
REPS=${REPS:-1}				# runs per (timers, mode)
MTU=${PAIR_MTU:-16384}
PMC=${PMC:-0}
LOCKSTAT=${LOCKSTAT:-0}
OUT=${OUT_DIR:-$(mktemp -d /tmp/${TEST}.XXXXXX)}
[ -n "$OUT_DIR" ] || cleanup_push "rm -rf $OUT"
LEAD=2					# discovery seconds
WIN=$((SECS - LEAD - 2))		# measurement window inside the run
[ "$WIN" -ge 3 ] || fail "BENCH_SECS must be at least 7"

orig_timers=$(sysctl -n net.inet.tcp.per_cpu_timers)
cleanup_push "sysctl -q net.inet.tcp.per_cpu_timers=$orig_timers"

create_pair
JS=$(jname s1); JC=$(jname c1)
mkjail "$JS"; mkjail "$JC"
must "move $PAIRA" ifconfig "$PAIRA" vnet "$JS"
must "move $PAIRB" ifconfig "$PAIRB" vnet "$JC"
must "mtu" sh -c "jexec $JS ifconfig $PAIRA mtu $MTU && jexec $JC ifconfig $PAIRB mtu $MTU"
must "addresses" sh -c "jexec $JS ifconfig $PAIRA inet 10.98.1.1/32 10.98.1.2 up && jexec $JC ifconfig $PAIRB inet 10.98.1.2/32 10.98.1.1 up"
must_retry 5 "ping" jexec "$JC" ping -q -o -c 2 -t 2 10.98.1.1
jexec "$JS" iperf3 -s >/dev/null 2>&1 &
cleanup_push "kill $!"
sleep 1

LATE=""
if command -v cc >/dev/null 2>&1 && cc -O2 -o "$OUT/lateness" "$(dirname "$0")/lateness.c" 2>/dev/null; then
	LATE="$OUT/lateness"
else
	log "note: no cc, the timer victim is skipped"
fi
case "$PAIR" in pair) TPREFIX="kwq_net/";; *) TPREFIX="${PAIR}_task_";; esac

rate_of() {
	awk '/receiver/ { for (i = 1; i <= NF; i++) if ($i == "Gbits/sec") { r = $(i - 1); if ($0 ~ /^\[SUM\]/) s = r } }
	    END { print (s != "" ? s : (r != "" ? r : "FAILED")) }' "$1"
}
busiest() {	# the pool thread that did most in one second
	if [ "$PAIR" = pair ]; then
		# By kwq's own pass counters: a polling worker may never switch,
		# so a sched:::on-cpu count can miss it.
		a=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }')
		sleep 1
		b=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }')
		printf '%s\n%s\n' "$a" "$b" | awk '{ d[$1] = $2 - d[$1] } END { for (c in d) if (d[c] > m) { m = d[c]; w = c } if (m > 0) print "kwq_net/" w }'
	else
		dtrace -qn "sched:::on-cpu /index(stringof(curthread->td_name), \"$TPREFIX\") == 0/ { @[stringof(curthread->td_name)] = count(); } tick-1s { trunc(@, 1); printa(\"%s\\n\", @); exit(0); }" 2>/dev/null | awk 'NF { print $1; exit }'
	fi
}
kwq_snap() {	# wakeups passes busy_ns idle_ns items-on-W(both queues) yields tick_yields spins spin_hits spin_ns
	[ "$PAIR" = pair ] || { echo "0 0 0 0 0 0 0 0 0 0"; return; }
	# The last three exist only with the idle poll (kwq-spin-experiment).
	_sp=$(sysctl -n kern.kwq.net.cpu.$1.spins kern.kwq.net.cpu.$1.spin_hits kern.kwq.net.cpu.$1.spin_ns 2>/dev/null | tr '\n' ' ')
	echo "$(sysctl -n kern.kwq.net.cpu.$1.wakeups kern.kwq.net.cpu.$1.passes kern.kwq.net.cpu.$1.busy_ns kern.kwq.net.cpu.$1.idle_ns | tr '\n' ' ') $(( $(sysctl -n kern.kwq.net.queue.$PAIRA.cpu.$1.items) + $(sysctl -n kern.kwq.net.queue.$PAIRB.cpu.$1.items) )) $(sysctl -n kern.kwq.net.cpu.$1.yields kern.kwq.net.cpu.$1.tick_yields | tr '\n' ' ') ${_sp:-0 0 0}"
}
has_oid() { sysctl -n "$1" >/dev/null 2>&1; }

if [ "$PAIR" = pair ] && [ -n "$KWQ_SPIN" ]; then
	# KWQ_SPIN=<us>: the NET class's bounded idle poll, a knob of the
	# kwq-spin-experiment branch; KWQ_HW its hardware wait.
	has_oid kern.kwq.net.idle_spin_us || fail "this kwq has no idle_spin_us knob"
	orig_spin=$(sysctl -n kern.kwq.net.idle_spin_us)
	cleanup_push "sysctl -q kern.kwq.net.idle_spin_us=$orig_spin"
	must "idle_spin_us=$KWQ_SPIN" sysctl -q kern.kwq.net.idle_spin_us="$KWQ_SPIN"
fi
if [ "$PAIR" = pair ] && [ -n "$KWQ_HW" ]; then
	has_oid kern.kwq.poll_hw || fail "this kwq has no poll_hw knob"
	orig_hw=$(sysctl -n kern.kwq.poll_hw)
	cleanup_push "sysctl -q kern.kwq.poll_hw=$orig_hw"
	must "poll_hw=$KWQ_HW" sysctl -q kern.kwq.poll_hw="$KWQ_HW"
fi
log "$PAIR: $CONNS conn, ${SECS}s runs, ${LEAD}s discovery, ${WIN}s window, timers in {$TIMERS}, modes {$MODES}$([ "$PAIR" = pair ] && has_oid kern.kwq.net.idle_spin_us && echo ", idle_spin_us $(sysctl -n kern.kwq.net.idle_spin_us), poll $(sysctl -n kern.kwq.poll_mode 2>/dev/null)")"
failed=0
for t in $TIMERS; do
	must "per_cpu_timers=$t" sysctl -q net.inet.tcp.per_cpu_timers=$t
	for mode in $MODES; do
	for rep in $(seq 1 "$REPS"); do
		tag="t$t.$mode"
		jexec "$JC" iperf3 -f g -c 10.98.1.1 -P "$CONNS" -t "$SECS" > "$OUT/iperf.$tag" 2>&1 &
		ipid=$!
		sleep $((LEAD - 1))
		TNAME=$(busiest)	# one second of sampling, the rest of the lead-in
		if [ -z "$TNAME" ]; then
			wait $ipid; log "timers=$t $mode: no worker thread of $TPREFIX ran during the lead-in ($(rate_of "$OUT/iperf.$tag") Gbit/s)"; failed=$((failed + 1)); continue
		fi
		W=${TNAME##*[/_]}
		spid=0
		if [ "$mode" = spinner ]; then
			cpuset -l "$W" yes > /dev/null & spid=$!
		fi
		# Window start.
		set -- $(kwq_snap "$W"); wk0=$1; pa0=$2; bz0=$3; id0=$4; it0=$5; yl0=$6; ty0=$7; sp0=$8; sh0=$9; sn0=${10}
		cat > "$OUT/gaps.d" <<EOF
/* CPU shares by sampling: a polling worker may never switch, so on-cpu/off-cpu stamps could miss it. */
profile-1999 /pid == $spid/ { @scpu["spinner_samples"] = count(); }
profile-1999 /stringof(curthread->td_name) == "$TNAME"/ { @cpu["worker_samples"] = count(); }
/*
 * sched:::sleep fires on every switch away on this kernel, so classify the
 * worker's departures by the inhibitor: TDI_SLEEPING (2) is a real sleep
 * (the idle wait on the doorbell), TDI_LOCK (8) a block on a contended
 * sleep mutex (turnstile), anything else a yield or a preemption.
 */
sched:::off-cpu /stringof(curthread->td_name) == "$TNAME" && (curthread->td_inhibitors & 2)/ { self->t = timestamp; }
sched:::off-cpu /stringof(curthread->td_name) == "$TNAME" && (curthread->td_inhibitors & 8)/ { self->l = timestamp; @lk["blocks", stringof(args[0]->td_name)] = count(); }
sched:::off-cpu /stringof(curthread->td_name) == "$TNAME" && !(curthread->td_inhibitors & 10)/ { self->y = timestamp; @yt["yields", stringof(args[0]->td_name)] = count(); }
sched:::on-cpu /self->t/ {
	this->g = (timestamp - self->t) / 1000;
	@q["sleep gap, us"] = quantize(this->g);
	@n["gaps"] = count();
	@lt20["lt20"] = sum(this->g < 20 ? 1 : 0);
	@lt50["lt50"] = sum(this->g < 50 ? 1 : 0);
	@lt100["lt100"] = sum(this->g < 100 ? 1 : 0);
	@lt500["lt500"] = sum(this->g < 500 ? 1 : 0);
	self->t = 0;
}
sched:::on-cpu /self->l/ {
	@lq["lock block, us"] = quantize((timestamp - self->l) / 1000);
	@ln["lblocks"] = count();
	@lns["lock_ns"] = sum(timestamp - self->l);
	self->l = 0;
}
sched:::on-cpu /self->y/ {
	this->g = (timestamp - self->y) / 1000;
	@yq["yield gap, us"] = quantize(this->g);
	@yn["ygaps"] = count();
	@ylt20["ylt20"] = sum(this->g < 20 ? 1 : 0);
	@yns["yield_ns"] = sum(timestamp - self->y);
	self->y = 0;
}
sched:::on-cpu /execname == "iperf3"/ { @ip["iperf3", pid, cpu] = count(); }
sched:::on-cpu /stringof(curthread->td_name) == "$TNAME"/ { @sw["switches"] = count(); }
sched:::wakeup /stringof(args[0]->td_name) == "$TNAME"/ { @wk["wakeups"] = count(); }
tick-${WIN}s { trunc(@ip, 4); exit(0); }
EOF
		pids=""
		dtrace -qs "$OUT/gaps.d" > "$OUT/dtrace.$tag" 2>&1 & pids="$pids $!"
		vmstat -w 1 -c $((WIN + 1)) > "$OUT/vmstat.$tag" 2>&1 & pids="$pids $!"
		top -b -P -d 2 -s "$WIN" > "$OUT/top.$tag" 2>&1 & pids="$pids $!"
		if [ "$mode" = timer ] && [ -n "$LATE" ]; then
			rtprio 31 cpuset -l "$W" "$LATE" "$WIN" > "$OUT/late.$tag" 2>&1 & pids="$pids $!"
		fi
		if [ "$PMC" = 1 ] && [ "$mode" = clean ]; then
			pmcstat -c "$W" -s CPU_CYCLES -s INST_RETIRED -w "$WIN" sleep "$WIN" > "$OUT/pmc.$tag" 2>&1 & pids="$pids $!"
		fi
		if [ "$LOCKSTAT" = 1 ] && [ "$mode" = clean ]; then
			lockstat -P sleep "$WIN" > "$OUT/lockstat.$tag" 2>&1 & pids="$pids $!"
		fi
		sleep "$WIN"
		# Window end.
		set -- $(kwq_snap "$W"); wk1=$1; pa1=$2; bz1=$3; id1=$4; it1=$5; yl1=$6; ty1=$7; sp1=$8; sh1=$9; sn1=${10}
		wait $pids 2>/dev/null
		[ "$spid" != 0 ] && kill $spid 2>/dev/null
		wait $ipid
		rate=$(rate_of "$OUT/iperf.$tag"); [ "$rate" = FAILED ] && failed=$((failed + 1))
		idle_late=""
		if [ "$mode" = timer ] && [ -n "$LATE" ]; then
			idle_late=$(rtprio 31 cpuset -l "$W" "$LATE" 2 | awk '{ printf("idle avg %d p99 %d max %d", $2, $3, $4) }')
		fi

		# Reductions.
		wcpu=$(awk -v w="$WIN" '/^  worker_samples/ { printf("%.2f", $2 / (w * 1999)) }' "$OUT/dtrace.$tag"); wcpu=${wcpu:-0}
		kwqs=""
		[ "$PAIR" = pair ] && kwqs=$(echo "$wk0 $wk1 $pa0 $pa1 $bz0 $bz1 $it0 $it1 $WIN $yl0 $yl1 $ty0 $ty1 $sp0 $sp1 $sh0 $sh1 $sn0 $sn1" | awk '{ p = $4 - $3; sp = $15 - $14; printf("; kwq: passes/s %d, items/pass %.1f, wakeups/s %d, yields/s %d+%d, busy %.0f%%", p / $9, p ? ($8 - $7) / p : 0, ($2 - $1) / $9, ($11 - $10) / $9, ($13 - $12) / $9, ($6 - $5) / ($9 * 1e9) * 100); if (sp > 0) printf(", spins/s %d (hit %.0f%%, %.2f s/s)", sp / $9, ($17 - $16) * 100 / sp, ($19 - $18) / ($9 * 1e9)) }')
		dts=$(awk -v w="$WIN" '/^  gaps/ { g = $2 } /^  lt20 / { a = $2 } /^  lt50 / { b = $2 } /^  lt100 / { c = $2 } /^  lt500 / { d = $2 } /^  switches/ { s = $2 } /^  wakeups/ { k = $2 } /^  ygaps/ { y = $2 } /^  ylt20 / { ya = $2 } /^  yield_ns/ { yn = $2 } /^  lblocks/ { l = $2 } /^  lock_ns/ { ln = $2 }
		    END { if (g == 0) g = 1; if (y == 0) y = 1; printf("dtrace: wakeups/s %d, switches/s %d, sleeps/s %d (<20us %d%%, <50us %d%%, <100us %d%%, <500us %d%%), lock blocks/s %d (off-cpu %.2f s/s), yields/s %d (<20us %d%%, off-cpu %.2f s/s)", k / w, s / w, g / w, a * 100 / g, b * 100 / g, c * 100 / g, d * 100 / g, l / w, ln / (w * 1e9), y / w, ya * 100 / y, yn / (w * 1e9)) }' "$OUT/dtrace.$tag")
		cs=$(awk '$1 == "r" && !h { for (i = 1; i <= NF; i++) if ($i == "cs") col = i; h = 1; next } h && col && $1 ~ /^[0-9]+$/ { s += $col; n++ } END { printf("%d", n ? s / n : 0) }' "$OUT/vmstat.$tag")
		idle=$(awk -v w="$W" '$1 == "CPU" && $2 == w":" { for (i = 1; i <= NF; i++) if ($i == "idle") v = $(i - 1) } END { print v }' "$OUT/top.$tag")
		vic=""
		if [ "$mode" = spinner ]; then
			share=$(awk -v w="$WIN" '/^  spinner_samples/ { printf("%.0f", $2 * 100 / (w * 1999)) }' "$OUT/dtrace.$tag")
			vic="; spinner on cpu $W got ${share:-0}% of it"
		elif [ "$mode" = timer ]; then
			lat=$([ -s "$OUT/late.$tag" ] && awk '{ printf("lateness avg %d p99 %d max %d us", $2, $3, $4) }' "$OUT/late.$tag" || echo "no lateness")
			vic="; timer on cpu $W: $lat ($idle_late)"
		fi
		where=$(awk '/^  iperf3 / { printf("%s@%s ", $2, $3) }' "$OUT/dtrace.$tag")
		log "timers=$t $mode: $rate Gbit/s; worker $TNAME on-cpu ${wcpu} s/s${kwqs}; $dts; vmstat cs/s $cs; cpu $W idle ${idle}; iperf3 pid@cpu ${where:-?}$vic"
		awk '/sleep gap/ { p = 1; print "      sleep gaps:" } /lock block, us/ { p = 1; print "      lock blocks:" } /yield gap/ { p = 1; print "      yield gaps:" } p && /\|/ && $NF != "0" { printf("      %7s us %s\n", $1, $NF) }' "$OUT/dtrace.$tag" | head -40
		awk '/^  (blocks|yields) / && $NF > 0 { printf("      %s -> %s %s/s\n", $1, $2, $3 / w) }' w="$WIN" "$OUT/dtrace.$tag" | sort -t' ' -k5 -rn | head -6
		[ "$PMC" = 1 ] && [ "$mode" = clean ] && [ -s "$OUT/pmc.$tag" ] && \
		    awk -v w="$W" -v win="$WIN" 'NR > 1 && NF >= 2 && $1 ~ /^[0-9]/ { c += $1; i += $2 } END { if (c) printf("      hwpmc cpu %s: %.2f G cycles/s, %.2f G instr/s, IPC %.2f\n", w, c / win / 1e9, i / win / 1e9, i / c) }' "$OUT/pmc.$tag"
		[ "$LOCKSTAT" = 1 ] && [ "$mode" = clean ] && [ -s "$OUT/lockstat.$tag" ] && \
		    grep -E "kwq|pairtq|mbuf|^Count|nsec" "$OUT/lockstat.$tag" | head -8 | sed 's/^/      lockstat: /'
		sleep 1
	done
	done
done

if [ "$PMC" = 1 ]; then
	# A sampling profile of the flow's CPU under the last timers setting.
	jexec "$JC" iperf3 -f g -c 10.98.1.1 -P "$CONNS" -t "$SECS" > "$OUT/iperf.prof" 2>&1 &
	ipid=$!; sleep "$LEAD"
	TNAME=$(busiest); W=${TNAME##*[/_]}
	pmcstat -c "$W" -S CPU_CYCLES -O "$OUT/pmc.raw" sleep "$WIN" >/dev/null 2>&1
	wait $ipid
	pmcstat -R "$OUT/pmc.raw" -G "$OUT/pmc.graph" >/dev/null 2>&1
	log "profile of cpu $W (timers=$t, $(rate_of "$OUT/iperf.prof") Gbit/s), top symbols by samples:"
	grep -E "^ *[0-9]+\.[0-9]+%" "$OUT/pmc.graph" | head -14 | sed 's/^/      /'
	[ -s "$OUT/pmc.graph" ] || log "profile: pmcstat produced no graph ($(wc -c < "$OUT/pmc.raw" 2>/dev/null || echo 0) raw bytes)"
fi

[ -n "$OUT_DIR" ] && log "raw output kept in $OUT"
[ "$failed" -gt 0 ] && fail "$failed runs did not complete"
pass
