#!/bin/sh
# Generic host-side watcher: runs REPRO on the guest with MODDIR, detects
# a deadlock as a 36 s freeze of the round counter, panics+reboots on a
# hit, up to TRIALS trials of ROUNDS rounds.  Vars: REPRO (basename on
# guest, default reproducer-ifpair.sh), MODDIR (guest path), TRIALS,
# ROUNDS, RESULT (host file).
REPRO=${REPRO:-reproducer-ifpair.sh}; MODDIR=${MODDIR:-/root/ifpairfix/fix}
TRIALS=${TRIALS:-1}; ROUNDS=${ROUNDS:-300}
SSH="sh /home/crest/if_pair/kwq/tests/kwqvm.sh ssh -o ConnectTimeout=5"
RESULT=${RESULT:-/dev/stdout}
t=0
while [ $t -lt $TRIALS ]; do
	t=$((t + 1))
	$SSH "rm -f /root/repro.log; (nohup sh -c 'ROUNDS=$ROUNDS MODDIR=$MODDIR sh /root/$REPRO; echo end' > /root/repro.log 2>&1 &)" >/dev/null
	start=$(date +%s); last=""; stall=0; v=""
	while :; do
		sleep 6
		line=$($SSH 'tail -1 /root/repro.log' 2>/dev/null)
		case "$line" in *"no deadlock"*|*end*) v="trial $t: NO deadlock, $ROUNDS rounds ($(( $(date +%s)-start ))s)"; break ;; esac
		if [ "$line" = "$last" ]; then stall=$((stall+6)); else stall=0; last="$line"; fi
		if [ $stall -ge 36 ]; then
			sig=$($SSH 'ps axo stat,wchan,command | grep -E "kldunload if_pair|jail -r lor2" | grep -v grep | tr "\n" ";"' 2>/dev/null)
			r=$(echo "$last" | sed -n 's/^round \([0-9]*\) start/\1/p')
			v="trial $t: DEADLOCK at round ${r:-?} ($(( $(date +%s)-start ))s); $sig"; break
		fi
		[ $(( $(date +%s)-start )) -gt 1500 ] && { v="trial $t: inconclusive (1500s, last:$line)"; break; }
	done
	echo "$v" | tee -a "$RESULT"
	case "$v" in *DEADLOCK*)
		($SSH 'sysctl debug.kdb.panic=1' >/dev/null 2>&1 &); sleep 25; n=0
		while ! $SSH 'uptime' >/dev/null 2>&1; do sleep 10; n=$((n+1)); [ $n -ge 60 ] && { echo "guest lost"|tee -a "$RESULT"; exit 1; }; done; sleep 5 ;;
	esac
done
