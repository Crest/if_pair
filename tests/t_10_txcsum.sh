#!/bin/sh
# Corner case: toggling transmit checksum offload on either or both
# sides must never break traffic (the documented ipfw-nat workaround
# path).
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

must "disable txcsum on a" ifconfig "$PAIRA" -txcsum -txcsum6
must_retry 5 "ping with a-side offload off" ping -q -o -c 3 -t 2 "$B4"
must "disable txcsum on b" jexec "$J1" ifconfig "$PAIRB" -txcsum -txcsum6
must_retry 5 "ping with both offloads off" ping -q -o -c 3 -t 2 "$B4"
must "re-enable txcsum on a" ifconfig "$PAIRA" txcsum txcsum6
must "re-enable txcsum on b" jexec "$J1" ifconfig "$PAIRB" txcsum txcsum6
must_retry 5 "ping with offloads restored" ping -q -o -c 3 -t 2 "$B4"
pass
