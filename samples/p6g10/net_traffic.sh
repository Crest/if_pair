#!/bin/sh
# net_traffic.sh net|netcpu: geli_bench's NET_CMD: traffic through the kpair
# pairs of net_setup.sh, "metric value" lines on stdout.  Two loads:
#   sparse  a ping every millisecond through every pair: the NET worker
#           sleeps between packets
#   dense   iperf3 through every pair and a ping every 5 ms beside it: the
#           NET worker stays busy
# Under both it yields at the end of a round if anything else is runnable on
# its CPU, the yield SCHED.md S6.4's BULK item sits in.
# net:    NP pairs, STREAMS connections each, each flow on the CPU of its
#         hash, nothing pinned.
# netcpu: the first pair, one connection, every packet on CPU PCPU by
#         net.link.kpair.cpu, the user threads kept off it (USERCPUS).
# Rows per load: the RTT's percentiles (us), the largest age of an element
# at a pass start of the pairs' queues (maxlat_us, what NET waited), NET
# yields and BULK hand-backs per second, and for dense Gbit/s, in total and
# of the slowest pair, the share of NET's items on PCPU.
M=${1:?net|netcpu}
: "${NP:=8}" "${STREAMS:=4}" "${NET_SECS:=12}" "${SPARSE_S:=5}" "${PCPU:=1}" "${USERCPUS:=2-15}" "${SETTINGS_LOG:=/dev/null}"
T=$(mktemp -d /tmp/net_traffic.XXXXXX) || exit 1
note() { echo "$(date -u +%FT%TZ) net_traffic: $*" >> "$SETTINGS_LOG"; }
done_() {	# however this ends, or the next stage's flows all go to PCPU
	[ $M = net ] || { sysctl -q net.link.kpair.cpu=-1 >/dev/null; note "set net.link.kpair.cpu=-1 (back)"; }
	rm -rf "$T"
}
trap done_ EXIT
case $M in
net) N=$NP; S=$STREAMS; PIN= ;;
netcpu) N=1; S=1; PIN="cpuset -l $USERCPUS"
	sysctl -q net.link.kpair.cpu="$PCPU" >/dev/null || exit 1
	note "set net.link.kpair.cpu=$PCPU" ;;
*) exit 2 ;;
esac

counters() {	# "NET yields, BULK hand-backs"
	{ sysctl kern.kwq.net.cpu; sysctl kern.kwq.bulk.cpu; } 2>/dev/null | awk -F': ' '
	    /^kern\.kwq\.net\.cpu\.[0-9]+\.(yields|tick_yields): / { y += $2 }
	    /^kern\.kwq\.bulk\.cpu\.[0-9]+\.handbacks: / { h += $2 }
	    END { printf("%.0f %.0f\n", y, h) }'
}
reset_q() {
	for q in $(sysctl -N kern.kwq.net.queue | sed -n 's/^kern\.kwq\.net\.queue\.\(kpair[0-9]*[ab]\)\.reset$/\1/p'); do
		sysctl -q kern.kwq.net.queue.$q.reset=1 >/dev/null
	done
}
maxlat() {	# PREFIX
	sysctl kern.kwq.net.queue 2>/dev/null | awk -F': ' -v p="$1" '
	    /^kern\.kwq\.net\.queue\.kpair[0-9]+[ab]\.cpu\.[0-9]+\.maxlat_ns: / { if ($2 + 0 > m) m = $2 + 0 }
	    END { printf("%s_maxlat_us %.0f\n", p, m / 1000) }'
}
items() {	# FILE: pairs' items per CPU
	sysctl kern.kwq.net.queue 2>/dev/null | awk -F': ' '
	    /^kern\.kwq\.net\.queue\.kpair[0-9]+[ab]\.cpu\.[0-9]+\.items: / { split($1, o, "."); c[o[7]] += $2 }
	    END { for (i in c) printf("%s %.0f\n", i, c[i]) }' > "$1"
}
rtt() {		# PREFIX FILE...: ping's per-packet times, in us
	p=$1; shift
	cat "$@" | awk -F'time=' '/time=/ { split($2, a, " "); print a[1] * 1000 }' | sort -n |
	    awk -v p="$p" '{ v[NR] = $1 }
	    function q(f) { i = int(NR * f); return v[i > 0 ? i : 1] }
	    END { printf("%s_rtt_n %d\n", p, NR); if (NR) printf("%s_rtt_p50_us %.0f\n%s_rtt_p90_us %.0f\n%s_rtt_p99_us %.0f\n%s_rtt_p999_us %.0f\n%s_rtt_max_us %.0f\n", p, q(.5), p, q(.9), p, q(.99), p, q(.999), p, v[NR]) }'
}

# sparse
reset_q; c0=$(counters); t0=$(date +%s)
i=1
while [ $i -le $N ]; do
	jexec ifp_kp_c$i $PIN ping -c $((SPARSE_S * 1000)) -i 0.001 10.99.$i.1 > $T/sp.$i 2>&1 &
	i=$((i + 1))
done
wait
t1=$(date +%s); c1=$(counters)
rtt sparse $T/sp.*
maxlat sparse
echo "$c0 $c1 $((t1 - t0))" | awk '{ d = $5 > 0 ? $5 : 1; printf("sparse_net_yields_s %.0f\nsparse_bulk_handbacks_s %.0f\n", ($3 - $1) / d, ($4 - $2) / d) }'

# dense
i=1; srv=""
while [ $i -le $N ]; do
	jexec ifp_kp_s$i $PIN iperf3 -s -1 -p 5301 > /dev/null 2>&1 &
	srv="$srv $!"
	i=$((i + 1))
done
sleep 1
reset_q; items $T/i0; c0=$(counters); t0=$(date +%s)
i=1; cl=""
while [ $i -le $N ]; do
	jexec ifp_kp_c$i $PIN timeout $((NET_SECS + 30)) iperf3 -f g -c 10.99.$i.1 -p 5301 -P "$S" -t "$NET_SECS" > $T/ip.$i 2>&1 &
	cl="$cl $!"
	i=$((i + 1))
done
sleep 2
i=1
while [ $i -le $N ]; do
	jexec ifp_kp_c$i $PIN ping -c $(( (NET_SECS - 4) * 200 )) -i 0.005 10.99.$i.1 > $T/dp.$i 2>&1 &
	cl="$cl $!"
	i=$((i + 1))
done
wait $cl
t1=$(date +%s); c1=$(counters); items $T/i1
wait $srv
rtt dense $T/dp.*
maxlat dense
echo "$c0 $c1 $((t1 - t0))" | awk '{ d = $5 > 0 ? $5 : 1; printf("dense_net_yields_s %.0f\ndense_bulk_handbacks_s %.0f\n", ($3 - $1) / d, ($4 - $2) / d) }'
for f in $T/ip.*; do
	awk '/receiver/ { for (i = 1; i <= NF; i++) if ($i == "Gbits/sec") { r = $(i - 1); if ($0 ~ /^\[SUM\]/) s = r } }
	    END { print (s != "" ? s : (r != "" ? r : -1)) }' $f
done | awk '{ s += $1; if (NR == 1 || $1 < m) m = $1 } END { printf("dense_gbps %.2f\ndense_pair_min_gbps %.2f\n", s, m) }'
awk -v c="$PCPU" 'NR == FNR { a[$1] = $2; next } { d = $2 - a[$1]; t += d; if ($1 == c) p = d }
    END { printf("dense_items %.0f\ndense_items_pcpu_pct %.1f\n", t, t > 0 ? p * 100 / t : 0) }' $T/i0 $T/i1
