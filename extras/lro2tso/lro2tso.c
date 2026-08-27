/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jan Bramkamp <crest+freebsd@rlwinm.de>
 *
 * lro2tso - pfil prototype: convert software-LRO aggregates into
 * TSO-marked frames at the IP input hook, so the forwarding path's
 * TSO exemptions apply to them and a TSO-capable egress re-splits
 * them in hardware.  Proof of concept for the "record segsz at
 * merge, convert at egress" design recorded in if_pair's NOTES.md;
 * the proper home for this logic is tcp_lro_flush(), this module
 * exists to measure the effect without patching the base system.
 *
 * Mechanism.  A tcp_lro(9) aggregate arrives at the inet/inet6
 * pfil input head (which ip_tryforward() runs BEFORE its MTU
 * check) carrying receive marks (CSUM_DATA_VALID|CSUM_PSEUDO_HDR)
 * and the merged segment count.  The count lives in
 * m_pkthdr.lro_nsegs, which is a #define ALIAS of tso_segsz
 * (mbuf.h) - the same 16-bit field holds "segments merged" inbound
 * and "split size" outbound.  The conversion therefore must read
 * the count and overwrite the field with the derived segment size
 * in that order, and a frame missing the conversion must never be
 * handed to TSO (the count would be interpreted as a segment
 * size).  We derive
 *
 *     segsz = floor(tcp_payload / nsegs)
 *
 * which is provably <= the largest original segment <= the
 * sender's MSS, so re-split frames never exceed what the endpoints
 * negotiated (NOTES.md, router-safe LRO item 1; exact recording at
 * merge time would replace the division).
 *
 * The receive checksum marks are CONVERTED, not kept: csum_data
 * cannot hold both the validated-checksum value (receive
 * semantics) and the th_sum field offset (transmit semantics).
 * After conversion the frame carries the transmit request bits
 * (CSUM_TCP|CSUM_TSO) with th_sum rewritten to the pseudo-header
 * form tcp_output() uses.  A frame that ends up delivered LOCALLY
 * is then accepted by tcp_input()'s request-bit branch ("packet
 * from local host, checksum not required") - acceptable because
 * the NIC/LRO already verified the wire checksum before merging.
 *
 * Guards: only frames with >= 2 merged segments that still carry
 * the LRO receive marks and are plain TCP (no v4 fragments, no v6
 * extension headers, no flags beyond ACK|PSH - tcp_lro merges
 * nothing else; VXLAN-outer aggregates are skipped by the
 * plain-TCP requirement).  Anything unexpected passes unmodified.
 *
 * Caveats (prototype): egress if_hw_tsomaxsegcount is not
 * reconciled - a ~44-mbuf aggregate exceeds most NICs' gather
 * limits and takes iflib's EFBIG -> m_defrag copy path (ice at
 * 128 segments and if_pair do not care); an egress narrower than
 * segsz still balks (the clamp belongs at the forwarding hop);
 * hardware-LRO merges (mlx5/qlnx/bxe hw_lro) must stay disabled -
 * only the software engine's aggregates are shaped as assumed.
 */

#include "opt_inet.h"
#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/counter.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/pfil.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>

#include <machine/in_cksum.h>

SYSCTL_NODE(_net, OID_AUTO, lro2tso, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "LRO-aggregate to TSO-frame conversion");

static bool lro2tso_enabled = true;
SYSCTL_BOOL(_net_lro2tso, OID_AUTO, enable, CTLFLAG_RWTUN,
    &lro2tso_enabled, false,
    "Convert software-LRO aggregates to TSO frames at pfil input");

COUNTER_U64_DEFINE_EARLY(lro2tso_converted);
SYSCTL_COUNTER_U64(_net_lro2tso, OID_AUTO, converted, CTLFLAG_RD,
    &lro2tso_converted, "Aggregates converted to TSO frames");

COUNTER_U64_DEFINE_EARLY(lro2tso_skipped);
SYSCTL_COUNTER_U64(_net_lro2tso, OID_AUTO, skipped, CTLFLAG_RD,
    &lro2tso_skipped, "Multi-segment aggregates left unconverted");

VNET_DEFINE_STATIC(pfil_hook_t, lro2tso_inet_hook);
#define	V_lro2tso_inet_hook	VNET(lro2tso_inet_hook)
VNET_DEFINE_STATIC(pfil_hook_t, lro2tso_inet6_hook);
#define	V_lro2tso_inet6_hook	VNET(lro2tso_inet6_hook)

/*
 * The convertible mark set: the software LRO engine's output.  A
 * frame already carrying CSUM_TSO (e.g. a forwarded deliver-whole
 * frame from if_pair) is not ours to touch and is excluded before
 * this test is consulted.
 */
#define	LRO2TSO_RX_MARKS	(CSUM_DATA_VALID | CSUM_PSEUDO_HDR)
#define	LRO2TSO_RX_CLEAR	(LRO2TSO_RX_MARKS | CSUM_IP_CHECKED | \
				 CSUM_IP_VALID)

static pfil_return_t
lro2tso_chk(struct mbuf **mp, int af)
{
	struct mbuf *m = *mp;
	struct tcphdr *th;
	uint32_t tcplen, paylen;
	u_int hlen, thlen, nsegs, segsz;

	if (!lro2tso_enabled)
		return (PFIL_PASS);

	/*
	 * lro_nsegs aliases tso_segsz: >= 2 only ever means "LRO
	 * merged this"; untouched frames carry 0 or 1 and TSO
	 * frames are excluded by the CSUM_TSO test.
	 */
	if ((m->m_flags & M_PKTHDR) == 0 ||
	    m->m_pkthdr.lro_nsegs < 2 ||
	    (m->m_pkthdr.csum_flags & CSUM_TSO) != 0 ||
	    (m->m_pkthdr.csum_flags & LRO2TSO_RX_MARKS) != LRO2TSO_RX_MARKS)
		return (PFIL_PASS);

	switch (af) {
#ifdef INET
	case AF_INET: {
		struct ip *ip;

		if (m->m_len < (int)sizeof(*ip))
			goto skip;
		ip = mtod(m, struct ip *);
		hlen = ip->ip_hl << 2;
		if (hlen < sizeof(*ip) || ip->ip_p != IPPROTO_TCP ||
		    (ip->ip_off & htons(IP_MF | IP_OFFMASK)) != 0)
			goto skip;
		if (m->m_len < (int)(hlen + sizeof(*th)))
			goto skip;
		th = (struct tcphdr *)(mtod(m, caddr_t) + hlen);
		tcplen = ntohs(ip->ip_len) - hlen;
		break;
	}
#endif
#ifdef INET6
	case AF_INET6: {
		struct ip6_hdr *ip6;

		if (m->m_len < (int)sizeof(*ip6))
			goto skip;
		ip6 = mtod(m, struct ip6_hdr *);
		/* Plain TCP only; extension chains are not walked. */
		if (ip6->ip6_nxt != IPPROTO_TCP)
			goto skip;
		hlen = sizeof(*ip6);
		if (m->m_len < (int)(hlen + sizeof(*th)))
			goto skip;
		th = (struct tcphdr *)(mtod(m, caddr_t) + hlen);
		tcplen = ntohs(ip6->ip6_plen);
		break;
	}
#endif
	default:
		goto skip;
	}

	thlen = th->th_off << 2;
	if (thlen < sizeof(*th) || tcplen <= thlen ||
	    m->m_len < (int)(hlen + thlen))
		goto skip;
	/* tcp_lro merges only ACK|PSH data runs; be paranoid. */
	if ((tcp_get_flags(th) & ~(TH_ACK | TH_PUSH)) != 0)
		goto skip;

	paylen = tcplen - thlen;
	nsegs = m->m_pkthdr.lro_nsegs;
	segsz = paylen / nsegs;
	if (segsz == 0)
		goto skip;

	/*
	 * Rewrite th_sum from the LRO-computed full checksum to the
	 * pseudo-header form tcp_output() provides to TSO consumers.
	 */
	switch (af) {
#ifdef INET
	case AF_INET: {
		struct ip *ip = mtod(m, struct ip *);

		th->th_sum = in_pseudo(ip->ip_src.s_addr, ip->ip_dst.s_addr,
		    htons(tcplen + IPPROTO_TCP));
		m->m_pkthdr.csum_flags = (m->m_pkthdr.csum_flags &
		    ~LRO2TSO_RX_CLEAR) | CSUM_IP_TCP | CSUM_IP_TSO;
		break;
	}
#endif
#ifdef INET6
	case AF_INET6: {
		struct ip6_hdr *ip6 = mtod(m, struct ip6_hdr *);

		th->th_sum = in6_cksum_pseudo(ip6, tcplen, IPPROTO_TCP, 0);
		m->m_pkthdr.csum_flags = (m->m_pkthdr.csum_flags &
		    ~LRO2TSO_RX_CLEAR) | CSUM_IP6_TCP | CSUM_IP6_TSO;
		break;
	}
#endif
	}
	m->m_pkthdr.csum_data = offsetof(struct tcphdr, th_sum);
	/* Order matters: this overwrites lro_nsegs (same field). */
	m->m_pkthdr.tso_segsz = segsz;

	counter_u64_add(lro2tso_converted, 1);
	return (PFIL_PASS);

skip:
	counter_u64_add(lro2tso_skipped, 1);
	return (PFIL_PASS);
}

#ifdef INET
static pfil_return_t
lro2tso_chk_inet(struct mbuf **mp, struct ifnet *ifp __unused,
    int flags __unused, void *ruleset __unused, struct inpcb *inp __unused)
{
	return (lro2tso_chk(mp, AF_INET));
}
#endif

#ifdef INET6
static pfil_return_t
lro2tso_chk_inet6(struct mbuf **mp, struct ifnet *ifp __unused,
    int flags __unused, void *ruleset __unused, struct inpcb *inp __unused)
{
	return (lro2tso_chk(mp, AF_INET6));
}
#endif

static void
lro2tso_hook(pfil_hook_t *hookp, pfil_mbuf_chk_t chk, enum pfil_types type,
    const char *rulname, const char *headname)
{
	struct pfil_hook_args pha = {
		.pa_version = PFIL_VERSION,
		.pa_flags = PFIL_IN,
		.pa_type = type,
		.pa_mbuf_chk = chk,
		.pa_modname = "lro2tso",
		.pa_rulname = rulname,
	};
	struct pfil_link_args pla = {
		.pa_version = PFIL_VERSION,
		.pa_flags = PFIL_IN | PFIL_HOOKPTR,
		.pa_headname = headname,
	};

	*hookp = pfil_add_hook(&pha);
	pla.pa_hook = *hookp;
	if (pfil_link(&pla) != 0)
		printf("lro2tso: could not link to %s\n", headname);
}

static void
vnet_lro2tso_init(const void *unused __unused)
{
#ifdef INET
	lro2tso_hook(&V_lro2tso_inet_hook, lro2tso_chk_inet,
	    PFIL_TYPE_IP4, "inet", "inet");
#endif
#ifdef INET6
	lro2tso_hook(&V_lro2tso_inet6_hook, lro2tso_chk_inet6,
	    PFIL_TYPE_IP6, "inet6", "inet6");
#endif
}
VNET_SYSINIT(vnet_lro2tso_init, SI_SUB_PROTO_PFIL, SI_ORDER_ANY,
    vnet_lro2tso_init, NULL);

static void
vnet_lro2tso_uninit(const void *unused __unused)
{
#ifdef INET
	pfil_remove_hook(V_lro2tso_inet_hook);
#endif
#ifdef INET6
	pfil_remove_hook(V_lro2tso_inet6_hook);
#endif
}
VNET_SYSUNINIT(vnet_lro2tso_uninit, SI_SUB_PROTO_PFIL, SI_ORDER_ANY,
    vnet_lro2tso_uninit, NULL);

static int
lro2tso_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
	case MOD_QUIESCE:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t lro2tso_mod = {
	"lro2tso",
	lro2tso_modevent,
	NULL,
};

DECLARE_MODULE(lro2tso, lro2tso_mod, SI_SUB_PROTO_PFIL, SI_ORDER_ANY);
MODULE_VERSION(lro2tso, 1);
