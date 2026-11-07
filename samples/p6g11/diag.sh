#!/bin/sh
# diag.sh lab|a07 DIR sweep|prof STAGE...: the storage model's write ceiling,
# run alone on the machine, with no probes beside the load (IO_QUIET).
#   sweep  request size 64 KiB to 1 MiB at the same 98 drives and depth: the
#          working set grows from below L3 to well above it.  Rows of
#          DIR/diag/sweep_<stage>_<bs>.tsv
#   prof   6 s dtrace profiles of the kernel's leaf functions under 1 MiB
#          writes alone and beside reads: DIR/diag/prof_<stage>_{write,both}.txt
# The stage eli runs with kwq.ko unloaded, any other with it loaded.
M=${1:?lab|a07}; D=${2:?DIR}; W=${3:?sweep|prof}; shift 3
S=$D/stage; O=$D/diag; mkdir -p $O
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) diag: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) diag: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
for m in kwq g_kwqeli g_nop g_simdisk if_kpair; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
log "start $W $*, uptime $(uptime)"
B=$D/src/kwq-geli/tests/geli_bench.sh
E="MODDIR=$S VICTIM=$S/victim SETTINGS_LOG=$L REPS=1 MD_TYPE=sim PROBE_S=3 IO_QUIET=1 TESTS=dev DEV_N=98 DEV_FAST=8 DEV_FW=3000 DEV_FR=5000 DEV_FLAT=20 DEV_W=200 DEV_R=200 DEV_LAT=100 DEV_SEEK=8000 DEV_QD=2 DEV_RQD=2"
DT=
if [ "$W" = prof ] && ! kldstat -q -m dtrace; then
	$SU kldload dtraceall && DT=1 && log "loaded dtraceall"
fi
load() { [ "$1" = eli ] || $SU kldload $S/kwq.ko; }
unload() { [ "$1" = eli ] || $SU kldunload kwq; }
prof() {	# STAGE PHASE PATTERN: 6 s of the load PATTERN finds
	i=0
	until pgrep -f "$3" > /dev/null || [ $i -gt 1200 ]; do sleep 0.5; i=$((i + 1)); done
	sleep 2
	$SU dtrace -q -x aggsize=8m -n 'profile-997 /arg0/ { @f[func(arg0)] = count(); } profile-997 /arg1/ { @u = count(); } tick-6s { printa("%@8u %a\n", @f); printa("%@8u user\n", @u); exit(0); }' 2>&1 | sort -n | tail -40 > $O/prof_$1_$2.txt
	# ITEM: SIZE, LIMIT, USED, FREE, REQ, FAIL, SLEEP, XDOMAIN
	vmstat -z | grep -E '^(ITEM|GELI)' >> $O/prof_$1_$2.txt
}
for st in "$@"; do
	load $st || exit 1
	case $W in
	sweep)
		for bs in 65536 131072 262144 524288 1048576; do
			$SU env $E OUT_TSV=$O/sweep_${st}_$bs.tsv DEV_BS=$bs DEV_RBS=$bs sh $B $st > $O/sweep_${st}_$bs.log 2>&1
			log "$st $bs: exit $?, write $(awk -F'\t' '$4 == "write_mbps" { print $5 }' $O/sweep_${st}_$bs.tsv) read $(awk -F'\t' '$4 == "read_mbps" { print $5 }' $O/sweep_${st}_$bs.tsv)"
		done ;;
	prof)
		# dtrace takes seconds to start: a load of 25 s outlasts it
		$SU env $E PROBE_S=10 OUT_TSV=$O/prof_$st.tsv sh $B $st > $O/prof_$st.log 2>&1 &
		bp=$!
		prof $st write 'iolat -w'
		# both starts writers and readers together; the read phase alone
		# has no writer, so wait for one to come back
		until ! pgrep -f 'iolat -w' > /dev/null || [ $i -gt 600 ]; do sleep 0.5; i=$((i + 1)); done
		prof $st both 'iolat -w'
		wait $bp
		log "$st: write $(awk -F'\t' '$4 == "write_mbps" { print $5 }' $O/prof_$st.tsv) both $(awk -F'\t' '$4 == "both_w_mbps" { print $5 }' $O/prof_$st.tsv)/$(awk -F'\t' '$4 == "both_r_mbps" { print $5 }' $O/prof_$st.tsv)" ;;
	esac
	unload $st || { log "unload failed: abort"; exit 1; }
done
if [ -n "$DT" ]; then
	$SU kldunload dtraceall && $SU kldunload profile dtnfscl dtmalloc dtaudit fbt sdt systrace systrace_freebsd32 systrace_linux systrace_linux32 fasttrap dtrace_test 2>/dev/null
	log "unloaded dtraceall, dtrace modules left: $(kldstat | grep -cE 'dtrace|dtmalloc|dtnfscl|fbt|sdt|systrace|profile|fasttrap|opensolaris')"
fi
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|simdisk'), uptime $(uptime)"
