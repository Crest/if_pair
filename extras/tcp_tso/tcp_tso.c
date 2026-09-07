/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jan Bramkamp <crest+freebsd@rlwinm.de>
 *
 * tcp_tso_chop() - split a CSUM_TSO-marked frame into wire-ready
 * TCP segments of tso_segsz payload bytes each.  The software
 * splitter FreeBSD never had as a callable function: the sfxge(4)
 * software-TSO arithmetic (sfxge_tx.c) emitting mbuf chains
 * instead of DMA descriptors, with the two gaps a shared facility
 * must close decided deliberately - IPv4 IDs INCREMENT per
 * segment (sfxge's XXX; RFC 6864 permits either for DF traffic,
 * incrementing matches hardware TSO) and checksums are
 * self-sufficient (hardware-assist or software, caller's choice).
 *
 * Framing: l2hlen bytes of opaque link header are replicated onto
 * every segment (0 for bare-IP consumers such as if_pair(4), gif,
 * gre; ETHER_HDR_LEN(+VLAN) for Ethernet consumers such as
 * epair(4) or an interposed NIC).  Link headers need no
 * per-segment fixup - same addresses, same type, every segment -
 * so both framings share this one parameter.  Worst-case copied
 * header (18 + 60 + 60) fits a single pkthdr mbuf.
 *
 * Contract (modeled on ip_fragment()):
 *   0                - *m0 consumed and set NULL; *chainp holds
 *                      the segments as an m_nextpkt chain for the
 *                      caller's send loop.  Segments carry the
 *                      original pkthdr copies (flowid, fib, tags
 *                      via m_dup_pkthdr) so NIC queue steering
 *                      and filter state survive.
 *   EPROTONOSUPPORT  - frame shape not handled (not plain
 *                      v4/v6 TCP, no tso_segsz, fragment bits,
 *                      or M_EXTPG payload without
 *                      TSO_CHOP_HWCSUM - see below).  *m0 is
 *                      still valid (possibly m_pullup-relocated);
 *                      the caller decides (pass through, drop).
 *   ENOBUFS          - allocation failed mid-chop; *m0 and any
 *                      partial chain are freed, *m0 set NULL.
 *
 * M_NOWAIT throughout: callers sit in transmit paths under the
 * network epoch.
 *
 * M_EXTPG policy (CHOPPER.txt P1 decision): header m_copydata and
 * payload m_copym are unmapped-aware, so EXTPG payloads chop fine
 * WHEN the egress computes checksums (TSO_CHOP_HWCSUM).  The
 * software checksum path walks payload bytes through m_data and
 * would fault on unmapped mbufs, so that combination is REJECTED
 * with EPROTONOSUPPORT rather than being silently wrong.  (In
 * practice EXTPG senders - sendfile, kTLS - run over NICs with
 * checksum offload.)  Note the segments still reference unmapped
 * pages; a consumer without IFCAP_MEXTPG handling remains the
 * caller's problem, as with any EXTPG transmit.
 *
 * Per-segment fixups: th_seq advances by the payload offset;
 * FIN|PSH are kept on the last segment only, CWR (ECN) on the
 * first only; v4 ip_len/ip_id and the always-computed v4 header
 * checksum, v6 ip6_plen; th_sum is recomputed per segment in
 * both checksum modes because the original pseudo-header sum
 * covered the whole chain.
 */

#include "opt_inet.h"
#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/systm.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>

#include <machine/in_cksum.h>

#include "tcp_tso.h"

/* Sanity bound for the opaque link header (ether + stacked tags). */
#define	TSO_CHOP_L2MAX		32

int
tcp_tso_chop(struct mbuf **m0, unsigned int l2hlen, unsigned int maxlen,
    int flags, struct mbuf **chainp)
{
	struct mbuf *m = *m0, *seg, *tail, *head, **prevp;
	struct ip *ip = NULL;
	struct ip6_hdr *ip6 = NULL;
	struct tcphdr *th;
	uint32_t seq;
	u_int hdrlen, iphl, thl, paylen, off, plen, segsz;
	uint16_t ipid = 0;
	bool v6, hwcsum, first;

	M_ASSERTPKTHDR(m);
	if (l2hlen > TSO_CHOP_L2MAX)
		return (EPROTONOSUPPORT);
	segsz = m->m_pkthdr.tso_segsz;
	if (segsz == 0)
		return (EPROTONOSUPPORT);
	hwcsum = (flags & TSO_CHOP_HWCSUM) != 0;
	if (!hwcsum && (m->m_flags & M_EXTPG) != 0)
		return (EPROTONOSUPPORT);

	/*
	 * Make the headers contiguous.  m_pullup() may relocate or,
	 * on failure, free the mbuf; keep *m0 current so the caller
	 * never holds a stale pointer.
	 */
#define	TSO_PULLUP(len)	do {					\
	if (m->m_len < (int)(len)) {				\
		m = m_pullup(m, (len));				\
		*m0 = m;					\
		if (m == NULL)					\
			return (ENOBUFS);			\
	}							\
} while (0)

	TSO_PULLUP(l2hlen + sizeof(struct ip));
	v6 = (*(mtod(m, uint8_t *) + l2hlen) >> 4) == 6;
	if (v6) {
		iphl = sizeof(struct ip6_hdr);
		TSO_PULLUP(l2hlen + iphl + sizeof(*th));
		ip6 = (struct ip6_hdr *)(mtod(m, caddr_t) + l2hlen);
		if (ip6->ip6_nxt != IPPROTO_TCP)
			return (EPROTONOSUPPORT);
	} else {
		ip = (struct ip *)(mtod(m, caddr_t) + l2hlen);
		iphl = ip->ip_hl << 2;
		if (iphl < sizeof(*ip) || ip->ip_p != IPPROTO_TCP ||
		    (ip->ip_off & htons(IP_MF | IP_OFFMASK)) != 0)
			return (EPROTONOSUPPORT);
		TSO_PULLUP(l2hlen + iphl + sizeof(*th));
		ip = (struct ip *)(mtod(m, caddr_t) + l2hlen);
		ipid = ntohs(ip->ip_id);
	}
	th = (struct tcphdr *)(mtod(m, caddr_t) + l2hlen + iphl);
	thl = th->th_off << 2;
	if (thl < sizeof(*th))
		return (EPROTONOSUPPORT);
	hdrlen = l2hlen + iphl + thl;
	TSO_PULLUP(hdrlen);
#undef TSO_PULLUP
	if (v6)
		ip6 = (struct ip6_hdr *)(mtod(m, caddr_t) + l2hlen);
	else
		ip = (struct ip *)(mtod(m, caddr_t) + l2hlen);
	th = (struct tcphdr *)(mtod(m, caddr_t) + l2hlen + iphl);

	if (m->m_pkthdr.len <= (int)hdrlen)
		return (EPROTONOSUPPORT);
	/*
	 * Router-case clamp: never emit a segment longer than the
	 * egress L3 MTU.  The clamp needs the parsed header sizes,
	 * which is why it lives here and not in the callers.
	 */
	if (maxlen != 0) {
		if (maxlen <= iphl + thl)
			return (EPROTONOSUPPORT);
		segsz = ulmin(segsz, maxlen - iphl - thl);
	}
	paylen = m->m_pkthdr.len - hdrlen;
	seq = ntohl(th->th_seq);

	head = NULL;
	prevp = &head;
	first = true;
	for (off = 0; off < paylen; off += plen, first = false) {
		struct ip *sip;
		struct ip6_hdr *sip6;
		struct tcphdr *sth;
		bool last;

		plen = ulmin(segsz, paylen - off);
		last = (off + plen == paylen);

		seg = m_gethdr(M_NOWAIT, MT_DATA);
		if (seg == NULL)
			goto fail;
		if (m_dup_pkthdr(seg, m, M_NOWAIT) == 0) {
			m_free(seg);
			goto fail;
		}
		m_copydata(m, 0, hdrlen, mtod(seg, caddr_t));
		seg->m_len = hdrlen;
		tail = m_copym(m, hdrlen + off, plen, M_NOWAIT);
		if (tail == NULL) {
			m_freem(seg);
			goto fail;
		}
		m_cat(seg, tail);
		seg->m_pkthdr.len = hdrlen + plen;

		/* Offload metadata: a plain (maybe csum-offload) frame. */
		seg->m_pkthdr.csum_flags &= ~(CSUM_TSO | CSUM_IP_TCP |
		    CSUM_IP6_TCP);
		seg->m_pkthdr.tso_segsz = 0;
		if (hwcsum) {
			seg->m_pkthdr.csum_flags |=
			    v6 ? CSUM_IP6_TCP : CSUM_IP_TCP;
			seg->m_pkthdr.csum_data =
			    offsetof(struct tcphdr, th_sum);
		}

		sth = (struct tcphdr *)(mtod(seg, caddr_t) + l2hlen + iphl);
		sth->th_seq = htonl(seq + off);
		if (!last)
			tcp_set_flags(sth, tcp_get_flags(sth) &
			    ~(TH_FIN | TH_PUSH));
		if (!first)
			tcp_set_flags(sth, tcp_get_flags(sth) & ~TH_CWR);
		if (v6) {
			sip6 = (struct ip6_hdr *)(mtod(seg, caddr_t) +
			    l2hlen);
			sip6->ip6_plen = htons(thl + plen);
			/*
			 * Pseudo-header sum first in both modes; the
			 * software path then folds the TCP bytes over
			 * it (in6_cksum() would assume the v6 header
			 * at offset zero and cannot serve l2-framed
			 * segments).
			 */
			sth->th_sum = in6_cksum_pseudo(sip6,
			    thl + plen, IPPROTO_TCP, 0);
			if (!hwcsum) {
				sth->th_sum = in_cksum_skip(seg,
				    hdrlen + plen, l2hlen + iphl);
			}
		} else {
			sip = (struct ip *)(mtod(seg, caddr_t) + l2hlen);
			sip->ip_len = htons(iphl + thl + plen);
			sip->ip_id = htons(ipid++);
			sth->th_sum = in_pseudo(sip->ip_src.s_addr,
			    sip->ip_dst.s_addr,
			    htons(thl + plen + IPPROTO_TCP));
			if (!hwcsum) {
				sth->th_sum = in_cksum_skip(seg,
				    hdrlen + plen, l2hlen + iphl);
			}
			sip->ip_sum = 0;
			if (l2hlen == 0) {
				sip->ip_sum = in_cksum(seg, iphl);
			} else {
				sip->ip_sum = in_cksum_skip(seg,
				    l2hlen + iphl, l2hlen);
			}
		}

		*prevp = seg;
		prevp = &seg->m_nextpkt;
	}

	m_freem(m);
	*m0 = NULL;
	*chainp = head;
	return (0);

fail:
	for (seg = head; seg != NULL; seg = tail) {
		tail = seg->m_nextpkt;
		seg->m_nextpkt = NULL;
		m_freem(seg);
	}
	m_freem(m);
	*m0 = NULL;
	return (ENOBUFS);
}
