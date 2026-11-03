#!/bin/sh
# geli_report.sh - geli_bench.sh's rows as tables: one line per metric of
# each test, one column per stage, the median over the repetitions with
# their range, and for every stage but the reference (the first stage of
# the file unless given) the change of the median against the
# reference's, in percent.  A "!" marks ranges that do not overlap the
# reference's (with five repetitions each, an exact permutation test's
# smallest two-sided p, 0.008; 0.1 with three).
#
#   geli_report.sh TSV [REFERENCE_STAGE]
[ -f "${1:-}" ] || { echo "usage: geli_report.sh TSV [REFERENCE_STAGE]" >&2; exit 1; }
awk -F'\t' -v ref="${2:-}" '
function sorted(k,    c, i, j, t) {
	c = cnt[k]
	for (i = 1; i <= c; i++) w[i] = val[k, i] + 0
	for (i = 2; i <= c; i++) for (j = i; j > 1 && w[j-1] > w[j]; j--) { t = w[j]; w[j] = w[j-1]; w[j-1] = t }
	return c
}
function med(c) { return (c % 2) ? w[(c + 1) / 2] : (w[c / 2] + w[c / 2 + 1]) / 2 }
function fmt(x) { return (x >= 1000 || x <= -1000) ? sprintf("%.0f", x) : sprintf("%.3g", x) }
{
	if (!($1 in seen)) { seen[$1] = ++ns; stage[ns] = $1 }
	m = $3 "." $4
	if (!(m in mseen)) { mseen[m] = ++nm; metric[nm] = m }
	k = $1 SUBSEP m
	val[k, ++cnt[k]] = $5
}
END {
	if (ref == "") ref = stage[1]
	if (!(ref in seen)) { print "no stage " ref; exit 1 }
	printf("%-28s", "test.metric")
	for (s = 1; s <= ns; s++) printf(" | %-30s", stage[s])
	printf("\n")
	last = ""
	for (i = 1; i <= nm; i++) {
		m = metric[i]
		split(m, part, ".")
		if (part[1] != last) { printf("\n"); last = part[1] }
		rk = ref SUBSEP m; rm = ""
		if (rk in cnt) { c = sorted(rk); rm = med(c); rlo = w[1]; rhi = w[c] }
		printf("%-28s", m)
		for (s = 1; s <= ns; s++) {
			k = stage[s] SUBSEP m
			if (!(k in cnt)) { printf(" | %-30s", "-"); continue }
			c = sorted(k); md = med(c); lo = w[1]; hi = w[c]
			cell = fmt(md) " [" fmt(lo) ".." fmt(hi) "]"
			if (stage[s] != ref && rm != "" && rm != 0) {
				cell = cell sprintf(" %+.0f%%", (md - rm) / rm * 100)
				if (lo > rhi || hi < rlo) cell = cell " !"
			}
			printf(" | %-30s", cell)
		}
		printf("\n")
	}
}' "$1"
