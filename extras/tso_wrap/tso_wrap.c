/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jan Bramkamp <crest+freebsd@rlwinm.de>
 *
 * tso_wrap - out-of-tree software TSO by interface interposition.
 *
 * Attaching to an interface (sysctl net.tso_wrap.control, write
 * the interface name; "-name" detaches; reading lists attachments)
 * does two things:
 *
 *  1. ADVERTISE: IFCAP_TSO4|TSO6 are set in the interface's
 *     capabilities and capenable, CSUM_IP_TSO|CSUM_IP6_TSO in its
 *     hwassist, and the stack-default chain limits (65518/35/2048)
 *     in its if_hw_tsomax fields.  From that moment tcp_maxmtu()
 *     grants TF_TSO to connections routed through the interface
 *     and ip_output()'s oversize exemption passes CSUM_TSO frames
 *     toward it (with patches/routed-tso-forwarding.patch the
 *     forwarding paths do too).  The original values are saved.
 *
 *  2. INTERPOSE: the interface's if_output pointer is replaced.
 *     The wrapper chops any CSUM_TSO frame larger than the MTU
 *     into tso_segsz-sized segments - the sfxge software-TSO
 *     arithmetic relocated above an arbitrary driver, emitting
 *     mbufs instead of DMA descriptors - and hands each segment
 *     to the saved original if_output.
 *
 * The placement is the whole design: if_output receives the
 * destination sockaddr and the route, so unlike a pfil hook the
 * chopper has the nexthop in hand - synchronous pf-shaped custody
 * with none of pf's context machinery (NOTES.md, custody
 * analysis).  Segments reuse the caller's dst/ro; per-flow NIC
 * queue steering is preserved by m_dup_pkthdr() carrying the
 * flowid.
 *
 * Checksums: ip_output() has already completed in software any
 * checksum the interface's hwassist does not claim, but for a TSO
 * frame the pseudo-header sum in th_sum covers the WHOLE chain,
 * so the wrapper recomputes th_sum per segment either way: as a
 * per-segment pseudo-header sum when the interface offers
 * hardware TCP checksumming (request bits kept), or as a full
 * software checksum otherwise.  The IPv4 header checksum is
 * always computed here (nothing downstream would).  IP IDs
 * increment per segment (the choice sfxge left as an XXX;
 * RFC 6864 permits either for DF traffic, incrementing matches
 * hardware TSO).  FIN|PSH are kept only on the last segment, CWR
 * only on the first (ECN).
 *
 * Caveats: capenable/hwassist are poked directly rather than via
 * the driver's SIOCSIFCAP path, so a driver whose ioctl handler
 * later rewrites them (ifconfig toggling other offloads) may
 * clear the advertisement - reattach after reconfiguring.  Only
 * plain IPv4/IPv6 TCP without extension headers is segmented
 * (matching what tcp_output() builds).  Detach restores the
 * original output pointer first and waits out the net epoch
 * before freeing state, so in-flight wrapper calls complete
 * safely; interface departure auto-detaches.
 */

#include "opt_inet.h"
#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/counter.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>	/* read if_output; drivers do this too */
#include <net/ethernet.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>

#include <machine/in_cksum.h>

#include "tcp_tso.h"

static MALLOC_DEFINE(M_TSOWRAP, "tso_wrap", "software TSO interposer");

struct tw_entry {
	CK_LIST_ENTRY(tw_entry)	 te_list;
	struct ifnet		*te_ifp;
	int (*te_output)(struct ifnet *, struct mbuf *,
	    const struct sockaddr *, struct route *);
	/* Saved advertisement state for restore on detach. */
	int			 te_capabilities;
	int			 te_capenable;
	uint64_t		 te_hwassist;
	u_int			 te_tsomax;
	u_int			 te_tsomaxsegcnt;
	u_int			 te_tsomaxsegsz;
};

static CK_LIST_HEAD(, tw_entry) tw_entries = CK_LIST_HEAD_INITIALIZER();
static struct sx tw_sx;
SX_SYSINIT(tw_sx, &tw_sx, "tso_wrap control");

SYSCTL_NODE(_net, OID_AUTO, tso_wrap, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "software TSO by if_output interposition");

COUNTER_U64_DEFINE_EARLY(tw_chopped);
SYSCTL_COUNTER_U64(_net_tso_wrap, OID_AUTO, chopped, CTLFLAG_RD,
    &tw_chopped, "TSO frames segmented in software");
COUNTER_U64_DEFINE_EARLY(tw_segments);
SYSCTL_COUNTER_U64(_net_tso_wrap, OID_AUTO, segments, CTLFLAG_RD,
    &tw_segments, "Segments produced");
COUNTER_U64_DEFINE_EARLY(tw_swcsum);
SYSCTL_COUNTER_U64(_net_tso_wrap, OID_AUTO, sw_csum, CTLFLAG_RD,
    &tw_swcsum, "Segments checksummed in software");
COUNTER_U64_DEFINE_EARLY(tw_drops);
SYSCTL_COUNTER_U64(_net_tso_wrap, OID_AUTO, drops, CTLFLAG_RD,
    &tw_drops, "TSO frames dropped (allocation or parse failure)");

/* Stack-default TSO limits, as in if_attach() and if_pair(4). */
#define	TW_TSO_MAXLEN		65518
#define	TW_TSO_MAXSEGCNT	35
#define	TW_TSO_MAXSEGSZ		2048

/*
 * Find the entry for ifp.  Datapath lookups run under the caller's
 * network epoch (every if_output entry path holds it); the control
 * path uses tw_sx and waits out the epoch before freeing.
 */
static struct tw_entry *
tw_lookup(struct ifnet *ifp)
{
	struct tw_entry *te;

	CK_LIST_FOREACH(te, &tw_entries, te_list)
		if (te->te_ifp == ifp)
			return (te);
	return (NULL);
}

/*
 * Chop one CSUM_TSO frame via the shared tcp_tso_chop() KPI
 * (../tcp_tso) and transmit each segment through the saved
 * original output function.  Consumes m except when the KPI
 * declines the frame's shape (EPROTONOSUPPORT), which is passed
 * through to the driver unmodified.
 */
static int
tw_chop(struct tw_entry *te, struct ifnet *ifp, struct mbuf *m,
    const struct sockaddr *dst, struct route *ro)
{
	struct mbuf *chain, *seg, *next;
	uint8_t vers;
	bool hwcsum;
	int error;

	vers = m->m_len >= 1 ? *mtod(m, uint8_t *) >> 4 : 0;
	hwcsum = (ifp->if_hwassist &
	    (vers == 6 ? CSUM_IP6_TCP : CSUM_IP_TCP)) != 0;

	error = tcp_tso_chop(&m, 0, ifp->if_mtu,
	    hwcsum ? TSO_CHOP_HWCSUM : 0, &chain);
	if (error == EPROTONOSUPPORT) {
		/* Not a shape we segment; the driver sees it as-is. */
		return (te->te_output(ifp, m, dst, ro));
	}
	if (error != 0) {
		counter_u64_add(tw_drops, 1);
		return (error);
	}

	counter_u64_add(tw_chopped, 1);
	for (seg = chain; seg != NULL; seg = next) {
		next = seg->m_nextpkt;
		seg->m_nextpkt = NULL;
		counter_u64_add(tw_segments, 1);
		if (!hwcsum)
			counter_u64_add(tw_swcsum, 1);
		if (error == 0)
			error = te->te_output(ifp, seg, dst, ro);
		else
			m_freem(seg);
	}
	return (error);
}

static int
tw_output(struct ifnet *ifp, struct mbuf *m, const struct sockaddr *dst,
    struct route *ro)
{
	struct tw_entry *te;

	NET_EPOCH_ASSERT();
	te = tw_lookup(ifp);
	if (__predict_false(te == NULL)) {
		/* Detach raced us; the restored pointer is current. */
		return (ifp->if_output(ifp, m, dst, ro));
	}
	if ((m->m_flags & M_PKTHDR) == 0 ||
	    (m->m_pkthdr.csum_flags & CSUM_TSO) == 0 ||
	    m->m_pkthdr.len <= (int)ifp->if_mtu)
		return (te->te_output(ifp, m, dst, ro));
	return (tw_chop(te, ifp, m, dst, ro));
}

static int
tw_attach(struct ifnet *ifp)
{
	struct tw_entry *te;

	sx_assert(&tw_sx, SA_XLOCKED);
	if (tw_lookup(ifp) != NULL)
		return (EEXIST);
	te = malloc(sizeof(*te), M_TSOWRAP, M_WAITOK | M_ZERO);
	te->te_ifp = ifp;
	te->te_output = ifp->if_output;
	te->te_capabilities = if_getcapabilities(ifp);
	te->te_capenable = if_getcapenable(ifp);
	te->te_hwassist = if_gethwassist(ifp);
	te->te_tsomax = if_gethwtsomax(ifp);
	te->te_tsomaxsegcnt = if_gethwtsomaxsegcount(ifp);
	te->te_tsomaxsegsz = if_gethwtsomaxsegsize(ifp);

	/* Advertise before interposing: grants only, no traffic yet. */
	if_setcapabilitiesbit(ifp, IFCAP_TSO4 | IFCAP_TSO6, 0);
	if_setcapenablebit(ifp, IFCAP_TSO4 | IFCAP_TSO6, 0);
	if_sethwassistbits(ifp, CSUM_IP_TSO | CSUM_IP6_TSO, 0);
	if_sethwtsomax(ifp, TW_TSO_MAXLEN);
	if_sethwtsomaxsegcount(ifp, TW_TSO_MAXSEGCNT);
	if_sethwtsomaxsegsize(ifp, TW_TSO_MAXSEGSZ);

	CK_LIST_INSERT_HEAD(&tw_entries, te, te_list);
	if_setoutputfn(ifp, tw_output);
	if_printf(ifp, "tso_wrap attached\n");
	return (0);
}

static void
tw_detach(struct tw_entry *te)
{
	struct ifnet *ifp = te->te_ifp;

	sx_assert(&tw_sx, SA_XLOCKED);
	/*
	 * Stop new wrapper entries, then wait out callers already
	 * inside tw_output() before unlinking and freeing.
	 */
	if_setoutputfn(ifp, te->te_output);
	NET_EPOCH_WAIT();
	CK_LIST_REMOVE(te, te_list);

	if_setcapabilities(ifp, te->te_capabilities);
	if_setcapenable(ifp, te->te_capenable);
	if_sethwassist(ifp, te->te_hwassist);
	if_sethwtsomax(ifp, te->te_tsomax);
	if_sethwtsomaxsegcount(ifp, te->te_tsomaxsegcnt);
	if_sethwtsomaxsegsize(ifp, te->te_tsomaxsegsz);
	if_printf(ifp, "tso_wrap detached\n");
	free(te, M_TSOWRAP);
}

static void
tw_departure(void *arg __unused, struct ifnet *ifp)
{
	struct tw_entry *te;

	sx_xlock(&tw_sx);
	te = tw_lookup(ifp);
	if (te != NULL)
		tw_detach(te);
	sx_xunlock(&tw_sx);
}

static int
tw_control_sysctl(SYSCTL_HANDLER_ARGS)
{
	char buf[IFNAMSIZ + 1];
	struct tw_entry *te;
	struct ifnet *ifp;
	const char *name;
	bool detach;
	int error;

	if (req->newptr == NULL) {
		struct sbuf sb;

		sbuf_new_for_sysctl(&sb, NULL, IFNAMSIZ * 8, req);
		sx_slock(&tw_sx);
		CK_LIST_FOREACH(te, &tw_entries, te_list)
			sbuf_printf(&sb, "%s ", if_name(te->te_ifp));
		sx_sunlock(&tw_sx);
		error = sbuf_finish(&sb);
		sbuf_delete(&sb);
		return (error);
	}

	bzero(buf, sizeof(buf));
	error = SYSCTL_IN(req, buf, ulmin(req->newlen, sizeof(buf) - 1));
	if (error != 0)
		return (error);
	name = buf;
	detach = (name[0] == '-');
	if (detach)
		name++;
	if (name[0] == '\0')
		return (EINVAL);

	sx_xlock(&tw_sx);
	if (detach) {
		error = ENOENT;
		CK_LIST_FOREACH(te, &tw_entries, te_list) {
			if (strcmp(if_name(te->te_ifp), name) == 0) {
				tw_detach(te);
				error = 0;
				break;
			}
		}
	} else {
		ifp = ifunit_ref(name);
		if (ifp == NULL) {
			error = ENOENT;
		} else {
			error = tw_attach(ifp);
			if_rele(ifp);
		}
	}
	sx_xunlock(&tw_sx);
	return (error);
}
SYSCTL_PROC(_net_tso_wrap, OID_AUTO, control,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    tw_control_sysctl, "A",
    "Write an interface name to attach, -name to detach; read to list");

static eventhandler_tag tw_departure_tag;

static int
tw_modevent(module_t mod __unused, int type, void *data __unused)
{
	struct tw_entry *te;

	switch (type) {
	case MOD_LOAD:
		tw_departure_tag = EVENTHANDLER_REGISTER(
		    ifnet_departure_event, tw_departure, NULL,
		    EVENTHANDLER_PRI_ANY);
		return (0);
	case MOD_UNLOAD:
		EVENTHANDLER_DEREGISTER(ifnet_departure_event,
		    tw_departure_tag);
		sx_xlock(&tw_sx);
		while ((te = CK_LIST_FIRST(&tw_entries)) != NULL)
			tw_detach(te);
		sx_xunlock(&tw_sx);
		return (0);
	case MOD_QUIESCE:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t tw_mod = {
	"tso_wrap",
	tw_modevent,
	NULL,
};

DECLARE_MODULE(tso_wrap, tw_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(tso_wrap, 1);
