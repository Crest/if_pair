#!/bin/sh
# suites.sh - before steps.sh on lab or a07: the kwq suite case by case with
# BULK at priority 55, kwq-geli's suite with kwq.ko preloaded at the four
# setups (base, bio: budget_sectors 0, prio: priority 55, both), tunables by
# kenv, unset after.  sh suites.sh lab|a07 DIR
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out/suites; mkdir -p $O; rm -f $D/suites.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) suites: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) suites: $*" | doas -n tee -a "$L"; } ;;
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
tun() {	# BUDGET PRIORITY: the tunables of one setup
	[ $1 = default ] || { $SU kenv kern.geom.kwqeli.budget_sectors=$1 >/dev/null; log "set kenv kern.geom.kwqeli.budget_sectors=$1"; }
	[ $2 = default ] || { $SU kenv kern.kwq.bulk.priority=$2 >/dev/null; log "set kenv kern.kwq.bulk.priority=$2"; }
}
untun() {
	for k in kern.geom.kwqeli.budget_sectors kern.kwq.bulk.priority; do
		[ -z "$(kenv -q $k 2>/dev/null)" ] || { $SU kenv -u $k; log "unset kenv $k"; }
	done
}
tally() { echo "$(grep -c ' passed' $1) passed, $(grep -c ' skipped' $1) skipped, of $(wc -l < $1 | tr -d ' '), $(grep -c 'again after' $1) after a kyua crash"; }
cd "$D/src/kwq/tests" || exit 1
tun default 55
t=$O/atf_kwq_prio.txt
for c in $(sed -n 's/^[[:space:]]*atf_add_test_case //p' kwq_test.sh); do
	kcase kwq_test.sh:$c test_suites.kwq.moddir="$S"
done > $t
for c in verify onecpu_fair weight; do
	kcase kwq_crypto_test.sh:$c test_suites.kwq.moddir="$S"
done >> $t
untun
log "kwq suite at priority 55: $(tally $t)"
cd "$D/src/kwq-geli/tests" || exit 1
for cfg in base:default:default bio:0:default prio:default:55 both:0:55; do
	s=${cfg%%:*}; b=${cfg#*:}; p=${b#*:}; b=${b%:*}
	tun $b $p
	$SU kldload $S/kwq.ko || { log "cannot preload kwq.ko"; untun; exit 1; }
	log "loaded kwq.ko for $s: priority=$(sysctl -n kern.kwq.bulk.priority)"
	t=$O/atf_geli_$s.txt
	for c in $(sed -n 's/^[[:space:]]*atf_add_test_case //p' kwqeli_test.sh); do
		kcase kwqeli_test.sh:$c test_suites.kwqeli.moddir="$S"
	done > $t
	$SU kldunload kwq
	untun
	log "unloaded kwq; kwq-geli suite, $s: $(tally $t)"
done
grep -vhE ': +(passed|skipped)|-> +(passed|skipped)' $O/atf_*.txt | head -8
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $($SU mdconfig -l | wc -w), uptime $(uptime)"
touch $D/suites.done
