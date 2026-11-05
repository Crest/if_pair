#!/bin/sh
# g2suite.sh - after gate G2 (SPEC-p6g.md D12-D15) on lab or a07: the kwq
# suite case by case under the module's defaults (no place set: P8 with
# the chain on one NUMA domain) and under place=0, kwq-geli's suite with
# kwq.ko preloaded at its defaults.  sh g2suite.sh lab|a07 DIR
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/g2suite.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) g2suite: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) g2suite: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
export KYUA_STOREDIR=$HOME/if_pair-scratch/kyua-store; mkdir -p "$KYUA_STOREDIR"
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
quiet() {
	n=0
	while $SU pgrep -f '_test\.sh' >/dev/null && [ $n -lt 120 ]; do sleep 1; n=$((n + 1)); done
}
kcase() {	# PROGRAM:CASE CONFIG...
	c=$1; shift
	r=$($SU env KYUA_STOREDIR="$KYUA_STOREDIR" kyua $(for v; do echo "-v $v"; done) debug -k Kyuafile "$c" 2>&1 | tail -1)
	case $r in *kyua-discuss*)
		quiet
		r="$($SU env KYUA_STOREDIR="$KYUA_STOREDIR" kyua $(for v; do echo "-v $v"; done) debug -k Kyuafile "$c" 2>&1 | tail -1) (again after a kyua crash)" ;;
	esac
	echo "$c: $r"
	quiet
}
cd "$D/src/kwq/tests" || exit 1
for P in default 0; do
	pv=; [ $P = default ] || pv=test_suites.kwq.place=$P
	t=$O/atf_kwq_$P.txt
	for c in $(sed -n 's/^[[:space:]]*atf_add_test_case //p' kwq_test.sh); do
		kcase kwq_test.sh:$c test_suites.kwq.moddir="$S" $pv
	done > $t
	for c in verify onecpu_fair weight; do
		kcase kwq_crypto_test.sh:$c test_suites.kwq.moddir="$S" $pv
	done >> $t
	log "kwq suite place=$P: $(grep -c ' passed' $t) passed, $(grep -c ' skipped' $t) skipped, of $(wc -l < $t | tr -d ' '), $(grep -c 'again after' $t) after a kyua crash"
done
$SU kldload $S/kwq.ko || { log "cannot preload kwq.ko"; exit 1; }
log "loaded $S/kwq.ko at its defaults: place=$(sysctl -n kern.kwq.bulk.place) directed=$(sysctl -n kern.kwq.bulk.shard.directed) directed_us=$(sysctl -n kern.kwq.bulk.shard.directed_us)"
cd "$D/src/kwq-geli/tests" || exit 1
t=$O/atf_geli_default.txt
for c in $(sed -n 's/^[[:space:]]*atf_add_test_case //p' kwqeli_test.sh); do
	kcase kwqeli_test.sh:$c test_suites.kwqeli.moddir="$S"
done > $t
$SU kldunload kwq
log "unloaded kwq; kwq-geli suite at the defaults: $(grep -c ' passed' $t) passed, $(grep -c ' skipped' $t) skipped, of $(wc -l < $t | tr -d ' ')"
grep -vhE ' -> +(passed|skipped)' $O/atf_*.txt | head -5
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $($SU mdconfig -l | wc -w), uptime $(uptime)"
touch $D/g2suite.done
