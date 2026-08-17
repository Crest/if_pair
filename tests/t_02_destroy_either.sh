#!/bin/sh
# Expected use: destroying either side removes both interfaces.
. "$(dirname "$0")/lib.sh"
test_init

create_pair
must "destroy via the a side" ifconfig "$PAIRA" destroy
mustfail "${PAIRA} is gone" ifconfig "$PAIRA"
mustfail "${PAIRB} is gone" ifconfig "$PAIRB"

create_pair
must "destroy via the b side" ifconfig "$PAIRB" destroy
mustfail "${PAIRA} is gone" ifconfig "$PAIRA"
mustfail "${PAIRB} is gone" ifconfig "$PAIRB"
pass
