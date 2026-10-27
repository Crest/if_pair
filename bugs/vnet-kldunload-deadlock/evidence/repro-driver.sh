#!/bin/sh
# Host-side driver for reproducer-epair.sh on the kwqvm guest: runs TRIALS
# trials of ROUNDS rounds each, detects the deadlock from outside (the
# reproducer's own shell hangs in `jail -r lor2`, which is one of the two
# deadlocked threads), records the round, panics the guest for the
# reboot and goes on.  Output: one line per trial in RESULT.
#   sh repro-driver.sh [TRIALS] [ROUNDS]
TRIALS=${1:-3}; ROUNDS=${2:-300}
SSH="sh /home/crest/if_pair/kwq/tests/kwqvm.sh ssh"
RESULT=${RESULT:-/home/crest/if_pair/bugs/vnet-kldunload-deadlock/evidence/repro-trials.txt}
t=0
while [ $t -lt $TRIALS ]; do
	t=$((t + 1))
	$SSH "rm -f /root/repro.log; (nohup sh -c 'ROUNDS=$ROUNDS sh /root/reproducer-epair.sh; echo exit=\$?' > /root/repro.log 2>&1 &); echo started"
	start=$(date +%s)
	while :; do
		sleep 5
		st=$($SSH -o ConnectTimeout=5 'ps axo stat,wchan,command | grep "kldunload if_epair" | grep -v grep | head -1; echo "|"; tail -1 /root/repro.log' 2>/dev/null)
		case "$st" in
		D*ifnet_de*)
			sleep 10
			# Re-check with ps only.  Do NOT run procstat here: on a
			# deadlocked kernel procstat -kk -a blocks and wedges the ssh.
			st2=$($SSH -o ConnectTimeout=5 'ps axo stat,wchan,command | grep -E "kldunload if_epair|jail -r lor2" | grep -v grep; echo "|"; grep -c "start" /root/repro.log' 2>/dev/null)
			case "$st2" in *D*ifnet_de*) ;; *) continue ;; esac
			round=$(echo "$st2" | sed -n '/^[0-9]/p' | tail -1)
			echo "trial $t: DEADLOCK by round $round after $(( $(date +%s) - start )) s; panicking for the reboot" | tee -a "$RESULT"
			($SSH -o ConnectTimeout=5 'sysctl debug.kdb.panic=1' >/dev/null 2>&1 &)
			sleep 30; n=0
			while ! $SSH -o ConnectTimeout=3 'uptime' >/dev/null 2>&1; do sleep 10; n=$((n+1)); [ $n -ge 60 ] && { echo "guest did not come back" | tee -a "$RESULT"; exit 1; }; done
			sleep 5
			break ;;
		*"exit="*)
			echo "trial $t: no deadlock in $ROUNDS rounds ($(( $(date +%s) - start )) s): $(echo "$st" | tail -1)" | tee -a "$RESULT"
			break ;;
		esac
		[ $(( $(date +%s) - start )) -gt 1800 ] && { echo "trial $t: timeout" | tee -a "$RESULT"; break; }
	done
done
