#!/bin/sh
# split.sh lab|a07 DIR: where does write_hold's gain on storage come from?  The
# storage model's drives apart and together, reads beside writes, without
# probes, write_hold 0 and 1 alternating.  One GELI at a time, geom_simdisk
# only.  Rows of DIR/${OUT:-split}/<rig>_<stage>_r<rep>.tsv
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/${OUT:-split}; mkdir -p $O; rm -f $D/split.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) split: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) split: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
for m in kwq g_kwqeli g_nop g_simdisk if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
log "start, uptime $(uptime)"
B=$D/src/kwq-geli/tests/geli_bench.sh
E="MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1 MD_TYPE=sim PROBE_S=3 IO_QUIET=1 TESTS=dev DEV_FW=3000 DEV_FR=5000 DEV_FLAT=20 DEV_W=200 DEV_R=200 DEV_LAT=100 DEV_SEEK=8000 DEV_QD=2 DEV_RQD=2"
$SU kldload $S/kwq.ko || exit 1
for r in 1 2 3; do
	for rig in fast hdd mix; do
		case $rig in
		fast) G="DEV_N=8 DEV_FAST=8" ;;
		hdd) G="DEV_N=90 DEV_FAST=0" ;;
		mix) G="DEV_N=98 DEV_FAST=8" ;;
		esac
		for st in kwqhold0 kwqhold1; do
			$SU env $E $G $EXTRA OUT_TSV=$O/${rig}_${st}_r$r.tsv sh $B $st > $O/${rig}_${st}_r$r.log 2>&1 || log "$rig $st r$r failed"
			log "$rig $st r$r: both $(awk -F'\t' '$4 == "both_w_mbps" { w = $5 } $4 == "both_r_mbps" { r = $5 } END { print w "/" r }' $O/${rig}_${st}_r$r.tsv)"
		done
	done
done
$SU kldunload kwq
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|simdisk'), uptime $(uptime)"
touch $D/split.done
