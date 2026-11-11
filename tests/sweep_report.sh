#!/bin/sh
# sweep_report.sh - the sweep rows of a compare.sh result file, one table
# per metric.
#
#   sweep_report.sh OUT_TSV [REFERENCE_STAGE [METRIC ...]]
#
# A line per connection count N, a column per stage: the median over the
# repetitions and in brackets their range, and for every stage but the
# reference (the first stage unless given) the change of the median against
# the reference's, in percent (a difference for lost and where the
# reference is 0).  A "!" marks a stage whose range does not overlap the
# reference's: with five repetitions of each that is an exact permutation
# test's smallest two-sided p, 0.008 (0.1 with three); with about a hundred
# cells, a few marks are expected by chance.  The METRICs default to the
# throughput, latency, jitter, context switch and CPU rows below.
[ -f "${1:-}" ] || { echo "usage: sweep_report.sh OUT_TSV [REFERENCE_STAGE [METRIC ...]]" >&2; exit 1; }
TSV=$1; REF=${2:-}
if [ $# -gt 2 ]; then shift 2; else set --; fi
METRICS=${*:-"gbit gbit_cv gbit_icv retrans tcp_rtt_med tcp_rtt_max rtt_p50 rtt_p99 rtt_p999 rtt_max rtt_sd rtt_ipdv lost cs_s cs_drv_vol_s cs_drv_invol_s cs_rcv_vol_s cs_snd_vol_s m_sys m_idle"}
awk -F'\t' -v ref="$REF" -v metrics="$METRICS" '
function sorted(k,    c, i, j, t) {	# the values of k ascending in w[1..c]; returns c
	c = cnt[k]
	for (i = 1; i <= c; i++) w[i] = val[k, i] + 0
	for (i = 2; i <= c; i++) for (j = i; j > 1 && w[j-1] > w[j]; j--) { t = w[j]; w[j] = w[j-1]; w[j-1] = t }
	return c
}
function med(c) { return (c % 2) ? w[(c + 1) / 2] : (w[c / 2] + w[c / 2 + 1]) / 2 }
function fmt(x) { return (x >= 1000 || x <= -1000) ? sprintf("%.0f", x) : sprintf("%.3g", x) }
$3 ~ /^sweep[0-9]+$/ {
	n = substr($3, 6) + 0
	if (!($1 in seen)) { seen[$1] = ++ns; stage[ns] = $1 }
	if (!(n in nseen)) { nseen[n] = 1; N[++nn] = n }
	k = $1 SUBSEP n SUBSEP $4
	val[k, ++cnt[k]] = $5
	has[$4] = 1
}
END {
	if (nn == 0) { print "no sweep rows"; exit 1 }
	if (ref == "") ref = stage[1]
	if (!(ref in seen)) { print "no stage " ref; exit 1 }
	for (i = 2; i <= nn; i++) for (j = i; j > 1 && N[j-1] > N[j]; j--) { t = N[j]; N[j] = N[j-1]; N[j-1] = t }
	nm = split(metrics, M, " ")
	for (m = 1; m <= nm; m++) {
		if (!(M[m] in has)) continue
		printf("\n%s (median [min..max]; change against %s)\n%5s", M[m], ref, "N")
		for (s = 1; s <= ns; s++) printf(" | %-32s", stage[s])
		printf("\n")
		for (i = 1; i <= nn; i++) {
			for (s = 1; s <= ns; s++) if ((stage[s] SUBSEP N[i] SUBSEP M[m]) in cnt) break
			if (s > ns) continue	# no stage has the row at this N (the connections rows at N = 0)
			rk = ref SUBSEP N[i] SUBSEP M[m]; rm = ""
			if (rk in cnt) { c = sorted(rk); rm = med(c); rlo = w[1]; rhi = w[c] }
			printf("%5d", N[i])
			for (s = 1; s <= ns; s++) {
				k = stage[s] SUBSEP N[i] SUBSEP M[m]
				if (!(k in cnt)) { printf(" | %-32s", "-"); continue }
				c = sorted(k); md = med(c); lo = w[1]; hi = w[c]
				cell = fmt(md) " [" fmt(lo) ".." fmt(hi) "]"
				if (stage[s] != ref && rm != "") {
					d = md - rm
					if (rm != 0 && M[m] != "lost") cell = cell sprintf(" %+.1f%%", d / rm * 100)
					else cell = cell " " (d >= 0 ? "+" : "") fmt(d)
					if (lo > rhi || hi < rlo) cell = cell " !"
				}
				printf(" | %-32s", cell)
			}
			printf("\n")
		}
	}
}' "$TSV"
