#!/bin/sh
# compare_report.sh - table from a compare.sh result file.
#
#   compare_report.sh OUT_TSV [REFERENCE_STAGE]
#
# One row per test and metric: the median over repetitions for every stage,
# in the order the stages first appear, and in brackets the change against
# the reference stage (the first stage unless given) in percent.  Shares
# and counts are compared as differences in points where a ratio makes no
# sense (lost requests).
awk -F'\t' -v ref="${2:-}" '
function median(key,    n, i, j, t, v) {
	n = cnt[key]
	for (i = 1; i <= n; i++) v[i] = val[key, i]
	for (i = 2; i <= n; i++) for (j = i; j > 1 && v[j-1] > v[j]; j--) { t = v[j]; v[j] = v[j-1]; v[j-1] = t }
	return (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
}
{
	if (!($1 in seen)) { seen[$1] = ++ns; stage[ns] = $1 }
	tm = $3 "\t" $4
	if (!(tm in tseen)) { tseen[tm] = ++nt; test[nt] = tm }
	key = $1 SUBSEP tm
	val[key, ++cnt[key]] = $5
}
END {
	if (ref == "") ref = stage[1]
	printf("%-24s", "test metric")
	for (s = 1; s <= ns; s++) printf(" | %18s", stage[s])
	printf("\n")
	for (t = 1; t <= nt; t++) {
		split(test[t], tmv, "\t")
		printf("%-24s", tmv[1] " " tmv[2])
		r = median(ref SUBSEP test[t])
		for (s = 1; s <= ns; s++) {
			key = stage[s] SUBSEP test[t]
			if (!(key in cnt)) { printf(" | %18s", "-"); continue }
			m = median(key)
			if (stage[s] == ref) printf(" | %18s", sprintf("%.3g", m))
			else if (tmv[2] == "lost" || r == 0) printf(" | %18s", sprintf("%.3g (%+.3g)", m, m - r))
			else printf(" | %18s", sprintf("%.3g (%+.0f%%)", m, (m - r) / r * 100))
		}
		printf("\n")
	}
}' "$1"
