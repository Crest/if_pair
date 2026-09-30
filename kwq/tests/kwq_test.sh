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
# properties asserted: ../SCHED.md S10 and ../KWQ.md S10.8.  The "sleep"
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
	kwq_setup
	kwq_run gaming secs=3
	kwq_done gaming
	[ "$(R cycles_b)" -le $(( $(R cycles_a) + $(R cycles_a) / 50 )) ] || \
	    atf_fail "gaming queue got more CPU than the flood: $(R cycles_b) vs $(R cycles_a)"
	[ "$(Q ktb.cpu.1.boosts)" -le 2 ] || atf_fail "gaming queue collected $(Q ktb.cpu.1.boosts) boosts"
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

atf_init_test_cases() {
	atf_add_test_case lifecycle
	atf_add_test_case fifo
	atf_add_test_case reject
	atf_add_test_case discard
	atf_add_test_case notify
	atf_add_test_case bad_names
	atf_add_test_case fairness
	atf_add_test_case fairness_2to1
	atf_add_test_case latency
	atf_add_test_case gaming
	atf_add_test_case overrun
	atf_add_test_case yield
	atf_add_test_case cost
	atf_add_test_case ifpair
}
