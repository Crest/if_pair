#!/bin/sh
# nettime.sh lab|a07 DIR: does NET still stop a bio under the time rule?
# Test 2 of samples/p6g10 (kpair beside GELI writes, pinned and not) at
# today's stop, the time rule and the candidate without the copy, 3
# repetitions with the stages rotated (they share the md devices, so a fixed
# order would show as a difference), on swap-backed md as there.
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/${OUT:-out}/net; mkdir -p $O; N=$D/src/samples/p6g10
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log; X="NP=8 STREAMS=4 USERCPUS=2-15 NET_SECS=${NET_SECS:-12} SPARSE_S=5"
	log() { echo "$(date -u +%FT%TZ) nettime: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log; X="NP=8 STREAMS=32 USERCPUS=2-63 NET_SECS=${NET_SECS:-12} SPARSE_S=5"
	log() { echo "$(date -u +%FT%TZ) nettime: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
NP=8
rot() {		# N WORD...: the words rotated by N
	n=$1; shift
	while [ "$n" -gt 0 ]; do w=$1; shift; set -- "$@" "$w"; n=$((n - 1)); done
	echo "$*"
}
log "start, uptime $(uptime)"
for m in kwq g_kwqeli if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
$SU kldload $S/kwq.ko && $SU kldload $S/if_kpair.ko || { log "cannot load"; exit 1; }
rc=0
if $SU sh $N/net_setup.sh up $NP; then
	r=1
	while [ $r -le ${NET_REPS:-3} ]; do
		$SU env MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1 SECS=10 OUT_TSV=$O/r$r.tsv TESTS="net netcpu" NET_CMD="env $X SETTINGS_LOG=$L sh $N/net_traffic.sh" \
		    sh $D/src/kwq-geli/tests/geli_bench.sh $(rot $((r - 1)) ${NET_STAGES:-kwqshard kwqtime kwqout}) > $O/r$r.log 2>&1
		e=$?
		log "rep $r: exit $e, kpair cpu $(sysctl -n net.link.kpair.cpu)"
		[ $e = 0 ] && [ "$(sysctl -n net.link.kpair.cpu)" = -1 ] || { rc=1; break; }
		r=$((r + 1))
	done
fi
if $SU sh $N/net_setup.sh down $NP; then
	$SU kldunload if_kpair; $SU kldunload kwq; log "unloaded if_kpair, kwq"
else
	log "net_setup down failed: if_kpair and kwq left loaded"; rc=1
fi
log "end, rc $rc, modules $(kldstat | grep -cE 'kwq|kwqeli|kpair'), jails $(jls -d name | grep -c '^ifp_'), uptime $(uptime)"
touch $D/nettime.done
