#!/bin/sh
# home_sweep.sh lab|a07 DIR: what decides reads beside writes on home (2
# SATA-like drives)?  The writers' queue depth swept for no encryption, stock,
# without write_hold (def) and with it, three repetitions, the order rotating.  All on
# geom_simdisk.  One GELI at a time, stock with kwq.ko not loaded.
# Output: DIR/${OUT:-sweep}/<stage>/q<QD>_r<rep>.tsv
# Environment: OUT, QDS (2 3 4 8), STAGES (nop stock def; also hold), EXTRA
# (bench options for every run).
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/${OUT:-sweep}; rm -f $D/sweep.done
: "${QDS:=2 3 4 8}" "${STAGES:=nop stock def}"
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) p6g11s: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) p6g11s: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
log "start, uptime $(uptime), processes $(ps -ax | wc -l)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop g_simdisk if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
[ -z "$($SU geli list 2>/dev/null | grep '^Geom name: \(md\|sd\)')" ] || { log "geli providers on test devices exist: abort"; exit 1; }
B=$D/src/kwq-geli/tests/geli_bench.sh
E="MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1 MD_TYPE=sim PROBE_S=3"
H="TESTS=dev DEV_N=2 DEV_FAST=0 DEV_W=450 DEV_R=520 DEV_LAT=100"
one() {		# GROUP STAGE QD REP
	mkdir -p $O/$1
	$SU env $E OUT_TSV=$O/$1/q$3_r$4.tsv $H DEV_QD=$3 $EXTRA sh $B $2 > $O/$1/q$3_r$4.log 2>&1
	e=$?
	[ $e = 0 ] || { log "$1 q$3 rep $4: exit $e: abort"; return 1; }
}
rot() {		# N WORD...: the words rotated by N
	n=$1; shift
	while [ "$n" -gt 0 ]; do w=$1; shift; set -- "$@" "$w"; n=$((n - 1)); done
	echo "$*"
}
stage() {	# nop|stock|def|hold REP
	case $1 in
	nop)	for q in $(rot $(($2 - 1)) $QDS); do one nop nop $q $2 || return 1; done ;;
	stock)	kldstat -q -m kwq && { log "kwq loaded before stock: abort"; return 1; }
		for q in $(rot $(($2 - 1)) $QDS); do one stock eli $q $2 || return 1; done ;;
	def|hold) $SU kldload $S/kwq.ko || { log "cannot load kwq"; return 1; }
		log "loaded kwq.ko"
		e=0; b=kwqhold2; [ $1 = hold ] || b=kwqhold0
		for q in $(rot $(($2 - 1)) $QDS); do one $1 $b $q $2 || { e=1; break; }; done
		$SU kldunload kwq || { log "kldunload kwq failed: abort"; return 1; }
		kldstat -q -m kwq && { log "kwq still loaded: abort"; return 1; }
		log "unloaded kwq"
		return $e ;;
	esac
}
r=1
while [ $r -le 3 ]; do
	for s in $(rot $((r - 1)) $STAGES); do
		stage $s $r || break 2
	done
	log "rep $r done"
	r=$((r + 1))
done
log "end after rep $r, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|simdisk'), md $($SU mdconfig -l | wc -w), processes $(ps -ax | wc -l), uptime $(uptime)"
touch $D/sweep.done
