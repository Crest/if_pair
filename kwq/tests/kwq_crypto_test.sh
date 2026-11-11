#! /usr/libexec/atf-sh
# ATF test program for kwq_crypto.ko, the dummy BULK client (../PLAN.txt
# P6d): AES-XTS through crypto(9) on BULK queues, checked for correctness
# (every other pass decrypts and compares) and for the shares several
# busy queues on one CPU get.  Run under kyua as root with at least 4
# CPUs; loads kwq.ko and kwq_crypto.ko from `moddir' as kwq_test.sh does.

kc_mod() {
	local moddir
	moddir=$(atf_config_get moddir "")
	if [ -z "$moddir" ]; then
		echo "$1"
	elif [ -f "$moddir/kwq/kwq.ko" ]; then
		echo "$moddir/kwq/$1.ko" | sed "s|/kwq/kwq_crypto.ko|/kwq_crypto/kwq_crypto.ko|"
	else
		echo "$moddir/$1.ko"
	fi
}

# The config variable place sets kern.kwq.bulk.place for the load, as in
# kwq_test.sh.
kc_setup() {
	[ "$(id -u)" -eq 0 ] || atf_skip "needs root"
	[ "$(sysctl -n hw.ncpu)" -ge 4 ] || atf_skip "needs at least 4 CPUs"
	: > loaded
	local place
	place=$(atf_config_get place "")
	if [ -n "$place" ] && ! kldstat -q -m kwq; then
		kenv kern.kwq.bulk.place=$place >/dev/null
		echo kern.kwq.bulk.place >> kenvs
	fi
	for m in kwq kwq_crypto; do
		if ! kldstat -q -m $m; then
			kldload "$(kc_mod $m)" || atf_skip "cannot load $(kc_mod $m)"
			echo $m >> loaded
		fi
	done
	[ "$(sysctl -n kern.kwq_crypto.result_state)" != running ] || \
	    atf_skip "another kwq_crypto run is in progress"
	sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=1 \
	    kern.kwq_crypto.bytes=65536 kern.kwq_crypto.sector=4096 \
	    kern.kwq_crypto.chunk=16384 kern.kwq_crypto.depth=16 \
	    kern.kwq_crypto.secs=3 kern.kwq_crypto.cpu=-1 \
	    kern.kwq_crypto.stealable=0 kern.kwq_crypto.threads=0 \
	    kern.kwq_crypto.prio=56 kern.kwq_crypto.weight0=1 \
	    kern.kwq_crypto.verify=0 kern.kwq_crypto.limit=0 >/dev/null
}

kc_cleanup() {
	local m n
	if [ -f kenvs ]; then
		while read -r m; do kenv -u "$m" >/dev/null 2>&1 || :; done < kenvs
		rm -f kenvs
	fi
	[ -f loaded ] || return 0
	n=0
	while [ "$(sysctl -n kern.kwq_crypto.result_state 2>/dev/null)" = running ] && \
	    [ $n -lt 300 ]; do sleep 0.2; n=$((n + 1)); done
	for m in kwq_crypto kwq; do
		grep -qx $m loaded || continue
		n=0
		while kldstat -q -m $m && ! kldunload $m 2>/dev/null; do
			sleep 0.1; n=$((n + 1))
			[ $n -lt 100 ] || { echo "cannot unload $m" >&2; break; }
		done
	done
	return 0
}

C() { sysctl -n "kern.kwq_crypto.result_$1"; }

# kc_run knob=value ...: set the knobs, run, wait, print the result
kc_run() {
	local kv n
	for kv in "$@"; do
		sysctl -q "kern.kwq_crypto.$kv" >/dev/null || atf_fail "sysctl kern.kwq_crypto.$kv"
	done
	sysctl -q kern.kwq_crypto.run=1 >/dev/null || atf_fail "cannot start"
	n=0
	while [ "$(C state)" = running ]; do
		sleep 0.2; n=$((n + 1))
		[ $n -lt 600 ] || atf_fail "no result after 120 s"
	done
	echo "$*: $(C state) $(C msg) mbps_x100=$(C mbps_x100) p50=$(C lat_p50_us) p99=$(C lat_p99_us) jain=$(C jain) cpus=$(C cpus) errors=$(C errors) driver=$(C driver) [$(C shares)]"
	[ "$(C state)" = done ] || atf_fail "$(C state): $(C msg)"
}

atf_test_case verify cleanup
verify_head() {
	atf_set descr "every mode (kwq, kwq stealable, scatter, thread) encrypts and decrypts back to the pattern"
	atf_set require.user root
}
verify_body() {
	kc_setup
	kc_run mode=kwq verify=1
	atf_check_equal 0 "$(C errors)"
	kc_run mode=kwq verify=1 stealable=1 cpu=1
	atf_check_equal 0 "$(C errors)"
	# Today's steal takes a list of two (SCHED.md S16); under
	# kern.kwq.bulk.place 1 and 2 a backlog below the hand-off's H or a
	# donor's backlog stays on its CPU (lab: 16 in flight of AES-NI)
	if [ "$(sysctl -n kern.kwq.bulk.place 2>/dev/null || echo 0)" = 0 ]; then
		[ "$(C cpus)" -ge 2 ] || atf_fail "stealable queue fed on one CPU ran on $(C cpus)"
	else
		echo "  place=$(sysctl -n kern.kwq.bulk.place): ran on $(C cpus) CPU(s)"
	fi
	kc_run mode=scatter verify=1 bytes=262144 chunk=16384 depth=4 cpu=-1 stealable=0
	atf_check_equal 0 "$(C errors)"
	kc_run mode=thread verify=1 bytes=65536 depth=16
	atf_check_equal 0 "$(C errors)"
}
verify_cleanup() { kc_cleanup; }

atf_test_case onecpu_fair cleanup
onecpu_fair_head() {
	atf_set descr "four equal BULK queues kept busy on one CPU share it evenly (Jain >= 0.98)"
	atf_set require.user root
}
onecpu_fair_body() {
	kc_setup
	for d in 8 32; do
		kc_run mode=kwq nq=4 depth=$d cpu=1 stealable=0
		[ "$(C jain)" -ge 980 ] || atf_fail "depth $d: Jain $(C jain)/1000 [$(C shares)]"
		atf_check_equal 1 "$(C cpus)"
	done
}
onecpu_fair_cleanup() { kc_cleanup; }

atf_test_case weight cleanup
weight_head() {
	atf_set descr "weight 3 against weight 1 on one CPU: queue 0 gets about three times queue 1's throughput"
	atf_set require.user root
}
weight_body() {
	kc_setup
	# Weights act only on queues backlogged through their pass: a pass
	# serves the requests queued when it began (../SCHED.md S4.3), so
	# queue 0's depth has to hold more than its three quanta of work.
	# No fixed depth does on every CPU: 32 requests of 256 KiB are
	# 4.4 ms of AES-XTS on a07 but 1.5 ms on lab (5.5 GB/s on one core),
	# where queue 0 ran dry every round and the ratio was 1.52.  So the
	# CPU's rate is measured first and the depth holds six quanta.
	kc_run mode=kwq nq=1 depth=32 bytes=262144 cpu=1 stealable=0 secs=1
	q=$(sysctl -n kern.kwq.bulk.quantum_us)
	d=$(echo "$(C mbps_x100) $q" | awk '{ d = int(6 * $2 * $1 / 100 / 262144) + 1; print(d < 32 ? 32 : d) }')
	echo "quantum $q us, depth $d"
	[ "$d" -le 1024 ] || atf_skip "six quanta of AES-XTS on one CPU exceed 1024 requests of 256 KiB"
	kc_run mode=kwq nq=2 depth=$d bytes=262144 cpu=1 stealable=0 weight0=3 secs=3
	r=$(C shares | awk '{ split($1, a, ":"); split($2, b, ":"); printf("%d", b[2] > 0 ? a[2] * 100 / b[2] : 0) }')
	echo "ratio x100: $r"
	[ "$r" -ge 240 ] && [ "$r" -le 360 ] || atf_fail "weight 3:1 gave a ratio of $r/100"
}
weight_cleanup() { kc_cleanup; }

atf_init_test_cases() {
	atf_add_test_case verify
	atf_add_test_case onecpu_fair
	atf_add_test_case weight
}
