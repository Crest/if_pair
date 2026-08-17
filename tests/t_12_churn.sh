#!/bin/sh
# Corner case: bulk create/destroy churn.  Creates CHURN_N pairs
# (default 30), destroys them all via their b sides, and verifies
# the pair group is exactly as it was before the test.
. "$(dirname "$0")/lib.sh"
test_init

N=${CHURN_N:-30}
before=$(ifconfig -g pair 2>/dev/null | sort)

made=""
i=0
while [ "$i" -lt "$N" ]; do
	a=$(ifconfig pair create) || fail "create number $i failed"
	made="$made $a"
	i=$((i + 1))
done
log "ok: created $N pairs"

for a in $made; do
	ifconfig "${a%a}b" destroy || fail "destroy of ${a%a}b failed"
done
log "ok: destroyed all $N pairs via their b sides"

after=$(ifconfig -g pair 2>/dev/null | sort)
if [ "$before" != "$after" ]; then
	fail "pair group changed: before [$before] after [$after]"
fi
log "ok: pair group membership unchanged"
pass
