#!/bin/sh
# yield_check.sh DIR (lab): why a sparse ping on stock GELI's CPU takes
# milliseconds.  The NET worker yields at PUSER (SCHED.md S6.2), stock GELI's
# workers run at PUSER, so the yield would hand them the CPU for their slice.
# netcpu with stock GELI, kern.kwq.net.yield_prio at 56, then at 55, which
# yields to kernel threads only, then at 56 again.
D=${1:?DIR}; S=$D/stage; O=$D/out/yield; N=$D/src/p6g10; mkdir -p $O
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) yield_check: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq g_kwqeli if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
sudo -n kldload $S/kwq.ko && sudo -n kldload $S/if_kpair.ko || exit 1
sudo -n sh $N/net_setup.sh up 1 || { log "net_setup up failed"; exit 1; }
y0=$(sysctl -n kern.kwq.net.yield_prio)
i=1
for y in 56 55 56; do
	sudo -n sysctl -q kern.kwq.net.yield_prio=$y >/dev/null; log "set kern.kwq.net.yield_prio=$y"
	sudo -n env MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1 SECS=10 OUT_TSV=$O/y${y}_$i.tsv TESTS=netcpu \
	    NET_CMD="env NP=1 USERCPUS=2-15 NET_SECS=12 SPARSE_S=5 SETTINGS_LOG=$L sh $N/net_traffic.sh" \
	    sh $D/src/kwq-geli/tests/geli_bench.sh eli > $O/y${y}_$i.log 2>&1
	log "yield_prio $y: exit $?"
	i=$((i + 1))
done
sudo -n sysctl -q kern.kwq.net.yield_prio=$y0 >/dev/null; log "set kern.kwq.net.yield_prio=$y0 (back)"
if sudo -n sh $N/net_setup.sh down 1; then sudo -n kldunload if_kpair; sudo -n kldunload kwq; log "unloaded if_kpair, kwq"; else log "net_setup down failed: modules left"; fi
log "end, modules $(kldstat | grep -cE 'kwq|kpair'), md $(sudo -n mdconfig -l | wc -w), jails $(jls -d name | grep -c '^ifp_'), uptime $(uptime)"
