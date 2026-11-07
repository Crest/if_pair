#!/bin/sh
# battery.sh lab|a07 DIR: every candidate, stock GELI included, interleaved
# within each of 3 repetitions on dummy-data drives (md -t null: no RAM, no
# md(4) copy, real crypto), scored by score.py against the best per test:
#   stock   stock GELI, kwq.ko not loaded
#   p2      kwq.ko at BULK's default priority: today (kwqshard), whole bios
#           (kwqbio), the time rule (kwqtime)
#   p55     BULK below md(4): whole bios, the time rule
# One GELI at a time.  Per setup and repetition: scale, tput resp, tput at 32
# KiB sectors, ssd (16 SSD models), and the drives of the machine it stands in
# for: lab the storage server (98 providers: 8 without a delay, 90 of about
# 200 MB/s) and the home server (2 of about 450 MB/s), a07 the compute server
# (4 drives, every CPU busy).  nop runs the device models once per repetition.
# Output: DIR/out/{stock,p2,p55,nop}/<test>_r<rep>.tsv.  sh battery.sh lab|a07 DIR
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out; rm -f $D/battery.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log; NS="4 16 32 64 128"
	log() { echo "$(date -u +%FT%TZ) p6g11: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log; NS="4 16 64 128"
	log() { echo "$(date -u +%FT%TZ) p6g11: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
mkdir -p $O/stock $O/p2 $O/p55 $O/nop
log "start, uptime $(uptime), processes $(ps -ax | wc -l)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
[ -z "$($SU geli list 2>/dev/null | grep '^Geom name: md')" ] || { log "geli providers on md exist: abort"; exit 1; }
[ -z "$(kenv -q kern.kwq.bulk.priority 2>/dev/null)" ] || { log "kern.kwq.bulk.priority set: abort"; exit 1; }
B=$D/src/kwq-geli/tests/geli_bench.sh
E="MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1 MD_TYPE=null PROBE_S=3"
one() {	# GROUP TAG REP STAGES OPTS...
	g=$1; t=$2; r=$3; st=$4; shift 4
	$SU env $E OUT_TSV=$O/$g/${t}_r$r.tsv "$@" sh $B $st > $O/$g/${t}_r$r.log 2>&1
	e=$?
	[ $e = 0 ] || { log "$g $t rep $r ($st): exit $e: abort"; return 1; }
}
drives() {	# GROUP REP STAGES
	case $M in
	lab)	one $1 storage $2 "$3" TESTS=dev DEV_N=98 DEV_FAST=8 DEV_DELAY_MS=10 DEV_QD=2 &&
		one $1 home $2 "$3" TESTS=dev DEV_N=2 DEV_FAST=0 DEV_DELAY_MS=9 DEV_QD=4 ;;
	a07)	one $1 compute $2 "$3" TESTS=dev DEV_N=4 DEV_FAST=0 DEV_DELAY_MS=1 DEV_QD=4 DEV_SPIN=1 ;;
	esac
}
bench() {	# GROUP REP STAGES
	one $1 scale $2 "$3" TESTS=scale IDLE_S=3 SCALE_NS="$NS" SCALE_MDSIZE=1024 &&
	one $1 tput $2 "$3" TESTS="tput resp" MDSIZE=1024 &&
	one $1 tput32k $2 "$3" TESTS=tput ELI_SECSIZE=32768 MDSIZE=1024 &&
	one $1 ssd $2 "$3" TESTS=ssd SSD_N=16 SSD_MDSIZE=1024 &&
	drives $1 $2 "$3" || return 1
	log "rep $2 $1 ($3) done, processes $(ps -ax | wc -l)"
}
kwqload() {	# PRIORITY|default
	[ $1 = default ] || { $SU kenv kern.kwq.bulk.priority=$1 >/dev/null; log "set kenv kern.kwq.bulk.priority=$1"; }
	$SU kldload $S/kwq.ko || { log "cannot load kwq"; return 1; }
	log "loaded kwq.ko: priority=$(sysctl -n kern.kwq.bulk.priority) quantum_us=$(sysctl -n kern.kwq.bulk.quantum_us) cap_period_us=$(sysctl -n kern.kwq.bulk.cap_period_us)"
}
kwqunload() {	# PRIORITY|default
	$SU kldunload kwq; log "unloaded kwq"
	[ $1 = default ] || { $SU kenv -u kern.kwq.bulk.priority; log "unset kenv kern.kwq.bulk.priority"; }
}
r=1
while [ $r -le 3 ]; do
	drives nop $r nop || break
	one nop ssd $r nop TESTS=ssd SSD_N=16 SSD_MDSIZE=1024 || break
	bench stock $r eli || break
	kwqload default || break
	bench p2 $r "kwqshard kwqbio kwqtime"; e=$?
	kwqunload default
	[ $e = 0 ] || break
	kwqload 55 || break
	bench p55 $r "kwqbio kwqtime"; e=$?
	kwqunload 55
	[ $e = 0 ] || break
	r=$((r + 1))
done
log "end after rep $r, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $($SU mdconfig -l | wc -w), geli on md $($SU geli list 2>/dev/null | grep -c '^Geom name: md'), kenv $(kenv -q kern.kwq.bulk.priority 2>/dev/null || echo unset), processes $(ps -ax | wc -l), uptime $(uptime)"
touch $D/battery.done
