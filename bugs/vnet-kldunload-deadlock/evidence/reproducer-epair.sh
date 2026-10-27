#!/bin/sh
# Reproducer for the vnet_destroy() / vnet_deregister_sysuninit() lock
# order deadlock with the stock epair(4) driver only (no if_pair, no kwq).
#
# Each round: load if_epair, create two epairs, give one end of each to
# a fresh vnet jail that runs one process, remove both jails, unload
# if_epair at once.
#
# Why two jails: a dying vnet's vnet_destroy() holds ifnet_detach_sxlock
# and runs vnet_sysuninit() with vnet_sysinit_sxlock shared, which takes
# a while.  kldunload queues on vnet_sysinit_sxlock as a writer in
# vnet_deregister_sysuninit(); sx prefers the queued writer, so it gets
# the lock the moment the first jail's chain drops it.  The jail_remove
# taskqueue then starts the second jail's vnet_destroy(): it takes
# ifnet_detach_sxlock and blocks on the sysinit lock.  kldunload, holding
# the sysinit lock, reaches if_clone_detach() and blocks on
# ifnet_detach_sxlock.  Both sit in D state for ever (wchan ifnet_de for
# the kldunload) and every later vnet jail creation or removal hangs.
#
# A deadlocked machine must be reset; `sysctl debug.kdb.panic=1` first if
# a dump is wanted.  Run as root on a disposable machine.
# ROUNDS (default 200): rounds to attempt.  Exit 2 on a deadlock.
ROUNDS=${ROUNDS:-200}
n=0
while [ $n -lt $ROUNDS ]; do
	n=$((n + 1))
	echo "round $n start"
	kldload -n if_epair || exit 1
	ea=$(ifconfig epair create) || exit 1
	eb=$(ifconfig epair create) || exit 1
	jail -c name=lor1 vnet persist >/dev/null || exit 1
	jail -c name=lor2 vnet persist >/dev/null || exit 1
	ifconfig "$ea" vnet lor1 || exit 1
	ifconfig "$eb" vnet lor2 || exit 1
	jexec lor1 ifconfig "$ea" 10.99.1.1/24 up
	jexec lor2 ifconfig "$eb" 10.99.2.1/24 up
	# A process in each jail: jail -r then kills it and returns, and the
	# vnet dies asynchronously when the process exits, a few ms later.
	# Without one, jail -r destroys the vnet synchronously before it
	# returns and the unload can never overlap (0 of 200 rounds).
	jexec lor1 sleep 1000 &
	sleep 0.2
	# jail -r lor1 returns at once; the vnet dies on the jail_remove
	# taskqueue when the sleeper exits (a chain of ~30 ms on a debug
	# kernel).  The unload starts concurrently so that it queues on the
	# sysinit lock during that chain; jail -r lor2 then waits for the
	# detach lock behind the first chain and is the vnet_destroy that
	# closes the cycle.
	jail -r lor1
	kldunload if_epair &
	kpid=$!
	jail -r lor2
	i=0
	while [ $i -lt 100 ]; do
		st=$(ps -o stat= -p $kpid 2>/dev/null)
		[ -z "$st" ] && break		# gone (reaped below)
		case "$st" in Z*) break ;; esac
		sleep 0.1
		i=$((i + 1))
	done
	if [ $i -ge 100 ]; then
		# Not reached in practice: `jail -r lor2` above is one of the two
		# deadlocked threads and this shell hangs in it; detect the
		# deadlock from outside (repro-driver.sh), by a kldunload in D
		# state on wchan ifnet_de and a jail in D state on vnet_sys.
		echo "round $n: kldunload if_epair hung for 10 s: deadlocked"
		ps -o pid,stat,wchan,etime,command -p $kpid
		procstat -kk -a | grep -E "if_clone_detach|vnet_destroy"
		jls -d jid name dying
		exit 2
	fi
	wait $kpid
	[ $((n % 20)) -eq 0 ] && echo "round $n ok"
done
echo "no deadlock in $ROUNDS rounds"
exit 0
