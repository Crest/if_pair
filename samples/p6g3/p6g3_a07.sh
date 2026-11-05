#!/bin/sh
# p6g3_a07.sh - P6g.3 on a07 (kwq/SPEC-p6g.md S5), with crest's permission
# of 2026-11-05: kern.kwq.bulk.place 0 (today), 1 (gossip), 2 (P8) on one
# boot.  Run on a07 as crest, under sh:
#
#   sh p6g3_a07.sh DIR      # DIR: where bulk_deploy.sh put src/ and stage/
#
# 1. the kwq suite case by case under place 2 and 1 (a07's kyua crashes on
#    a whole suite), kwq-geli's under place 2 (kwq.ko preloaded); a case
#    that does not pass stops the run before the benchmarks;
# 2. kwq_test steal and anyburst, kwq_crypto with 1 and 4 queues fed on
#    CPU 1, under each place, 3 repetitions;
# 3. geli_bench scale (4, 16, 64, 128 providers) on kwqnosteal, kwq,
#    kwqgossip, kwqshard; tput and resp on kwq, kwqgossip, kwqshard; ssd
#    (16 gnop SSD models) on nop, eli, kwq, kwqgossip, kwqshard; 3 reps.
# Uptime before and after; every module, md and knob through the settings
# log; geom_eli, hwpmc and dtrace stay loaded, if_pair.ko is crest's.
D=${1:?DIR}
L=$HOME/if_pair-scratch/settings_a07.log; O=$D/out; S=$D/stage
export KYUA_STOREDIR=$HOME/if_pair-scratch/kyua-store
mkdir -p "$KYUA_STOREDIR" "$O"; rm -f "$D/p6g3_a07.done"
log() { echo "$(date -u +%FT%TZ) p6g3: $*" | doas -n tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do
	kldstat -q -m $m && { log "$m loaded before the run: abort"; exit 1; }
done
[ -z "$(doas -n mdconfig -l)" ] || { log "md devices exist before the run: abort"; exit 1; }
quiet() {
	n=0
	while doas -n pgrep -f '_test\.sh' >/dev/null && [ $n -lt 120 ]; do
		sleep 1; n=$((n + 1))
	done
}
kcase() {	# PROGRAM:CASE CONFIG...
	c=$1; shift
	r=$(doas -n env KYUA_STOREDIR="$KYUA_STOREDIR" kyua $(for v; do echo "-v $v"; done) debug -k Kyuafile "$c" 2>&1 | tail -1)
	case $r in *kyua-discuss*)
		quiet
		r="$(doas -n env KYUA_STOREDIR="$KYUA_STOREDIR" kyua $(for v; do echo "-v $v"; done) debug -k Kyuafile "$c" 2>&1 | tail -1) (again after a kyua crash)" ;;
	esac
	echo "$c: $r"
	quiet
}
cd "$D/src/kwq/tests" || exit 1
for P in 2 1; do
	for c in $(sed -n 's/^[[:space:]]*atf_add_test_case //p' kwq_test.sh); do
		kcase kwq_test.sh:$c test_suites.kwq.moddir="$S" test_suites.kwq.place=$P
	done > "$O/atf_kwq_place$P.txt"
	for c in verify onecpu_fair weight; do
		kcase kwq_crypto_test.sh:$c test_suites.kwq.moddir="$S" test_suites.kwq.place=$P
	done >> "$O/atf_kwq_place$P.txt"
	t=$O/atf_kwq_place$P.txt
	log "kwq suite place=$P: $(grep -c ' passed' "$t") passed, $(grep -c ' skipped' "$t") skipped, of $(wc -l < "$t" | tr -d ' '), $(grep -c 'again after' "$t") after a kyua crash"
done
doas -n kenv kern.kwq.bulk.place=2 >/dev/null && doas -n kldload $S/kwq.ko || { log "cannot preload kwq.ko"; exit 1; }
log "kenv kern.kwq.bulk.place=2, loaded $S/kwq.ko for kwq-geli's suite"
cd "$D/src/kwq-geli/tests" || exit 1
for c in $(sed -n 's/^[[:space:]]*atf_add_test_case //p' kwqeli_test.sh); do
	kcase kwqeli_test.sh:$c test_suites.kwqeli.moddir="$S"
done > "$O/atf_geli_place2.txt"
doas -n kldunload kwq; doas -n kenv -u kern.kwq.bulk.place
log "unloaded kwq, kenv -u kern.kwq.bulk.place; kwq-geli suite place=2: $(grep -c ' passed' "$O/atf_geli_place2.txt") passed of $(wc -l < "$O/atf_geli_place2.txt" | tr -d ' ')"
for m in kwq kwq_test kwq_crypto g_kwqeli; do kldstat -q -m $m && { log "$m left loaded: abort"; exit 1; }; done
# no_arm_when_idle skips on arm64 (no per-CPU timer line in vmstat -i)
grep -vE ' -> +(passed|skipped)' "$O"/atf_*.txt && { log "a case did not pass: stop before the benchmarks"; exit 1; }

doas -n kldload $S/kwq.ko && doas -n kldload $S/kwq_test.ko && doas -n kldload $S/kwq_crypto.ko || { log "cannot load kwq"; exit 1; }
log "loaded kwq.ko kwq_test.ko kwq_crypto.ko ($S)"
T=$O/kwqtest_a07.tsv; : > $T; K=$O/crypto_a07.tsv; : > $K
r() { sysctl -n kern.kwq_test.result_$1; }
for rep in 1 2 3; do
	for P in 0 1 2; do
		doas -n sysctl -q kern.kwq.bulk.place=$P >/dev/null
		for sc in steal anyburst; do
			c=1; [ $sc = anyburst ] && c=0
			doas -n sysctl -q kern.kwq_test.scenario=$sc kern.kwq_test.cpu=$c \
			    kern.kwq_test.items=20000 kern.kwq_test.cost_us=20 kern.kwq_test.limit=100000 >/dev/null
			doas -n sysctl -q kern.kwq_test.run=1 >/dev/null
			while [ "$(r state)" = running ]; do sleep 0.2; done
			printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $sc $P $rep "$(r state)" "$(r ns)" \
			    "$(r offcpu)" "$(r cpus)" "$(r dups)" >> $T
		done
		for nq in 1 4; do
			doas -n sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=$nq kern.kwq_crypto.depth=16 \
			    kern.kwq_crypto.cpu=1 kern.kwq_crypto.stealable=1 kern.kwq_crypto.secs=3 \
			    kern.kwq_crypto.verify=0 kern.kwq_crypto.bytes=65536 >/dev/null
			doas -n sysctl -q kern.kwq_crypto.run=1 >/dev/null
			while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.2; done
			printf '%s\t%s\t%s\t%s\n' $nq $P $rep "$(dmesg | grep 'kwq_crypto: kwq nq' | tail -1)" >> $K
		done
	done
done
doas -n sysctl -q kern.kwq.bulk.place=0 >/dev/null
doas -n kldunload kwq_crypto; doas -n kldunload kwq_test; doas -n kldunload kwq
log "kwq_test and kwq_crypto done ($(wc -l < $T) + $(wc -l < $K) rows), unloaded"

B="$D/src/kwq-geli/tests/geli_bench.sh"
doas -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_a07.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS="4 16 64 128" SCALE_MDSIZE=32 TESTS=scale \
    sh "$B" kwqnosteal kwq kwqgossip kwqshard > "$O/scale_a07.log" 2>&1
log "geli_bench scale exit $?"
doas -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/tput_a07.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 TESTS="tput resp" \
    sh "$B" kwq kwqgossip kwqshard > "$O/tput_a07.log" 2>&1
log "geli_bench tput resp exit $?"
doas -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/ssd_a07.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 TESTS=ssd \
    sh "$B" nop eli kwq kwqgossip kwqshard > "$O/ssd_a07.log" 2>&1
log "geli_bench ssd exit $?"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(doas -n mdconfig -l | wc -w), uptime $(uptime)"
touch "$D/p6g3_a07.done"
