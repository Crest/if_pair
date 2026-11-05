#!/bin/sh
# suite.sh - placesim's validation (SPEC-p6g.md S3.6): V1 queueing
# theory, V2 conservation (every run), V3 lab's 128-provider cases, V4
# P6g.0's sibling order.  Exit status 0 when every check is within its
# tolerance.  Run from this directory after `make'.
P=./placesim
fail=0
get() {	# METRIC: the value from the last run's output in $OUT
	echo "$OUT" | awk -F'\t' -v m="$1" '$5 == m { print $6 }'
}
check() {	# NAME VALUE LO HI
	if awk -v v="$2" -v lo="$3" -v hi="$4" 'BEGIN { exit !(v >= lo && v <= hi) }'; then
		printf "ok   %-46s %s in [%s, %s]\n" "$1" "$2" "$3" "$4"
	else
		printf "FAIL %-46s %s not in [%s, %s]\n" "$1" "$2" "$3" "$4"
		fail=1
	fi
}
run() {	# NAME KEY=VALUE...: run, check V2 (no violations)
	n=$1; shift
	OUT=$($P "$@") || { echo "FAIL $n: exit"; fail=1; }
	check "$n: violations" "$(get violations)" 0 0
}
Z="c_hit=0 c_sib=0 c_l3=0 c_atomic=0 scan_entry=0 place_ns=0 merge_ns=0 wl_short_us=0 wl_long_us=0 wl_deep_us=0 smt_bulk=1 smt_other=1 expcost=1 small_us=10 csmall_us=1 topo=guest scen=uniform dur_ms=1500 warm_ms=20"
# V1: the oracle with FCFS CPUs is M/M/c (Erlang C), random placement M/M/1.
run "V1 oracle rho 0.8" $Z policy=0 oneq=1 rho=0.8
check "V1 oracle rho 0.8 Wq (Erlang C 7.455 us)" "$(get wait_all_mean_us)" 7.0 7.9
run "V1 oracle rho 0.9" $Z policy=0 oneq=1 rho=0.9
check "V1 oracle rho 0.9 Wq (Erlang C 19.69 us)" "$(get wait_all_mean_us)" 18.5 20.9
run "V1 random rho 0.8" $Z policy=6 rho=0.8
check "V1 random rho 0.8 Wq (M/M/1 40 us)" "$(get wait_all_mean_us)" 37.5 42.5
# V3: lab, 4 KiB reads, 128 providers, one in flight each.
G="topo=lab scen=geli inflight=1 small_us=1.1 other_us=4.6 dur_ms=150 warm_ms=30"
run "V3 128 no stealing" $G nprov=128 policy=2
check "V3 128 no stealing IOPS (lab 1.58M)" "$(get ops_per_s)" 1.45e6 1.75e6
run "V3 128 stealing" $G nprov=128 policy=1
check "V3 128 stealing IOPS (lab 0.55M)" "$(get ops_per_s)" 0.48e6 0.63e6
check "V3 128 stealing tries/op (lab 1.05)" "$(get steal_tries_per_op)" 0.9 1.15
run "V3 128 steal_scan 2" $G nprov=128 policy=1 steal_scan=2
check "V3 128 steal_scan 2 IOPS (lab 1.35M)" "$(get ops_per_s)" 1.15e6 1.5e6
run "V3 16 of 128 stealing" $G nprov=16 extraq=112 policy=1
check "V3 16 of 128 stealing IOPS (lab 0.56M)" "$(get ops_per_s)" 0.45e6 0.65e6
# V4: P6g.0, kwq_crypto 2 queues x 4 requests of 1 MiB on lab.
V="topo=lab scen=crypto small_us=191 nprov=2 inflight=4 dur_ms=300 warm_ms=50"
run "V4 today's order" $V policy=1
a=$(get ops_per_s)
check "V4 today's order GB/s (lab 41.9)" "$(echo "$a" | awk '{ print $1 * 1.048576 / 1000 }')" 39.8 44
run "V4 sibling last" $V policy=1 sib_last=1
b=$(get ops_per_s)
check "V4 sibling last / today (lab 0.80)" "$(echo "$a $b" | awk '{ print $2 / $1 }')" 0.74 0.86
run "V4 gossip" $V policy=5
check "V4 gossip / today (no worse)" "$(echo "$a $(get ops_per_s)" | awk '{ print $2 / $1 }')" 0.98 1.2
exit $fail
