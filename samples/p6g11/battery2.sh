#!/bin/sh
# battery2.sh lab|a07 DIR: every candidate on geom_simdisk.ko (MD_TYPE=sim),
# scored by score.py against the best per test:
#   stock   stock GELI, kwq.ko not loaded (checked)
#   p2      BULK's default priority: today (kwqshard), whole bios (kwqbio),
#           the time rule (kwqtime), with reads ahead (kwqrq), and with 1 MiB
#           buffers from a zone (kwqbuf)
#   p55     BULK below md's priority: kwqbuf
#   nop     no encryption: the drives alone
# Three repetitions; the groups' order and the stages' order within p2 rotate
# with the repetition, so that no candidate always runs first or last.  One
# GELI at a time.  Per setup and repetition: scale, tput resp, tput at 32 KiB
# sectors, and the drives of the machine it stands in for:
#   lab  storage (8 NVMe-like, 90 HDD-like), storage64 (the same at 64 KiB
#        requests, below kmem_zmax), home (2 SATA-like)
#   a07  compute (4 NVMe-like, every CPU spinning, iolat at rtprio), storage,
#        storage64
# Output: DIR/out2/{stock,p2,p55,nop}/<test>_r<rep>.tsv
# Environment: OUT (out2), the output directory under DIR; STAGES, the stages
# of p2; GROUPS (stock p2 p55), the groups, nop always first.
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/${OUT:-out2}; rm -f $D/battery2.done
: "${STAGES:=kwqshard kwqbio kwqtime kwqrq kwqbuf}" "${GROUPS:=stock p2 p55}"
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log; NS="4 16 32 64 128"
	log() { echo "$(date -u +%FT%TZ) p6g11b: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log; NS="4 16 64 128"
	log() { echo "$(date -u +%FT%TZ) p6g11b: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
mkdir -p $O/stock $O/p2 $O/p55 $O/nop
log "start, uptime $(uptime), processes $(ps -ax | wc -l)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop g_simdisk if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
[ -z "$($SU geli list 2>/dev/null | grep '^Geom name: \(md\|sd\)')" ] || { log "geli providers on test devices exist: abort"; exit 1; }
[ -z "$(kenv -q kern.kwq.bulk.priority 2>/dev/null)" ] || { log "kern.kwq.bulk.priority set: abort"; exit 1; }
B=$D/src/kwq-geli/tests/geli_bench.sh
E="MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1 MD_TYPE=sim PROBE_S=3"
STO="TESTS=dev DEV_N=98 DEV_FAST=8 DEV_FW=3000 DEV_FR=5000 DEV_FLAT=20 DEV_W=200 DEV_R=200 DEV_LAT=100 DEV_SEEK=8000 DEV_QD=2 DEV_RQD=2"
one() {	# GROUP TAG REP STAGES OPTS...
	g=$1; t=$2; r=$3; st=$4; shift 4
	$SU env $E OUT_TSV=$O/$g/${t}_r$r.tsv "$@" sh $B $st > $O/$g/${t}_r$r.log 2>&1
	e=$?
	[ $e = 0 ] || { log "$g $t rep $r ($st): exit $e: abort"; return 1; }
}
drives() {	# GROUP REP STAGES
	one $1 storage $2 "$3" $STO &&
	one $1 storage64 $2 "$3" $STO DEV_BS=65536 DEV_RBS=65536 &&
	case $M in
	lab)	one $1 home $2 "$3" TESTS=dev DEV_N=2 DEV_FAST=0 DEV_W=450 DEV_R=520 DEV_LAT=100 DEV_QD=4 ;;
	a07)	one $1 compute $2 "$3" TESTS=dev DEV_N=4 DEV_FAST=4 DEV_FW=3000 DEV_FR=6000 DEV_FLAT=20 DEV_QD=8 DEV_RQD=4 DEV_SPIN=1 ;;
	esac
}
bench() {	# GROUP REP STAGES
	one $1 scale $2 "$3" TESTS=scale IDLE_S=3 SCALE_NS="$NS" SCALE_MDSIZE=1024 &&
	one $1 tput $2 "$3" TESTS="tput resp" MDSIZE=1024 &&
	one $1 tput32k $2 "$3" TESTS=tput ELI_SECSIZE=32768 MDSIZE=1024 &&
	drives $1 $2 "$3" || return 1
	log "rep $2 $1 ($3) done, processes $(ps -ax | wc -l)"
}
kwqload() {	# PRIORITY|default
	[ $1 = default ] || { $SU kenv kern.kwq.bulk.priority=$1 >/dev/null; log "set kenv kern.kwq.bulk.priority=$1"; }
	$SU kldload $S/kwq.ko || { log "cannot load kwq"; return 1; }
	log "loaded kwq.ko: priority=$(sysctl -n kern.kwq.bulk.priority) item_min_us=$(sysctl -n kern.kwq.bulk.item_min_us)"
}
kwqunload() {	# PRIORITY|default
	$SU kldunload kwq || { log "kldunload kwq failed: abort"; return 1; }
	kldstat -q -m kwq && { log "kwq still loaded: abort"; return 1; }
	log "unloaded kwq"
	[ $1 = default ] || { $SU kenv -u kern.kwq.bulk.priority; log "unset kenv kern.kwq.bulk.priority"; }
}
rot() {		# N WORD...: the words rotated by N
	n=$1; shift
	while [ "$n" -gt 0 ]; do w=$1; shift; set -- "$@" "$w"; n=$((n - 1)); done
	echo "$*"
}
group() {	# stock|p2|p55 REP
	case $1 in
	stock)	kldstat -q -m kwq && { log "kwq loaded before stock: abort"; return 1; }
		bench stock $2 eli ;;
	p2)	kwqload default || return 1
		bench p2 $2 "$(rot $(($2 - 1)) $STAGES)"; e=$?
		kwqunload default && return $e ;;
	p55)	kwqload 55 || return 1
		bench p55 $2 kwqbuf; e=$?
		kwqunload 55 && return $e ;;
	esac
}
r=1
while [ $r -le 3 ]; do
	drives nop $r nop || break
	for g in $(rot $((r - 1)) $GROUPS); do
		group $g $r || break 2
	done
	r=$((r + 1))
done
log "end after rep $r, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|simdisk'), md $($SU mdconfig -l | wc -w), geli on test devices $($SU geli list 2>/dev/null | grep -c '^Geom name: \(md\|sd\)'), kenv $(kenv -q kern.kwq.bulk.priority 2>/dev/null || echo unset), processes $(ps -ax | wc -l), uptime $(uptime)"
touch $D/battery2.done
