#!/bin/sh
# Corner case: destroying a pair while a flood ping runs across it
# from inside the jail - the shape of the load that panicked the old
# inline-dispatch design.  Must succeed with no leftovers.
. "$(dirname "$0")/lib.sh"
test_init

A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1)
create_pair
mkjail "$J1"
must "move ${PAIRB} into ${J1}" ifconfig "$PAIRB" vnet "$J1"
must "address the host side" ifconfig "$PAIRA" inet "${A4}/32" "$B4"
must "address the jail side" \
    jexec "$J1" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "baseline ping" ping -q -o -c 3 -t 2 "$B4"

# timeout(1) bounds the flood so a test failure cannot orphan it.
jexec "$J1" timeout 30 ping -f "$A4" >/dev/null 2>&1 &
FLOOD=$!
sleep 2
must "destroy under load" ifconfig "$PAIRA" destroy
kill "$FLOOD" 2>/dev/null
mustfail "${PAIRA} is gone" ifconfig "$PAIRA"
mustfail "${PAIRB} is gone from the jail" jexec "$J1" ifconfig "$PAIRB"
pass
