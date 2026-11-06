#!/bin/sh
# steps.sh - BULK.md S7.6's two steps against stock GELI, on lab or a07:
#   stock  stock GELI, kwq.ko not loaded
#   base   kwq-geli as it is (budget_sectors 16, BULK at PI_SOFT)
#   bio    budget_sectors 0: the budget checked between bios only
#   prio   kern.kwq.bulk.priority=55: BULK below md(4)
#   both   the two together
# One setup at a time, in that order within each of 3 repetitions, so that
# drift falls on all of them alike; only one GELI runs at any time.  Per
# setup and repetition: scale (lab 4 16 32 64 128, a07 4 16 64 128
# providers), tput resp onecpu, tput at 32 KiB sectors, on a07 also ssd (16
# SSD models, three loads; nop with stock).  The priority is a load tunable,
# so kwq.ko is loaded per priority; geom_eli stays loaded (swap).
# Output: DIR/out/steps/{stock,p2,p55}/<battery>_r<rep>.tsv
# sh steps.sh lab|a07 DIR
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out/steps; rm -f $D/steps.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log; NS="4 16 32 64 128"
	log() { echo "$(date -u +%FT%TZ) steps: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log; NS="4 16 64 128"
	log() { echo "$(date -u +%FT%TZ) steps: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
mkdir -p $O/stock $O/p2 $O/p55
log "start, uptime $(uptime), processes $(ps -ax | wc -l)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
[ -z "$($SU geli list 2>/dev/null | grep '^Geom name: md')" ] || { log "geli providers on md exist: abort"; exit 1; }
[ -z "$(kenv -q kern.kwq.bulk.priority 2>/dev/null)" ] || { log "kern.kwq.bulk.priority set: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
bench() {	# GROUP REP STAGES SSD-STAGES
	o=$O/$1; r=$2
	$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$o/scale_r$r.tsv" SETTINGS_LOG="$L" \
	    REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="$NS" SCALE_MDSIZE=32 TESTS=scale \
	    sh "$B" $3 > "$o/scale_r$r.log" 2>&1
	e1=$?
	$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$o/tput_r$r.tsv" SETTINGS_LOG="$L" \
	    REPS=1 PROBE_S=3 TESTS="tput resp onecpu" sh "$B" $3 > "$o/tput_r$r.log" 2>&1
	e2=$?
	$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$o/tput32k_r$r.tsv" SETTINGS_LOG="$L" \
	    REPS=1 ELI_SECSIZE=32768 TESTS=tput sh "$B" $3 > "$o/tput32k_r$r.log" 2>&1
	e3=$?
	e4=0
	if [ $M = a07 ]; then
		$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$o/ssd_r$r.tsv" SETTINGS_LOG="$L" \
		    REPS=1 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 TESTS=ssd \
		    sh "$B" $4 > "$o/ssd_r$r.log" 2>&1
		e4=$?
	fi
	log "rep $r $1 ($3): exit $e1 $e2 $e3 $e4, processes $(ps -ax | wc -l)"
	[ "$e1$e2$e3$e4" = 0000 ] || { log "a battery failed: abort"; return 1; }
}
kwqload() {	# PRIORITY|default
	[ $1 = default ] || { $SU kenv kern.kwq.bulk.priority=$1 >/dev/null; log "set kenv kern.kwq.bulk.priority=$1"; }
	$SU kldload $S/kwq.ko || { log "cannot load kwq"; return 1; }
	log "loaded kwq.ko: priority=$(sysctl -n kern.kwq.bulk.priority) place=$(sysctl -n kern.kwq.bulk.place) quantum_us=$(sysctl -n kern.kwq.bulk.quantum_us) cap_period_us=$(sysctl -n kern.kwq.bulk.cap_period_us) cap_permille=$(sysctl -n kern.kwq.bulk.cap_permille)"
}
kwqunload() {	# PRIORITY|default
	$SU kldunload kwq; log "unloaded kwq"
	[ $1 = default ] || { $SU kenv -u kern.kwq.bulk.priority; log "unset kenv kern.kwq.bulk.priority"; }
}
r=1
while [ $r -le 3 ]; do
	bench stock $r eli "nop eli" || break
	kwqload default || break
	bench p2 $r "kwqshard kwqbio" "kwqshard kwqbio"; e=$?
	kwqunload default
	[ $e = 0 ] || break
	kwqload 55 || break
	bench p55 $r "kwqshard kwqbio" "kwqshard kwqbio"; e=$?
	kwqunload 55
	[ $e = 0 ] || break
	r=$((r + 1))
done
log "end after rep $r, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $($SU mdconfig -l | wc -w), geli on md $($SU geli list 2>/dev/null | grep -c '^Geom name: md'), kenv $(kenv -q kern.kwq.bulk.priority 2>/dev/null || echo unset), processes $(ps -ax | wc -l), uptime $(uptime)"
touch $D/steps.done
