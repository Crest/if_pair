#! /usr/libexec/atf-sh
# ATF test program for kwq.ko, driven through kwq_test.ko's sysctls
# (../PLAN.txt P3).  Run under kyua:
#
#   kyua test -k /path/to/kwq/tests/Kyuafile
#   kyua test -k ... -v test_suites.kwq.moddir=/root/kwq   # modules there
#   kyua report
#
# Every case needs root and at least 4 CPUs (skipped otherwise), loads
# the two modules if they are not loaded and unloads in cleanup what it
# loaded.  The scenarios and their knobs: ../kwq_test/kwq_test.c; the
# properties asserted: ../SCHED.md S10, ../KWQ.md S10.8, and for the
# regimes and the priority invariant ../KWQ.md S16 and SPEC-p4e.md.  The "sleep"
# scenario (a handler that sleeps must panic an INVARIANTS kernel) is
# deliberately not here: it takes the machine down.

# ---------------------------------------------------------------- helpers

kwq_mod() {
	local moddir
	moddir=$(atf_config_get moddir "")
	if [ -n "$moddir" ]; then
		if [ -f "$moddir/kwq/kwq.ko" ]; then
			echo "$moddir/kwq/$1.ko" | sed "s|/kwq/kwq_test.ko|/kwq_test/kwq_test.ko|"
		else
			echo "$moddir/$1.ko"
		fi
	else
		echo "$1"
	fi
}

# Load kwq.ko and kwq_test.ko unless loaded; remember what we loaded.
kwq_setup() {
	[ "$(id -u)" -eq 0 ] || atf_skip "needs root"
	[ "$(sysctl -n hw.ncpu)" -ge 4 ] || atf_skip "needs at least 4 CPUs"
	: > loaded
	for m in kwq kwq_test; do
		if ! kldstat -q -m $m; then
			kldload "$(kwq_mod $m)" || atf_skip "cannot load $(kwq_mod $m)"
			echo $m >> loaded
		fi
	done
	[ "$(sysctl -n kern.kwq_test.result_state)" != running ] || \
	    atf_skip "another kwq_test scenario is running"
	# knob defaults, so a case sees only what it sets
	sysctl -q kern.kwq_test.cost_us=0 kern.kwq_test.limit=0 kern.kwq_test.cpu=-1 \
	    kern.kwq_test.items=1000 kern.kwq_test.reps=10 kern.kwq_test.secs=3 \
	    kern.kwq_test.weight_a=1 kern.kwq_test.weight_b=1 kern.kwq_test.cost_a=50 \
	    kern.kwq_test.cost_b=500 kern.kwq_test.batch=1 kern.kwq_test.pairs=1 \
	    kern.kwq_test.fanin=1 kern.kwq_test.ifaces=1 kern.kwq_test.spread=1 \
	    kern.kwq_test.prod_ns=1000 kern.kwq_test.cons_ns=1500 \
	    kern.kwq_test.qname=test kern.kwq_test.allow_panic=0 >/dev/null
	# the SPEC-p4e knobs, on a test module that has them: so the burst and
	# interrupt-load cases leave nothing behind for the next case (writing
	# intr_secs=0 drains the stressor's callout)
	sysctl -q kern.kwq_test.prod_gap_us=0 kern.kwq_test.prod_prio=-1 \
	    kern.kwq_test.burst_items=64 kern.kwq_test.burst_gap_us=500 \
	    kern.kwq_test.burst_mode=0 kern.kwq_test.intr_secs=0 \
	    kern.kwq_test.intr_cpu=-1 >/dev/null 2>&1 || :
}

kwq_cleanup() {
	[ -f loaded ] || return 0
	# a scenario still running would make the unload fail: wait for it
	local n=0
	while [ "$(sysctl -n kern.kwq_test.result_state 2>/dev/null)" = running ] && \
	    [ $n -lt 300 ]; do sleep 0.2; n=$((n + 1)); done
	# Unload in reverse order of loading, kwq_test first in any case.
	# "done" is published before the scenario thread has exited, so an
	# unload right after a run can see EBUSY for a moment: retry.
	for m in kwq_test kwq; do
		grep -qx $m loaded || continue
		n=0
		while kldstat -q -m $m && ! kldunload $m 2>/dev/null; do
			sleep 0.1; n=$((n + 1))
			[ $n -lt 100 ] || { echo "cannot unload $m" >&2; break; }
		done
	done
	return 0
}

# kwq_run scenario [knob=value ...]: start it and wait for the result.
kwq_run() {
	local name=$1 kv n
	shift
	for kv in "$@"; do
		sysctl -q "kern.kwq_test.$kv" >/dev/null || atf_fail "sysctl kern.kwq_test.$kv"
	done
	sysctl -q kern.kwq_test.scenario=$name >/dev/null
	sysctl -q kern.kwq_test.run=1 >/dev/null || atf_fail "cannot start $name"
	n=0
	while [ "$(sysctl -n kern.kwq_test.result_state)" = running ]; do
		sleep 0.2
		n=$((n + 1))
		[ $n -lt 600 ] || atf_fail "$name: no result after 120 s"
	done
	echo "$name: $(sysctl -n kern.kwq_test.result_state) $(sysctl -n kern.kwq_test.result_msg)"
	echo "  in=$(R in) out=$(R out) rejected=$(R rejected) discarded=$(R discarded)" \
	    "runs=$(R runs) fifo_errors=$(R fifo_errors) dups=$(R dups)" \
	    "cycles=$(R cycles_a)/$(R cycles_b) maxlat_us=$(R maxlat_a_us)/$(R maxlat_b_us)"
}

R() { sysctl -n kern.kwq_test.result_$1; }
Q() { sysctl -n kern.kwq.net.queue.$1; }

kwq_done() {
	[ "$(R state)" = done ] || atf_fail "$1: $(R state): $(R msg)"
}

# kwq_ratio_within a b lo hi: 1000*a/b must lie in [lo, hi]
kwq_ratio_within() {
	local r
	[ "$2" -gt 0 ] || atf_fail "ratio: denominator 0"
	r=$(( $1 * 1000 / $2 ))
	[ $r -ge $3 ] && [ $r -le $4 ] || atf_fail "ratio $1/$2 = 0.$r, wanted 0.$3..0.$4"
}

# ---------------------------------------------------------------- P0

atf_test_case lifecycle cleanup
lifecycle_head() {
	atf_set descr "create/activate/enqueue/drain/destroy 100 times; every item run, discarded or rejected exactly once, per-CPU FIFO kept"
	atf_set require.user root
}
lifecycle_body() {
	kwq_setup
	kwq_run lifecycle reps=100 items=1000
	kwq_done lifecycle
	atf_check_equal 0 "$(R fifo_errors)"
	atf_check_equal 0 "$(R dups)"
	[ "$(R in)" -eq $(( $(R out) + $(R discarded) + $(R rejected) )) ] || \
	    atf_fail "items lost or duplicated: in $(R in) out $(R out) discarded $(R discarded) rejected $(R rejected)"
	atf_check_equal 0 "$(sysctl -n kern.kwq.nqueues)"
}
lifecycle_cleanup() { kwq_cleanup; }

atf_test_case fifo cleanup
fifo_head() {
	atf_set descr "200000 items round-robin over the CPUs: per-CPU sequence numbers never observed out of order"
	atf_set require.user root
}
fifo_body() {
	kwq_setup
	kwq_run fifo items=100000 reps=2
	kwq_done fifo
	atf_check_equal 0 "$(R fifo_errors)"
	atf_check_equal 0 "$(R dups)"
	[ "$(R out)" -gt 0 ] || atf_fail "nothing delivered"
}
fifo_cleanup() { kwq_cleanup; }

atf_test_case reject cleanup
reject_head() {
	atf_set descr "one CPU's list of 8 items drained at 20 us per item against a nanosecond producer on another CPU: ENOBUFS, counted by caller and service alike"
	atf_set require.user root
}
reject_body() {
	kwq_setup
	# One target list: round-robin over many CPUs never fills any list
	# (on 128 CPUs the producer's cycle is longer than an item's service).
	kwq_run reject cpu=1 limit=8 cost_us=20 items=5000 reps=2
	kwq_done reject
	[ "$(R rejected)" -gt 0 ] || atf_fail "no rejections with limit 8"
	[ "$(R in)" -eq $(( $(R out) + $(R rejected) )) ] || \
	    atf_fail "in $(R in) != out $(R out) + rejected $(R rejected)"
	atf_check_equal 0 "$(R dups)"
}
reject_cleanup() { kwq_cleanup; }

atf_test_case discard cleanup
discard_head() {
	atf_set descr "KWQ_F_DISCARD: a drain with items still queued hands them to the handler with n < 0, none lost, none twice"
	atf_set require.user root
}
discard_body() {
	kwq_setup
	kwq_run discard cpu=1 cost_us=20 limit=100000 items=5000 reps=2
	kwq_done discard
	[ "$(R discarded)" -gt 0 ] || atf_fail "nothing discarded at drain"
	[ "$(R in)" -eq $(( $(R out) + $(R discarded) )) ] || \
	    atf_fail "in $(R in) != out $(R out) + discarded $(R discarded)"
	atf_check_equal 0 "$(R dups)"
}
discard_cleanup() { kwq_cleanup; }

atf_test_case notify cleanup
notify_head() {
	atf_set descr "kwq_notify(): every final state observed, runs == newly queued signals, later signals coalesced"
	atf_set require.user root
}
notify_body() {
	kwq_setup
	kwq_run notify items=100000
	kwq_done notify
	[ "$(R runs)" -ge 1 ] || atf_fail "handler never ran"
	[ "$(R coalesced)" -gt 0 ] || atf_fail "100000 signals and none coalesced"
	atf_check_equal "$(R runs)" "$(R out)"
}
notify_cleanup() { kwq_cleanup; }

atf_test_case bad_names cleanup
bad_names_head() {
	atf_set descr "queue names outside [A-Za-z0-9_-] are refused by kwq_create() (the length bound is enforced by the qname knob's own buffer and cannot be reached from here)"
	atf_set require.user root
}
bad_names_body() {
	kwq_setup
	for name in a.b a/b 'a b' 'x%25y' 'cpu.0' ''; do
		kwq_run lifecycle reps=1 items=10 "qname=$name"
		[ "$(R state)" = fail ] || atf_fail "name '$name' was accepted"
		case "$(R msg)" in *kwq_create*) ;; *) atf_fail "name '$name': unexpected failure: $(R msg)" ;; esac
	done
	kwq_run lifecycle reps=1 items=10 qname=ok-name_1
	kwq_done "good name"
	atf_check_equal 0 "$(sysctl -n kern.kwq.nqueues)"
}
bad_names_cleanup() { kwq_cleanup; }

atf_test_case class_limit cleanup
class_limit_head() {
	atf_set descr "kern.kwq.<class>.limit refuses 0 (EINVAL), by sysctl and as a loader tunable, and keeps its value; a queue created afterwards with limit = 0 takes items"
	atf_set require.user root
}
class_limit_body() {
	local c v
	# The tunable is read when kwq.ko registers its sysctls, so it is
	# tested only when this case loads the module.
	if ! kldstat -q -m kwq && kenv kern.kwq.bulk.limit=0 >/dev/null; then
		: > kenv_set
	fi
	kwq_setup
	if [ -f kenv_set ]; then
		kenv -u kern.kwq.bulk.limit
		rm kenv_set
		atf_check_equal 4096 "$(sysctl -n kern.kwq.bulk.limit)"
	fi
	for c in net bulk blocking; do
		v=$(sysctl -n kern.kwq.$c.limit)
		echo "kern.kwq.$c.limit=$v" >> limits
		atf_check -s not-exit:0 -o ignore -e match:"Invalid argument" \
		    sysctl kern.kwq.$c.limit=0
		atf_check_equal "$v" "$(sysctl -n kern.kwq.$c.limit)"
		atf_check -o ignore sysctl kern.kwq.$c.limit=8
		atf_check_equal 8 "$(sysctl -n kern.kwq.$c.limit)"
		atf_check -o ignore sysctl kern.kwq.$c.limit=$v
	done
	kwq_run lifecycle reps=1 items=10 limit=0
	kwq_done "limit = 0 after the refused writes"
}
class_limit_cleanup() {
	[ -f kenv_set ] && kenv -u kern.kwq.bulk.limit
	[ -f limits ] && kldstat -q -m kwq && sysctl -q $(cat limits) >/dev/null
	kwq_cleanup
}

# ---------------------------------------------------------------- P1b

atf_test_case fairness cleanup
fairness_head() {
	atf_set descr "two saturating queues of equal weight with 50 and 500 us items share one CPU's time within 10 %"
	atf_set require.user root
}
fairness_body() {
	kwq_setup
	kwq_run fairness secs=3
	kwq_done fairness
	kwq_ratio_within "$(R cycles_a)" "$(R cycles_b)" 900 1100
}
fairness_cleanup() { kwq_cleanup; }

atf_test_case fairness_2to1 cleanup
fairness_2to1_head() {
	atf_set descr "weights 2:1 give a 2:1 share of CPU time within 10 %"
	atf_set require.user root
}
fairness_2to1_body() {
	kwq_setup
	kwq_run fairness secs=3 weight_a=2
	kwq_done fairness
	kwq_ratio_within "$(R cycles_a)" "$(R cycles_b)" 1800 2200
}
fairness_2to1_cleanup() { kwq_cleanup; }

atf_test_case latency cleanup
latency_head() {
	atf_set descr "a 1 kHz light queue next to a 20 us flood: its worst enqueue-to-run latency stays within ten quanta (SCHED.md S10: 2Q + c on hardware, WITNESS kernels are slower)"
	atf_set require.user root
}
latency_body() {
	local q
	kwq_setup
	q=$(sysctl -n kern.kwq.net.quantum_us)
	kwq_run latency secs=3 cost_a=20
	kwq_done latency
	[ "$(R runs)" -gt 100 ] || atf_fail "light queue ran only $(R runs) times in 3 s"
	[ "$(R maxlat_b_us)" -le $(( 10 * q )) ] || \
	    atf_fail "light queue worst latency $(R maxlat_b_us) us > 10 x quantum ($q us)"
}
latency_cleanup() { kwq_cleanup; }

atf_test_case gaming cleanup
gaming_head() {
	atf_set descr "a burst-and-pause producer gets no more CPU than a flood and at most a couple of boosts (grace rule, SCHED.md B6)"
	atf_set require.user root
}
gaming_body() {
	local try b0
	kwq_setup
	# Once per whole-suite run (2026-10-24, three times) the gaming queue
	# collected 4-10 boosts and passed alone the next minute: the boosts
	# counter persists across runs on a loaded module, and the suite's
	# earlier cases leave the worker in a state the first run of this
	# scenario inherits.  One retry before reporting.
	for try in 1 2; do
		b0=$(Q ktb.cpu.1.boosts 2>/dev/null || echo 0)
		kwq_run gaming secs=3
		kwq_done gaming
		[ $(( $(Q ktb.cpu.1.boosts) - b0 )) -le 2 ] && break
		[ $try -eq 1 ] && echo "gaming: $(( $(Q ktb.cpu.1.boosts) - b0 )) boosts on the first try, retrying once"
	done
	[ "$(R cycles_b)" -le $(( $(R cycles_a) + $(R cycles_a) / 50 )) ] || \
	    atf_fail "gaming queue got more CPU than the flood: $(R cycles_b) vs $(R cycles_a)"
	[ $(( $(Q ktb.cpu.1.boosts) - b0 )) -le 2 ] || atf_fail "gaming queue collected $(( $(Q ktb.cpu.1.boosts) - b0 )) boosts"
	[ "$(Q ktb.cpu.1.grace)" -gt 100 ] || atf_fail "grace rule barely applied: $(Q ktb.cpu.1.grace) hits"
}
gaming_cleanup() { kwq_cleanup; }

atf_test_case overrun cleanup
overrun_head() {
	atf_set descr "a budget-ignoring handler with 2 ms items is counted as overrunning and parked; the cooperative queue keeps at least a third of the CPU"
	atf_set require.user root
}
overrun_body() {
	kwq_setup
	kwq_run overrun secs=3
	kwq_done overrun
	[ "$(Q ktb.cpu.1.overruns)" -gt 0 ] || atf_fail "no overrun counted"
	[ "$(Q ktb.cpu.1.parks)" -gt 0 ] || atf_fail "overrunner never parked"
	[ "$(R cycles_b)" -le $(( 2 * $(R cycles_a) )) ] || \
	    atf_fail "overrunner took $(R cycles_b) cycles against $(R cycles_a): more than twice the cooperative queue"
}
overrun_cleanup() { kwq_cleanup; }

atf_test_case yield cleanup
yield_head() {
	atf_set descr "a 1 kHz callout on the saturated CPU keeps firing: the worker yields at tick boundaries"
	atf_set require.user root
}
yield_body() {
	kwq_setup
	kwq_run yield secs=3
	# The bhyve guest stalls callouts for 20-40 ms now and then on its
	# own (SCHED.md S10.2, a VM artifact never seen on hardware): the
	# module's lateness check cannot be judged there.
	if [ "$(R state)" = fail ] && [ "$(sysctl -n kern.vm_guest)" != none ]; then
		case "$(R msg)" in *"callout late"*) atf_skip "VM callout stall: $(R msg)" ;; esac
	fi
	kwq_done yield
	[ "$(R runs)" -gt 1000 ] || atf_fail "callout fired only $(R runs) times in 3 s"
	[ "$(sysctl -n kern.kwq.net.cpu.1.tick_yields)" -gt 0 ] || atf_fail "worker never yielded at a tick"
}
yield_cleanup() { kwq_cleanup; }

atf_test_case cost cleanup
cost_head() {
	atf_set descr "smoke: one producer floods an empty handler for 3 s; items flow and are accounted"
	atf_set require.user root
}
cost_body() {
	kwq_setup
	kwq_run cost secs=3 items=1024
	kwq_done cost
	[ "$(R out)" -gt 100000 ] || atf_fail "only $(R out) items in 3 s"
	[ "$(R out)" -le "$(R in)" ] || atf_fail "out $(R out) > in $(R in)"
}
cost_cleanup() { kwq_cleanup; }

atf_test_case ifpair cleanup
ifpair_head() {
	atf_set descr "smoke: if_pair's shape, two senders into one worker with 1 us / 1.5 us of work per packet; items flow, nothing rejected"
	atf_set require.user root
}
ifpair_body() {
	kwq_setup
	kwq_run ifpair secs=3 items=1024 fanin=2 limit=1000000
	kwq_done ifpair
	[ "$(R out)" -gt 10000 ] || atf_fail "only $(R out) packets in 3 s"
	atf_check_equal 0 "$(R rejected)"
}
ifpair_cleanup() { kwq_cleanup; }

# ---- notifiers and items on one queue (the "mix" scenarios) ----------
# The assertions state the wanted behaviour: arrival-order delivery of
# notifiers among items, notifiers under the budget, no starvation of the
# item list by a re-arming notifier.  Where the implementation does not
# yet provide it the case fails, which is the point.

atf_test_case mixorder cleanup
mixorder_head() {
	atf_set descr "a notifier signalled between two groups of items is delivered between them (arrival order, not notifiers-first)"
	atf_set require.user root
}
mixorder_body() {
	kwq_setup
	kwq_run mixorder items=50 reps=20
	kwq_done mixorder
	echo "inversions=$(R inversions) exact_errors=$(R exact_errors) calls=$(R calls) runs=$(R runs)"
	atf_check_equal "$(R exact_errors)" 0
	[ "$(R runs)" -eq 20 ] || atf_fail "notifier ran $(R runs) times for 20 signals"
	[ "$(R inversions)" -eq 0 ] || atf_fail "$(R inversions) items delivered on the wrong side of the notifier"
}
mixorder_cleanup() { kwq_cleanup; }

atf_test_case mixpattern cleanup
mixpattern_head() {
	atf_set descr "every pattern of up to four items and two notifiers is delivered in arrival order, each notifier exactly once (tail and head notifiers included)"
	atf_set require.user root
}
mixpattern_body() {
	kwq_setup
	kwq_run mixpattern
	kwq_done mixpattern
	echo "patterns=$(R patterns) inversions=$(R inversions) exact_errors=$(R exact_errors)"
	atf_check_equal "$(R patterns)" 120
	atf_check_equal "$(R exact_errors)" 0
	[ "$(R inversions)" -eq 0 ] || atf_fail "$(R inversions) out-of-order deliveries over 120 patterns"
}
mixpattern_cleanup() { kwq_cleanup; }

atf_test_case mixbudget cleanup
mixbudget_head() {
	atf_set descr "64 notifiers of 50 us each behind one doorbell are spread over passes by the budget: no overrun, a requeue; not all run in one pass"
	atf_set require.user root
}
mixbudget_body() {
	kwq_setup
	kwq_run mixbudget nnf=64 cost_a=50 items=16 reps=10 cpu=1
	kwq_done mixbudget
	echo "calls=$(R calls) runs=$(R runs) requeues=$(R requeues) wcpu=$(R wcpu)"
	[ "$(R runs)" -eq 640 ] || atf_fail "$(R runs) notifier runs for 640 signals"
	[ "$(R requeues)" -gt 0 ] || atf_fail "64 x 50 us against a 200 us quantum and the handler never requeued: notifiers are not budgeted"
}
mixbudget_cleanup() { kwq_cleanup; }

atf_test_case mixstarve cleanup
mixstarve_head() {
	atf_set descr "a notifier of one quantum that re-arms itself does not starve the item flood on the same queue (SCHED.md S14 case 4): items keep at least a tenth of their solo rate"
	atf_set require.user root
	atf_set timeout 120
}
mixstarve_body() {
	kwq_setup
	kwq_run mixstarve secs=2 cost_us=1 cost_a=200
	kwq_done mixstarve
	echo "without=$(R without) with=$(R with) runs=$(R runs) calls=$(R calls)"
	[ "$(R without)" -gt 1000 ] || atf_fail "flood alone delivered only $(R without) items"
	[ $(( $(R with) * 10 )) -ge "$(R without)" ] || \
	    atf_fail "items fell to $(R with) of $(R without) beside the re-arming notifier"
}
mixstarve_cleanup() { kwq_cleanup; }

atf_test_case mixrearm cleanup
mixrearm_head() {
	atf_set descr "signals landing while the notifier's handler runs yield exactly one further run that sees the final level (no lost wakeup, no duplicate)"
	atf_set require.user root
}
mixrearm_body() {
	kwq_setup
	kwq_run mixrearm reps=20 cost_a=2000 batch=3
	kwq_done mixrearm
	echo "runs=$(R runs) coalesced=$(R coalesced) in=$(R in)"
	atf_check_equal "$(R runs)" 40
}
mixrearm_cleanup() { kwq_cleanup; }

atf_test_case mixcancel cleanup
mixcancel_head() {
	atf_set descr "cancel during a run returns after the handler, the cancelled notifier refuses signals, re-init revives it; and a signal/cancel hammer for 2 s leaves it working"
	atf_set require.user root
	atf_set timeout 120
}
mixcancel_body() {
	kwq_setup
	kwq_run mixcancel reps=10 cost_a=2000 secs=2
	kwq_done mixcancel
	echo "cancels=$(R in) hammer_signals=$(R coalesced) runs=$(R runs)"
	[ "$(R in)" -gt 10 ] || atf_fail "hammer did not run"
}
mixcancel_cleanup() { kwq_cleanup; }

atf_test_case mixrequeue cleanup
mixrequeue_head() {
	atf_set descr "a handler that requeues after every element delivers a list with notifiers among the items exactly once and in arrival order"
	atf_set require.user root
}
mixrequeue_body() {
	kwq_setup
	kwq_run mixrequeue items=30 batch=1 reps=20
	kwq_done mixrequeue
	echo "requeues=$(R requeues) inversions=$(R inversions) exact_errors=$(R exact_errors) runs=$(R runs)"
	atf_check_equal "$(R exact_errors)" 0
	[ "$(R requeues)" -gt 0 ] || atf_fail "the handler never requeued"
	atf_check_equal "$(R runs)" 200
	[ "$(R inversions)" -eq 0 ] || atf_fail "$(R inversions) out-of-order deliveries"
}
mixrequeue_cleanup() { kwq_cleanup; }

atf_test_case mixdrain cleanup
mixdrain_head() {
	atf_set descr "pending notifiers among items at drain time are run (or discarded, on a KWQ_F_DISCARD queue), every notifier is idle afterwards and a later signal is refused"
	atf_set require.user root
}
mixdrain_body() {
	kwq_setup
	kwq_run mixdrain reps=5
	kwq_done mixdrain
	echo "runs=$(R runs) out=$(R out) exact_errors=$(R exact_errors)"
	atf_check_equal "$(R exact_errors)" 0
	kwq_run mixdrain_discard reps=5
	kwq_done mixdrain_discard
	echo "discarded=$(R discarded) exact_errors=$(R exact_errors)"
	atf_check_equal "$(R exact_errors)" 0
	[ "$(R discarded)" -gt 0 ] || atf_fail "nothing was discarded on the KWQ_F_DISCARD queue"
}
mixdrain_cleanup() { kwq_cleanup; }

# ---- SPEC-p4e.md S5: the tick regime and the priority invariant.  Each
# case skips, with the reason, on a build that lacks the knob or counter it
# needs, so the suite stays runnable against every branch.  The tick bound's
# class knob is `lknob' in the kyua configuration (default tick_us, the
# bound L of the tick regime, KWQ.md S16): -v
# test_suites.kwq.lknob=latency_us for a build that spells it that way.
#
# SPEC-p4e.md S5's charge_modes case is not here: this tree has no
# kern.kwq.<class>.charge knob (NET charges wall time, SCHED.md S6.3), so
# there are no modes to sweep.

L_KNOB() { atf_config_get lknob tick_us; }
has_oid() { sysctl -n "$1" >/dev/null 2>&1; }
need_oid() { has_oid "$1" || atf_skip "this build has no $1"; }
W() { sysctl -n "kern.kwq.net.cpu.1.$1" 2>/dev/null || echo 0; }
# the sum of a worker counter over every NET worker
Wsum() { sysctl kern.kwq.net.cpu 2>/dev/null | awk -F'[.:]' -v f="$1" '$6 == f { s += $NF } END { print s + 0 }'; }
# set a class knob for the case and restore it in cleanup (knobs file)
knob() {
	local cur
	cur=$(sysctl -n "$1") || atf_fail "no knob $1"
	echo "$1=$cur" >> knobs
	sysctl -q "$1=$2" >/dev/null || atf_fail "cannot set $1=$2"
}
knobs_restore() {
	[ -f knobs ] || return 0
	while read -r kv; do sysctl -q "$kv" >/dev/null 2>&1; done < knobs
	rm -f knobs
}
# a scenario the build may not have: run it, skip if the module does not know it
kwq_run_or_skip() {
	kwq_run "$@"
	case "$(R msg)" in *nknown*scenario*|*unknown*) atf_skip "this test module has no $1 scenario: $(R msg)" ;; esac
}
timer_intr_w() {	# amd64: the worker CPU's timer interrupts so far; "" elsewhere
	vmstat -i 2>/dev/null | awk '$1 == "cpu1:timer" { print $2; f = 1 } END { if (!f) print "" }'
}

atf_test_case prio_invariant cleanup
prio_invariant_head() {
	atf_set descr "with every moderation knob on and interrupt load on the worker CPU, no pass ever starts below the class priority (prio_violations stays 0) and WITNESS stays quiet"
	atf_set require.user root
}
prio_invariant_body() {
	kwq_setup
	need_oid kern.kwq.net.cpu.1.prio_violations
	# every moderation mechanism this tree has at once: the tick bound, the
	# idle-class wait for a same-CPU producer (its deadline and the priority
	# it waits at, KWQ.md S16 "Below the doorbell"), and the share limit
	knob "kern.kwq.net.$(L_KNOB)" 20
	has_oid kern.kwq.net.wake_delay_us && knob kern.kwq.net.wake_delay_us 20
	has_oid kern.kwq.net.cap_prio && knob kern.kwq.net.cap_prio 224
	has_oid kern.kwq.net.cap_permille && knob kern.kwq.net.cap_permille 500
	local v0 lor0
	v0=$(Wsum prio_violations); lor0=$(dmesg | grep -c "lock order reversal")
	if has_oid kern.kwq_test.intr_cpu; then
		kwq_run ifpair secs=10 items=1024 fanin=2 limit=1000000 intr_cpu=1 intr_us=100 intr_work_ns=20000 intr_secs=10
	else
		echo "no intr_load stressor in this test module: running without interrupt load"
		kwq_run ifpair secs=10 items=1024 fanin=2 limit=1000000
	fi
	kwq_done ifpair
	atf_check_equal "$v0" "$(Wsum prio_violations)"
	[ "$(dmesg | grep -c 'lock order reversal')" -eq "$lor0" ] || atf_fail "WITNESS reported a lock order reversal during the run"
}
prio_invariant_cleanup() { knobs_restore; kwq_cleanup; }

atf_test_case tick_hysteresis cleanup
tick_hysteresis_head() {
	atf_set descr "an isolated item never enters tick mode; a dense burst does and leaves it with at most one empty tick per burst; a silence leaves nothing armed"
	atf_set require.user root
}
tick_hysteresis_body() {
	kwq_setup
	need_oid kern.kwq.net.cpu.1.tick_enters
	knob "kern.kwq.net.$(L_KNOB)" 50
	local e0 t0 t1 b
	e0=$(W tick_enters)
	kwq_run_or_skip burst secs=3 burst_items=1 burst_gap_us=2000 burst_mode=0 cons_ns=500 limit=1000000
	kwq_done burst
	atf_check_equal "$e0" "$(W tick_enters)"
	e0=$(W tick_enters); t0=$(W ticks_empty)
	kwq_run burst secs=3 burst_items=16 burst_gap_us=200 burst_mode=0 cons_ns=500 limit=1000000
	kwq_done burst
	b=$(R bursts 2>/dev/null || echo 1)
	[ "$(W tick_enters)" -gt "$e0" ] || atf_fail "a 16-item burst every 200 us never entered tick mode"
	[ $(( $(W ticks_empty) - t0 )) -le "$b" ] || atf_fail "$(( $(W ticks_empty) - t0 )) empty ticks for $b bursts"
	sleep 0.5
	t0=$(W ticks); sleep 0.5; t1=$(W ticks)
	atf_check_equal "$t0" "$t1"
}
tick_hysteresis_cleanup() { knobs_restore; kwq_cleanup; }

atf_test_case no_arm_when_idle cleanup
no_arm_when_idle_head() {
	atf_set descr "after a burst ends, two seconds of silence cost the worker CPU no more timer interrupts than two idle seconds with nothing queued"
	atf_set require.user root
}
no_arm_when_idle_body() {
	kwq_setup
	need_oid kern.kwq.net.cpu.1.ticks
	[ -n "$(timer_intr_w)" ] || atf_skip "no per-CPU timer interrupt line in vmstat -i on this architecture"
	knob "kern.kwq.net.$(L_KNOB)" 50
	local i0 i1 idle after
	# the idle baseline: modules loaded, nothing queued
	i0=$(timer_intr_w); sleep 2; i1=$(timer_intr_w); idle=$(( i1 - i0 ))
	kwq_run_or_skip burst secs=2 burst_items=64 burst_gap_us=500 burst_mode=0 cons_ns=500 limit=1000000
	kwq_done burst
	sleep 0.1
	i0=$(timer_intr_w); sleep 2; i1=$(timer_intr_w); after=$(( i1 - i0 ))
	echo "timer interrupts on CPU 1 over 2 s: idle $idle, after the burst $after"
	[ "$after" -le $(( idle + idle / 4 + 20 )) ] || atf_fail "the worker CPU took $after timer interrupts in the silence against $idle idle"
}
no_arm_when_idle_cleanup() { knobs_restore; kwq_cleanup; }

atf_init_test_cases() {
	atf_add_test_case lifecycle
	atf_add_test_case fifo
	atf_add_test_case reject
	atf_add_test_case discard
	atf_add_test_case notify
	atf_add_test_case bad_names
	atf_add_test_case class_limit
	atf_add_test_case fairness
	atf_add_test_case fairness_2to1
	atf_add_test_case latency
	atf_add_test_case gaming
	atf_add_test_case overrun
	atf_add_test_case yield
	atf_add_test_case cost
	atf_add_test_case ifpair
	atf_add_test_case mixorder
	atf_add_test_case mixpattern
	atf_add_test_case mixbudget
	atf_add_test_case mixstarve
	atf_add_test_case mixrearm
	atf_add_test_case mixcancel
	atf_add_test_case mixrequeue
	atf_add_test_case mixdrain
	atf_add_test_case prio_invariant
	atf_add_test_case tick_hysteresis
	atf_add_test_case no_arm_when_idle
}
