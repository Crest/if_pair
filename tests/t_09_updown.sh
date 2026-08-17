#!/bin/sh
# Corner case: administratively downing one side stops traffic in
# both directions; bringing it back up restores it.
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

must "down the jail side" jexec "$J1" ifconfig "$PAIRB" down
mustfail "ping fails while the peer is down" ping -q -o -c 1 -t 1 "$B4"
must "up the jail side again" jexec "$J1" ifconfig "$PAIRB" up
must_retry 5 "ping works again" ping -q -o -c 3 -t 2 "$B4"
pass
