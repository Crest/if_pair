#!/bin/sh
# kwq_rows.sh - the kwq_test rows of a comparison on one machine
# (kwq/tests/SPEC-p4e.md S4): for every stage (a module directory, a bound
# and a knob setting) the scenarios selected by ROWS, each followed by the
# consumer's counters, into OUT; kwq_rows_report.py summarises OUT.
#
#   KWQ_SSH="ssh lab sudo -n" TESTMOD=/path/kwq_test.ko sh kwq_rows.sh STAGEFILE OUT
#
# STAGEFILE: one stage per line, "LABEL MODDIR [L=us] [knob ...]"; MODDIR
# holds kwq.ko (or kwq/kwq.ko), the knobs are kern.kwq.-relative sysctls
# set after the load (e.g. net.latency_us=20), L= is the stage's latency
# bound for the report's predictions and is not a knob.  TESTMOD is the
# one kwq_test.ko used with every stage (required: a stage's own test
# module may lack knobs the rows need).  Runs kwq/tests/run_p1b.sh
# (RUNNER) with KWQ_SSH and MODDIR; the remote helper always goes through
# `$KWQ_SSH 'sh -s'` with a heredoc, and the final unload through sh -c,
# because doas and sudo take a command, not a command string.
#
# ROWS (default "fanin cost pingpong ifpair coloc"; also burst sparse
# conformance):
#   fanin        1, 2, 4 and 16 single-item producers into one list
#   cost         the cost scenario (cross-CPU, then the same CPU)
#   pingpong     the wake path's round trip
#   ifpair       the ifpair shapes: 1000/1500 ns per packet with 1 and 2
#                senders, 3000/500 (S) and 10000/500 (X) with 1 and 2
#   coloc        the cost scenario with the producer at priority 100 on the
#                worker's CPU (needs prod_prio in the test module)
#   burst        SPEC 2.1: BURSTS "B:G_us" pairs (default the spec's seven)
#                in the placements PLACES (default "cross same")
#   sparse       SPEC 2.3: ifpair with SPARSE_FANIN senders (default "1 8")
#                each sleeping SPARSE_GAPS us between packets (default
#                "20 100 1000")
#   conformance  SPEC 2.4: one producer at CONF_RATES items/s (default
#                "10000 100000 1000000") with constant (mode 1) and
#                exponential (mode 2) spacing
# REPS repetitions (default 2).
set -u
STAGEFILE=$1; OUT=$2; REPS=${REPS:-2}
ROWS=${ROWS:-"fanin cost pingpong ifpair coloc"}
# absolute, and this checkout's own: a relative RUNNER lost every counter
# line on 2026-10-24, and a hard-coded path outside the tree runs another
# checkout's run_p1b.sh, which need not have the knobs these rows set
RUNNER=${RUNNER:-$(cd "$(dirname "$0")/../kwq/tests" && pwd)/run_p1b.sh}
BURSTS=${BURSTS:-"1:2000 4:200 16:200 64:500 64:2000 64:50000 1024:5000"}
PLACES=${PLACES:-"cross same"}
SPARSE_FANIN=${SPARSE_FANIN:-"1 8"}
SPARSE_GAPS=${SPARSE_GAPS:-"20 100 1000"}
CONF_RATES=${CONF_RATES:-"10000 100000 1000000"}
[ -n "${KWQ_SSH:-}" ] || { echo "set KWQ_SSH (e.g. \"ssh lab sudo -n\")" >&2; exit 2; }
[ -n "${TESTMOD:-}" ] || { echo "set TESTMOD to the one kwq_test.ko of this comparison" >&2; exit 2; }
export KWQ_SSH TESTMOD
: > "$OUT"

# The consumer's counters since the (re)load at the start of the last run:
# the busiest NET worker's, the queues' items on its CPU, and every
# branch's wake counters (0 where the build lacks them).  `doorbells' is
# there for SPEC-p4e.md S1's identity, wakeups == doorbells + ticks.  There
# is no count-threshold field: the specification dropped count thresholds
# (KWQ.md S16 "Below the doorbell"), so wakes_count and
# wake_promotions_count are not read and no row reports them.
counters() {	# tag
	$KWQ_SSH 'sh -s' <<'REMOTE' 2>&1 | sed "s/^/[$label r$rep $1] /" >> "$OUT"
W=$(sysctl kern.kwq.net.cpu 2>/dev/null | awk -F'[.:]' '$6 == "passes" && $NF + 0 > m { m = $NF + 0; w = $5 } END { print w }')
C=kern.kwq.net.cpu.$W
g() { sysctl -n "$1" 2>/dev/null || echo 0; }
qs() { sysctl kern.kwq.net.queue 2>/dev/null | awk -F'[.:]' -v w="$W" -v f="$1" '$7 == w && $8 == f { s += $NF } END { print s + 0 }'; }
p=$(g $C.passes)
i=$(qs items)
echo "counters W=$W passes=$p items=$i items_per_pass=$(( p > 0 ? i / p : 0 )) wakeups=$(g $C.wakeups) deferred=$(g $C.wakes_deferred) defer_ns=$(g $C.wake_defer_ns) by_deadline=$(( $(g $C.wakes_deadline) + $(g $C.wake_promotions_deadline) )) urgent=$(g $C.wake_promotions_urgent) low=$(g $C.wakes_low) self=$(g $C.wake_self) low_ns=$(g $C.wake_low_ns) idle_cpu=$(g $C.wake_idle_cpu) batched=$(g $C.db_batched) flushed=$(g $C.db_flushed) ticks=$(g $C.ticks) ticks_empty=$(g $C.ticks_empty) ticks_single=$(g $C.ticks_single) tick_enters=$(g $C.tick_enters) tick_exits=$(g $C.tick_exits) tick_ms=$(( $(g $C.tick_ns) / 1000000 )) tick_barren=$(g $C.tick_barren) doorbells=$(qs doorbells) db_skipped=$(qs doorbells_skipped) overruns=$(qs overruns) overruns_switched=$(qs overruns_switched) parks=$(qs parks) debts=$(qs debts) prio_violations=$(g $C.prio_violations) switched=$(g $C.switched) idle_ms=$(( $(g $C.idle_ns) / 1000000 )) busy_ms=$(( $(g $C.busy_ns) / 1000000 )) maxlat_us=$(( $(qs maxlat_ns) / 1000 ))"
REMOTE
}
runner() {	# tag scenario [env...]: one run_p1b.sh invocation, its result lines tagged
	tag=$1; sc=$2; shift 2
	env "$@" MODDIR="$moddir" KWQ_KNOBS="$knobs" sh "$RUNNER" "$sc" 2>&1 | grep -E "^$sc|^    |TIMEOUT|FAIL" | sed "s/^/[$label r$rep $tag] /" >> "$OUT"
	counters "$tag"
}
has() { case " $ROWS " in *" $1 "*) return 0;; esac; return 1; }

rep=1
while [ "$rep" -le "$REPS" ]; do
	while read -r label moddir rest; do
		case $label in ''|\#*) continue;; esac
		L=0; knobs=""
		for tok in $rest; do case $tok in L=*) L=${tok#L=};; *) knobs="$knobs $tok";; esac; done
		knobs=${knobs# }
		echo "=== rep $rep stage $label ($moddir) L=$L knobs: ${knobs:-none} $(date +%T)" >> "$OUT"
		if has fanin; then for f in 1 2 4 16; do runner "fanin$f" fanin FANIN=$f BATCH=1; done; fi
		if has cost || has pingpong; then
			env MODDIR="$moddir" KWQ_KNOBS="$knobs" sh "$RUNNER" cost pingpong 2>&1 | grep -E "^cost|^pingpong|consumer busy|round trip|TIMEOUT|FAIL" | sed "s/^/[$label r$rep] /" >> "$OUT"
		fi
		if has coloc; then runner coloc cost KT_EXTRA="cpu=1 pcpu=1 prod_prio=100"; fi
		if has ifpair; then
			for shape in ifpair1:1:1000:1500 ifpair2:2:1000:1500 ifpairS1:1:3000:500 ifpairS2:2:3000:500 ifpairX1:1:10000:500 ifpairX2:2:10000:500; do
				f=${shape%%:*}; r=${shape#*:}; fn=${r%%:*}; r=${r#*:}; pn=${r%%:*}; cn=${r#*:}
				runner "$f" ifpair IFACES=1 SPREAD=1 FANIN=$fn BATCH=1 PROD_NS=$pn CONS_NS=$cn
			done
		fi
		if has burst; then
			for bg in $BURSTS; do
				B=${bg%%:*}; G=${bg#*:}
				for pl in $PLACES; do
					case $pl in same) extra="cpu=1 pcpu=1 prod_prio=100";; *) extra="";; esac
					runner "burst_${B}_${G}_$pl" burst BURST_ITEMS=$B BURST_GAP_US=$G BURST_MODE=0 KT_EXTRA="$extra"
				done
			done
		fi
		if has sparse; then
			for fn in $SPARSE_FANIN; do for gp in $SPARSE_GAPS; do
				runner "sparse_${fn}_${gp}" ifpair IFACES=1 SPREAD=1 FANIN=$fn BATCH=1 PROD_NS=0 CONS_NS=500 PROD_GAP_US=$gp
			done; done
		fi
		if has conformance; then
			for mode in 1 2; do for rate in $CONF_RATES; do
				gap=$((1000000 / rate)); [ "$gap" -lt 1 ] && gap=1
				runner "conf_${mode}_${rate}" burst BURST_ITEMS=1 BURST_GAP_US=$gap BURST_MODE=$mode
			done; done
		fi
	done < "$STAGEFILE"
	rep=$((rep + 1))
done
$KWQ_SSH 'sh -c "kldstat -q -m kwq_test && kldunload kwq_test; kldstat -q -m kwq && kldunload kwq; kldstat | grep -c kwq"' >> "$OUT" 2>&1
echo "=== done $(date +%T)" >> "$OUT"
