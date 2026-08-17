#!/bin/sh
# Corner case: the create-return-window race stress - regression
# test for the destroy-side wait-retry.  One loop churns an explicit
# unit, another hammers destroys of its b side, aiming for the gap
# between create's return and the framework linking the a side.
# Afterwards every member of the pair group must actually exist
# (ghost members are the old corruption's signature).
# RACE_SECS (default 20) sets the duration.
. "$(dirname "$0")/lib.sh"
test_init

U=89
SECS=${RACE_SECS:-20}
mustfail "unit ${U} is free before the test" ifconfig "pair${U}a"
cleanup_push "ifconfig pair${U}a destroy"

end=$(( $(date +%s) + SECS ))
(
	while [ "$(date +%s)" -lt "$end" ]; do
		ifconfig "pair${U}" create >/dev/null 2>&1
		ifconfig "pair${U}a" destroy >/dev/null 2>&1
	done
) &
churn=$!
(
	while [ "$(date +%s)" -lt "$end" ]; do
		ifconfig "pair${U}b" destroy >/dev/null 2>&1
	done
) &
hammer=$!
log "racing create/destroy for ${SECS}s ..."
wait "$churn" "$hammer"

ifconfig "pair${U}a" destroy >/dev/null 2>&1	# reap a survivor
mustfail "pair${U}a is gone" ifconfig "pair${U}a"
mustfail "pair${U}b is gone" ifconfig "pair${U}b"
for m in $(ifconfig -g pair 2>/dev/null); do
	must "group member $m exists" ifconfig "$m"
done
log "ok: no ghost members in the pair group"
pass
