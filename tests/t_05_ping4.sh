#!/bin/sh
# Expected use: IPv4 between host and jail over a pair, addressed
# point-to-point style (local /32 plus peer destination).
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
must_retry 5 "host -> jail ping" ping -q -o -c 3 -t 2 "$B4"
must_retry 5 "jail -> host ping" jexec "$J1" ping -q -o -c 3 -t 2 "$A4"
pass
