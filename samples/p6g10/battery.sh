#!/bin/sh
# battery.sh lab|a07|guest DIR [t1] [t2]: the two tests of BULK.md S7.6's end,
# with budget_sectors 0, the check between bios only.
#   t1  budget_check_every 1 against 8 (kwqbio1, kwqbio): 4 KiB reads in deep
#       queues, unpinned and on one CPU, and the mixed sizes the estimate
#       misjudges; the two stages' order alternates, R1 repetitions
#   t2  if_pair traffic beside GELI writes, unpinned and on one CPU: no GELI
#       (none), stock (eli), kwq-geli at budget_sectors 16 (kwqshard) and 0
#       (kwqbio), R2 repetitions.  One GELI at a time.  if_pair needs kwq.ko,
#       so unlike the other batteries it stays loaded in stock GELI's stage,
#       with geom_kwqeli unloaded and BULK idle
# DIR/stage holds the modules with if_kpair.ko, DIR/src the sources; output
# DIR/out/{t1,t2}/*.tsv.  kpair.py says why if_kpair and not if_pair.
M=${1:?lab|a07|guest}; D=${2:?DIR}; shift 2; W=${*:-t1 t2}
S=$D/stage; O=$D/out; rm -f $D/battery.done; mkdir -p $O/t1 $O/t2
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	NP=8 STREAMS=4 DEEP=16 USERCPUS=2-15 R1=7 R2=3 SECS=10
	log() { echo "$(date -u +%FT%TZ) battery: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	NP=8 STREAMS=32 DEEP=32 USERCPUS=2-63 R1=7 R2=3 SECS=10
	log() { echo "$(date -u +%FT%TZ) battery: $*" | doas -n tee -a "$L"; } ;;
guest) SU=; L=/root/ifpair-scratch/settings_guest.log
	NP=2 STREAMS=2 DEEP=4 USERCPUS=2-3 R1=2 R2=1 SECS=5
	log() { echo "$(date -u +%FT%TZ) battery: $*" | tee -a "$L"; } ;;
*) exit 2 ;;
esac
B=$D/src/kwq-geli/tests/geli_bench.sh; N=$D/src/p6g10
log "start ($W), uptime $(uptime), processes $(ps -ax | wc -l)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
[ -z "$($SU geli list 2>/dev/null | grep '^Geom name: md')" ] || { log "geli providers on md exist: abort"; exit 1; }
[ -z "$(ifconfig -l | tr ' ' '\n' | grep '^kpair')" ] || { log "kpair interfaces exist: abort"; exit 1; }
[ -z "$(jls -d name | grep '^ifp_')" ] || { log "test jails exist: abort"; exit 1; }
E="MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1"
rc=0
case " $W " in *" t1 "*)
	r=1
	while [ $r -le $R1 ]; do
		case $((r % 2)) in 1) o="kwqbio kwqbio1" ;; 0) o="kwqbio1 kwqbio" ;; esac
		$SU env $E OUT_TSV=$O/t1/r$r.tsv SECS=$SECS PROBE_S=3 DEEP_N=$DEEP TESTS="deep deepcpu mixed" \
		    sh $B $o > $O/t1/r$r.log 2>&1
		e=$?
		log "t1 rep $r ($o): exit $e, processes $(ps -ax | wc -l)"
		[ $e = 0 ] || { rc=1; break; }
		r=$((r + 1))
	done ;;
esac
case " $W " in *" t2 "*)
	[ $rc = 0 ] || { log "t1 failed: t2 not run"; exit 1; }
	$SU kldload $S/kwq.ko && $SU kldload $S/if_kpair.ko || { log "cannot load kwq, if_kpair"; exit 1; }
	log "loaded kwq.ko, if_kpair.ko"
	unload() {
		if $SU sh $N/net_setup.sh down $NP; then
			$SU kldunload if_kpair; $SU kldunload kwq; log "unloaded if_kpair, kwq"
		else
			log "net_setup down failed: if_kpair and kwq left loaded"; rc=1
		fi
	}
	$SU sh $N/net_setup.sh up $NP || { log "net_setup up failed"; unload; exit 1; }
	log "$NP pairs up"
	X="NP=$NP STREAMS=$STREAMS USERCPUS=$USERCPUS NET_SECS=12 SPARSE_S=5"
	NC="env $X SETTINGS_LOG=$L sh $N/net_traffic.sh"
	r=1
	while [ $r -le $R2 ]; do
		for g in "none" "eli" "kwqshard kwqbio"; do
			t=$(echo $g | tr ' ' '_')
			case $g in none) a="P=1 ONECPU_N=1" ;; *) a= ;; esac
			$SU env $E $a OUT_TSV=$O/t2/${t}_r$r.tsv SECS=$SECS TESTS="net netcpu" NET_CMD="$NC" \
			    sh $B $g > $O/t2/${t}_r$r.log 2>&1
			e=$?
			log "t2 rep $r ($g): exit $e, processes $(ps -ax | wc -l), kpair cpu $(sysctl -n net.link.kpair.cpu)"
			[ "$(sysctl -n net.link.kpair.cpu)" = -1 ] || { log "net.link.kpair.cpu left set: abort"; e=1; }
			[ $e = 0 ] || { rc=1; break 2; }
		done
		r=$((r + 1))
	done
	unload ;;
esac
log "end, rc $rc, modules $(kldstat | grep -cE 'kwq|kwqeli|kpair|geom_nop'), md $($SU mdconfig -l | wc -w), geli on md $($SU geli list 2>/dev/null | grep -c '^Geom name: md'), kpair $(ifconfig -l | tr ' ' '\n' | grep -c '^kpair'), jails $(jls -d name | grep -c '^ifp_'), processes $(ps -ax | wc -l), uptime $(uptime)"
touch $D/battery.done
exit $rc
