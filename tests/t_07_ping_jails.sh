#!/bin/sh
# Expected use: both sides jailed - IPv4 between two vnet jails.
. "$(dirname "$0")/lib.sh"
test_init

A4=192.0.2.5; B4=192.0.2.6
J1=$(jname 1); J2=$(jname 2)
create_pair
mkjail "$J1"
mkjail "$J2"
must "move ${PAIRA} into ${J1}" ifconfig "$PAIRA" vnet "$J1"
must "move ${PAIRB} into ${J2}" ifconfig "$PAIRB" vnet "$J2"
must "address side a" jexec "$J1" ifconfig "$PAIRA" inet "${A4}/32" "$B4" up
must "address side b" jexec "$J2" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "jail1 -> jail2 ping" jexec "$J1" ping -q -o -c 3 -t 2 "$B4"
must_retry 5 "jail2 -> jail1 ping" jexec "$J2" ping -q -o -c 3 -t 2 "$A4"
pass
