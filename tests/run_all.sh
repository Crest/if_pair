#!/bin/sh
# run_all.sh - run every t_*.sh in this directory, in order.  Tests
# clean up after themselves on success; a failed test leaves its
# state behind by design (sweep with cleanup.sh after debugging).
# Test names are unique per test, so the run continues past
# failures.  Exits nonzero if any test failed.
dir=$(dirname "$0")

npass=0; nfail=0; failed=""
for t in "$dir"/t_*.sh; do
	name=$(basename "$t")
	echo "==> $name"
	if sh "$t"; then
		npass=$((npass + 1))
	else
		nfail=$((nfail + 1))
		failed="$failed $name"
	fi
	echo ""
done

echo "passed $npass, failed $nfail${failed:+:$failed}"
[ "$nfail" -eq 0 ]
