#!/bin/sh
# Corner case: module unload with live pairs, one of them with its b
# side inside a jail.  The cloner detach loop must destroy them all,
# and afterwards nothing of the module may remain (the four-line
# verification from the VM test plan).
#
# Skipped when pair interfaces we did not create exist (unloading
# would destroy them) or when the module was preloaded and no
# if_pair.ko is available to reload it afterwards.
. "$(dirname "$0")/lib.sh"
test_init

if [ -n "$(ifconfig -g pair 2>/dev/null)" ]; then
	log "SKIP: pre-existing pair interfaces; refusing to unload"
	exit 0
fi
KO=$(resolve_ko) || KO=""
if [ "$MOD_PRELOADED" = yes ] && [ -z "$KO" ]; then
	log "SKIP: module preloaded and no if_pair.ko found to reload"
	exit 0
fi

create_pair
A1=$PAIRA
J1=$(jname 1)
mkjail "$J1"
create_pair
A2=$PAIRA; B2=$PAIRB
must "move ${B2} into ${J1}" ifconfig "$B2" vnet "$J1"

must "kldunload with live pairs" kldunload if_pair
mustfail "no leaked-memory warning" \
    sh -c "dmesg | tail -50 | grep -iq 'if_pair.*leaked'"
# Worker threads may still be mid-exit for a moment after kldunload
# returns: taskqueue_terminate() returns once the thread count hits
# zero, while the threads themselves are still inside kthread_exit()
# and remain visible to procstat until the scheduler reaps them.
# The pattern is anchored on leading whitespace because a bare
# "pair_task" also matches epair(4)'s "epair_task" thread.
must_retry 10 "no pair_task worker threads left" \
    sh -c "! procstat -ta | grep -Eq '[[:space:]]pair_task_[0-9]'"
[ -z "$(ifconfig -g pair 2>/dev/null)" ] || \
    fail "pair group still has members after unload"
log "ok: pair group is empty"
mustfail "malloc type unregistered" sh -c "vmstat -m | grep -q if_pair"
mustfail "${A1} is gone" ifconfig "$A1"
mustfail "${A2} is gone" ifconfig "$A2"
mustfail "jailed ${B2} is gone" jexec "$J1" ifconfig "$B2"

if [ "$MOD_PRELOADED" = yes ]; then
	must "reload the module" kldload "$KO"
fi
pass
