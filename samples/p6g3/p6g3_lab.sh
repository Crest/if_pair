#!/bin/sh
# p6g3_lab.sh - P6g.3 on lab (kwq/SPEC-p6g.md S5): kern.kwq.bulk.place
# 0 (today), 1 (gossip) and 2 (P8) on one boot.  Run on lab as crest,
# under sh (zsh is lab's shell):
#
#   sh p6g3_lab.sh DIR      # DIR: where bulk_deploy.sh put src/ and stage/
#
# 1. the kwq ATF suites under place 1 and 2 (lab's 16 threads, SMT);
# 2. kwq_test steal and anyburst under each place, 3 repetitions;
# 3. geli_bench scale (4, 16, 128 providers; small reads, bulk writes) on
#    kwqnosteal, kwq, kwqgossip, kwqshard, 3 repetitions;
# 4. geli_bench tput and resp on kwq, kwqgossip, kwqshard, 3 repetitions.
# Uptime before and after; every module, md and knob through the settings
# log; geom_eli (lab's swap) and if_epair are never unloaded.
D=${1:?DIR}
H=$HOME/if_pair-scratch; L=$H/settings_lab.log; O=$D/out
mkdir -p "$O"; rm -f "$D/p6g3_lab.done"
log() { echo "$(date -u +%FT%TZ) p6g3: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do
	kldstat -q -m $m && { log "$m loaded before the run: abort"; exit 1; }
done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist before the run: abort"; exit 1; }
pgrep -f 'sh (bulk_lab|sl_lab|scale_lab|geli_bench)[.]sh' >/dev/null && { log "another battery runs: abort"; exit 1; }
S=$D/stage

# 1. ATF
for P in 1 2; do
	sudo -n kyua -v test_suites.kwq.moddir=$S -v test_suites.kwq.place=$P \
	    test -k $D/src/kwq/tests/Kyuafile > $O/atf_place$P.log 2>&1
	sudo -n kyua report --verbose --results-filter=failed,broken > $O/atf_place$P.fail 2>&1
	log "atf place=$P: $(grep 'passed (' $O/atf_place$P.log)"
done
for m in kwq_test kwq_crypto kwq; do kldstat -q -m $m && { log "$m left loaded by the suite: abort"; exit 1; }; done

# 2. kwq_test under each place
sudo -n kldload $S/kwq.ko && sudo -n kldload $S/kwq_test.ko || { log "cannot load kwq"; exit 1; }
log "loaded kwq.ko kwq_test.ko ($S)"
T=$O/kwqtest_lab.tsv; : > $T
r() { sysctl -n kern.kwq_test.result_$1; }
for rep in 1 2 3; do
	for P in 0 1 2; do
		sudo -n sysctl -q kern.kwq.bulk.place=$P >/dev/null
		for sc in steal anyburst; do
			c=1; [ $sc = anyburst ] && c=0
			sudo -n sysctl -q kern.kwq_test.scenario=$sc kern.kwq_test.cpu=$c \
			    kern.kwq_test.items=20000 kern.kwq_test.cost_us=20 kern.kwq_test.limit=100000 >/dev/null
			sudo -n sysctl -q kern.kwq_test.run=1 >/dev/null
			while [ "$(r state)" = running ]; do sleep 0.2; done
			printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $sc $P $rep "$(r state)" "$(r ns)" \
			    "$(r offcpu)" "$(r cpus)" "$(r dups)" >> $T
		done
	done
done
# no_arm_when_idle under place=1 failed once (43 timer interrupts in the
# silence against 5): three runs of that case alone
sudo -n sysctl -q kern.kwq.bulk.place=0 >/dev/null
sudo -n kldunload kwq_test; sudo -n kldunload kwq
log "kwq_test done, unloaded kwq_test kwq: $(wc -l < $T) rows"
for i in 1 2 3; do
	sudo -n kyua -v test_suites.kwq.moddir=$S -v test_suites.kwq.place=1 \
	    test -k $D/src/kwq/tests/Kyuafile kwq_test.sh:no_arm_when_idle > $O/noarm$i.log 2>&1
	sudo -n kyua report --verbose --results-filter=passed,failed 2>/dev/null | grep "timer interrupts" >> $O/noarm$i.log
	log "no_arm_when_idle place=1 run $i: $(grep -c 'passed (' $O/noarm$i.log) passed, $(grep 'timer interrupts' $O/noarm$i.log)"
done

# kwq_crypto: one stealable queue, and four, fed on CPU 1 (K4), each place
sudo -n kldload $S/kwq.ko && sudo -n kldload $S/kwq_crypto.ko || { log "cannot load kwq_crypto"; exit 1; }
log "loaded kwq.ko kwq_crypto.ko ($S)"
K=$O/crypto_lab.tsv; : > $K
for rep in 1 2 3; do
	for P in 0 1 2; do
		sudo -n sysctl -q kern.kwq.bulk.place=$P >/dev/null
		for nq in 1 4; do
			sudo -n sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=$nq kern.kwq_crypto.depth=16 \
			    kern.kwq_crypto.cpu=1 kern.kwq_crypto.stealable=1 kern.kwq_crypto.secs=3 \
			    kern.kwq_crypto.verify=0 kern.kwq_crypto.bytes=65536 >/dev/null
			sudo -n sysctl -q kern.kwq_crypto.run=1 >/dev/null
			while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.2; done
			printf '%s\t%s\t%s\t%s\n' $nq $P $rep "$(dmesg | grep 'kwq_crypto: kwq nq' | tail -1)" >> $K
		done
	done
done
sudo -n sysctl -q kern.kwq.bulk.place=0 >/dev/null
sudo -n kldunload kwq_crypto; sudo -n kldunload kwq
log "kwq_crypto done, unloaded kwq_crypto kwq: $(wc -l < $K) rows"

# 3. and 4. geli_bench
B="$D/src/kwq-geli/tests/geli_bench.sh"
sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_lab.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS="4 16 128" SCALE_MDSIZE=32 TESTS=scale \
    sh "$B" kwqnosteal kwq kwqgossip kwqshard > "$O/scale_lab.log" 2>&1
log "geli_bench scale exit $?"
sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/tput_lab.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 TESTS="tput resp" \
    sh "$B" kwq kwqgossip kwqshard > "$O/tput_lab.log" 2>&1
log "geli_bench tput resp exit $?"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(sudo -n mdconfig -l | wc -w), uptime $(uptime)"
touch "$D/p6g3_lab.done"
