#!/bin/sh
# where_lab.sh - where.d around geli_bench scale at 16, 32 and 128
# providers, REPS=1, for stock GELI (eli), kwq-geli at the defaults
# (kwqshard) and kwq-geli with BULK's share limit off (kwqshard with
# kern.kwq.bulk.cap_permille=1000, restored after); dtraceall loaded and
# unloaded again, every setting through the settings log; geom_kwqeli is
# preloaded so that the profile resolves its functions at the end.
#   sh where_lab.sh DIR      (DIR with stage/ and src/)
D=${1:?DIR}; S=$D/stage; O=$D/out/where; mkdir -p $O; rm -f $D/where.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) where: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
sudo -n kldload $S/kwq.ko && sudo -n kldload $S/geom_kwqeli.ko || { log "cannot load kwq, geom_kwqeli"; exit 1; }
log "loaded kwq.ko: place=$(sysctl -n kern.kwq.bulk.place) quantum_us=$(sysctl -n kern.kwq.bulk.quantum_us) cap_period_us=$(sysctl -n kern.kwq.bulk.cap_period_us) cap_permille=$(sysctl -n kern.kwq.bulk.cap_permille)"
dl=0; kldstat -q -m dtraceall || { sudo -n kldload dtraceall && dl=1 && log "loaded dtraceall"; }
c0=$(sysctl -n kern.kwq.bulk.cap_permille)
for run in eli:eli kwqshard:kwqshard nocap:kwqshard; do
	tag=${run%%:*}; st=${run#*:}
	if [ $tag = nocap ]; then sudo -n sysctl -q kern.kwq.bulk.cap_permille=1000 >/dev/null; log "set kern.kwq.bulk.cap_permille=1000 (was $c0)"; fi
	cat > $O/run_$tag.sh <<EOS
env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_$tag.tsv" SETTINGS_LOG="$L" REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="16 32 128" SCALE_MDSIZE=32 TESTS=scale sh "$B" $st > "$O/scale_$tag.log" 2>&1
EOS
	sudo -n dtrace -L $D/src/kwq/kwq -s $D/where.d -c "sh $O/run_$tag.sh" > $O/where_$tag.out 2>&1
	log "where.d over $tag: exit $?"
	if [ $tag = nocap ]; then sudo -n sysctl -q kern.kwq.bulk.cap_permille=$c0 >/dev/null; log "restored kern.kwq.bulk.cap_permille=$c0"; fi
done
[ $dl = 1 ] && sudo -n kldunload dtraceall && log "unloaded dtraceall"
sudo -n kldunload geom_kwqeli; sudo -n kldunload kwq
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|dtrace'), md $(sudo -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/where.done
