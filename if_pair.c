/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jan Bramkamp <crest+freebsd@rlwinm.de>
 *
 * if_pair(4) - a pair of point-to-point layer-3 interfaces.
 *
 * Like if_epair(4), creating a "pair" yields two interfaces (pairNa and
 * pairNb) whose transmit paths are cross-connected, and either side can be
 * moved into a vnet jail.  Unlike epair there is no Ethernet emulation:
 * the interfaces are IFF_POINTOPOINT, carry bare IPv4/IPv6 packets, and a
 * transmitted mbuf is handed straight to the peer's protocol input via
 * netisr.  No link-layer headers, no ARP/NDP neighbor discovery, no
 * bridge/vlan machinery - just two ends of a wire for routed traffic
 * between vnets (or between the host and a vnet).
 */

#include "opt_inet.h"
#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/epoch.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/netisr.h>
#include <net/vnet.h>

#define	PAIR_NAME	"pair"
#define	PAIR_MTU_DFLT	16384	/* lo(4)'s LOMTU; see commit af78195e0024 */
#define	PAIR_MTU_MIN	72
#define	PAIR_MTU_MAX	65535

/*
 * Checksums the sender's stack may leave for "hardware" to fill in.
 * For traffic terminating in the peer vnet they are never computed at
 * all; for traffic the peer forwards onward they are completed at the
 * true egress (see pair_csum_vouch()).  The set matches epair(4)
 * exactly, and the two omissions are deliberate:
 *
 * SCTP CRC offload: the forwarding paths do complete pending SCTP
 * CRCs (sctp_delayed_cksum() in ip_tryforward(), ip6_tryforward(),
 * ip_output() and pf_route(); commit bcb298fa9e23), but only in
 * kernels built with SCTP support, which an out-of-tree module
 * cannot assume.
 *
 * CSUM_IP (IPv4 header checksum): divert_packet() completes pending
 * L4 checksums before handing a packet to a divert(4) socket (commit
 * f0cada84b1e2) but not a pending header checksum, so an elided
 * ip_sum would reach userland
 * (e.g. natd(8)) as garbage; divert(4) documents the consequence:
 * "Packets written as incoming and having incorrect checksums will be
 * dropped."  The header sum is cheap (20-60 bytes); ip_output()
 * computes it at the true origin instead.
 */
#define	PAIR_CSUM_FEATURES	(CSUM_IP_TCP | CSUM_IP_UDP)
#define	PAIR_CSUM_FEATURES6	(CSUM_IP6_TCP | CSUM_IP6_UDP)

static const char pairname[] = PAIR_NAME;

static MALLOC_DEFINE(M_PAIR, "if_pair", "Point-to-point interface pairs");

enum pair_side {
	PAIR_SIDE_A = 0,
	PAIR_SIDE_B = 1,
};

struct pair_softc {
	if_t			 sc_ifp;
	struct pair_softc	*sc_peer;
	enum pair_side		 sc_side;
	int			 sc_unit;
};

VNET_DEFINE_STATIC(struct if_clone *, pair_cloner);
#define	V_pair_cloner	VNET(pair_cloner)

/*
 * The mbuf crosses the pair with its transmit checksum requests KEPT
 * and csum_data untouched.
 *
 * TCP and UDP need no translation at all: the input paths accept a
 * pending request bit (the "Packet from local host" branches testing
 * CSUM_IP_TCP etc. in tcp_input()/udp_input() and the IPv6 variants;
 * commit bcb298fa9e23: "such packets never have been on the wire")
 * as proof the packet never crossed a physical medium, and the kept
 * request bits make ip_tryforward()/ip_output()/pf_route() complete
 * the checksum if the packet is instead forwarded out a real
 * interface - in NIC hardware or via in_delayed_cksum(), which reads
 * the checksum field offset from csum_data (the offset semantics are
 * documented in mbuf(9)).  Do NOT set CSUM_DATA_VALID |
 * CSUM_PSEUDO_HDR here: with those set the input paths read csum_data
 * as the checksum value computed by hardware and would reject every
 * packet, since ours still holds the offset.  This is exactly
 * epair(4)'s (non-)handling (commit 39d4094173f9), and epair(4)
 * documents the contract: offloaded checksums between interfaces on
 * the same host are unnecessary and ignored, and such packets carry
 * an incorrect checksum on the wire.
 *
 * The IPv4 header checksum is different on both counts: ip_input()
 * has no request-bit shortcut, and CSUM_IP is not in our hwassist
 * (see PAIR_CSUM_FEATURES), so every IP-stack entry point into
 * pair_output() - ip_output() for local traffic, ip_tryforward()/
 * ip_output()/pf_route() for forwarded traffic - has already
 * completed ip_sum in software before handing us the packet.
 * Vouching for it unconditionally saves the peer's input path a
 * software verification.  The one entry path outside that guarantee
 * is bpfwrite(): packets a privileged process injects via bpf(4) are
 * vouched without their header sum ever being computed or checked,
 * matching how lo(4) treats injected packets.
 */
static void
pair_csum_vouch(struct mbuf *m, uint32_t af __unused)
{
#ifdef INET
	if (af == AF_INET)
		m->m_pkthdr.csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;
#endif
}

/*
 * Transmit: validate, tap BPF (DLT_NULL, 4-byte AF pseudo-header, as in
 * lo(4)/gif(4)), then re-inject the packet as peer input in the peer's
 * vnet via netisr.
 *
 * The packet is ALWAYS queued to netisr (netisr_queue()); it is never
 * dispatched inline, no matter what net.isr.dispatch is set to.
 * Inline dispatch would run the peer's entire input path nested
 * inside the sender's call chain, and for TCP between two local
 * sockets that chain loops: the sender's tcp_output() holds its
 * inpcb lock when the peer's inline-processed ACK arrives back and
 * tcp_input() takes the same inpcb lock in the same thread.  On an
 * INVARIANTS kernel that dies as "recursed on non-recursive mutex";
 * on production kernels the ownership KASSERT is compiled out, the
 * mutex silently recurses, and the nested ACK processing mutates the
 * connection (sbdrop(), snd_una) underneath the suspended outer
 * tcp_output(), whose stale send-buffer snapshot later walks off the
 * end of the mbuf chain and panics.  Observed in practice as a page
 * fault in tcp_default_output() under iperf3.  This is why lo(4)
 * always uses netisr_queue() (if_loop.c: "mbuf is free'd on
 * failure") and why epair(4) decouples transmit from receive with
 * its own queues.  Queueing also bounds kernel stack usage for
 * chained pairs and routing loops, so no stack-depth guard is
 * needed.  Concurrency comes from the netisr threads
 * (net.isr.maxthreads, distributed by flowid under
 * NETISR_POLICY_FLOW; netisr(9), commit d4b5cae49bff).
 *
 * Teardown synchronization: every entry path into if_output (the IP
 * stack, netisr and bpfwrite()) runs within the network epoch, and the
 * IFF_DRV_RUNNING check on our own interface below happens before
 * sc_peer is ever dereferenced.  pair_clone_destroy() clears
 * IFF_DRV_RUNNING on both sides and then NET_EPOCH_WAIT()s before
 * detaching or freeing anything, so no thread can be at or past the
 * peer dereference once teardown proceeds.  (Epoch coverage of the
 * netisr workers comes from their swi being INTR_TYPE_NET, commit
 * 511d1afb6bfe.)  Packets sitting in a netisr queue when the pair is
 * destroyed are safe as well: netisr stores rcvif as an
 * index+generation pair (m_rcvif_serialize(), commit 6871de9363e5)
 * and revalidates it at dequeue, dropping packets whose interface is
 * gone rather than dereferencing it.
 */
static int
pair_output(if_t ifp, struct mbuf *m, const struct sockaddr *dst,
    struct route *ro __unused)
{
	struct pair_softc *sc;
	if_t peer_ifp;
	uint32_t af;
	int isr;

	M_ASSERTPKTHDR(m);
	NET_EPOCH_ASSERT();

	if ((if_getflags(ifp) & IFF_UP) == 0 ||
	    (if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0) {
		m_freem(m);
		return (ENETDOWN);
	}

	af = dst->sa_family;
	switch (af) {
#ifdef INET
	case AF_INET:
		isr = NETISR_IP;
		break;
#endif
#ifdef INET6
	case AF_INET6:
		isr = NETISR_IPV6;
		break;
#endif
	default:
		m_freem(m);
		return (EAFNOSUPPORT);
	}

	bpf_mtap2_if(ifp, &af, sizeof(af), m);

	sc = if_getsoftc(ifp);
	peer_ifp = sc->sc_peer->sc_ifp;
	if ((if_getdrvflags(peer_ifp) & IFF_DRV_RUNNING) == 0 ||
	    (if_getflags(peer_ifp) & IFF_UP) == 0) {
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		m_freem(m);
		return (ENETDOWN);
	}

	if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, m->m_pkthdr.len);

	m->m_flags &= ~(M_BCAST | M_MCAST);

	/*
	 * snd_tag shares pkthdr union space with rcvif (introduced by
	 * commit f3e7afe2d7b2); release any send tag (e.g. from pf
	 * route-to diverting a ratelimited flow here) before rcvif is
	 * set, as epair(4) does.
	 */
	if (m->m_pkthdr.csum_flags & CSUM_SND_TAG) {
		m_snd_tag_rele(m->m_pkthdr.snd_tag);
		m->m_pkthdr.snd_tag = NULL;
		m->m_pkthdr.csum_flags &= ~CSUM_SND_TAG;
	}
	m->m_pkthdr.rcvif = peer_ifp;
	pair_csum_vouch(m, af);

	CURVNET_SET_QUIET(if_getvnet(peer_ifp));
	M_SETFIB(m, if_getfib(peer_ifp));
	bpf_mtap2_if(peer_ifp, &af, sizeof(af), m);
	if_inc_counter(peer_ifp, IFCOUNTER_IPACKETS, 1);
	if_inc_counter(peer_ifp, IFCOUNTER_IBYTES, m->m_pkthdr.len);

	/* Never inline; see the function comment.  Frees m on failure. */
	if (netisr_queue(isr, m) != 0)
		if_inc_counter(peer_ifp, IFCOUNTER_IQDROPS, 1);
	CURVNET_RESTORE();

	return (0);
}

static int
pair_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct ifreq *ifr = (struct ifreq *)data;
	int error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
		if_setflagbits(ifp, IFF_UP, 0);
		break;
	case SIOCSIFFLAGS:
		break;
	case SIOCSIFMTU:
		if (ifr->ifr_mtu < PAIR_MTU_MIN || ifr->ifr_mtu > PAIR_MTU_MAX)
			error = EINVAL;
		else
			if_setmtu(ifp, ifr->ifr_mtu);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	case SIOCSIFCAP: {
		/*
		 * Only hwassist (whether the sending stack elides
		 * checksums) tracks the enabled capabilities.  Receive-side
		 * validity is carried per-mbuf by the request bits the
		 * input paths honor, so it stays correct regardless of
		 * which side has offloads toggled.
		 */
		int reqcap = ifr->ifr_reqcap & if_getcapabilities(ifp);
		uint64_t hwassist = 0;

		if (reqcap & IFCAP_TXCSUM)
			hwassist |= PAIR_CSUM_FEATURES;
		if (reqcap & IFCAP_TXCSUM_IPV6)
			hwassist |= PAIR_CSUM_FEATURES6;
		if_setcapenable(ifp, reqcap);
		if_sethwassist(ifp, hwassist);
		break;
	}
	default:
		error = EINVAL;
		break;
	}
	return (error);
}

static struct pair_softc *
pair_alloc_side(int unit, enum pair_side side)
{
	struct pair_softc *sc;
	if_t ifp;
	char name[IFNAMSIZ];

	sc = malloc(sizeof(*sc), M_PAIR, M_WAITOK | M_ZERO);
	sc->sc_side = side;
	sc->sc_unit = unit;

	ifp = if_alloc(IFT_PPP);
	sc->sc_ifp = ifp;
	if_setsoftc(ifp, sc);

	/*
	 * if_dname must remain the bare cloner name ("pair"):
	 * if_clone_destroy() resolves the owning cloner via
	 * ifc_find_cloner_in_vnet(ifp->if_dname, ...), so a full
	 * "pairNa" there makes every destroy fail with EINVAL (found
	 * in VM testing).  Only if_xname carries the full name, as
	 * epair(4) does.
	 */
	if_initname(ifp, pairname, IF_DUNIT_NONE);
	snprintf(name, sizeof(name), "%s%d%c", pairname, unit,
	    side == PAIR_SIDE_A ? 'a' : 'b');
	if_setname(ifp, name);

	if_setflags(ifp, IFF_POINTOPOINT | IFF_MULTICAST);
	if_setmtu(ifp, PAIR_MTU_DFLT);
	if_setbaudrate(ifp, IF_Gbps(10));
	if_setoutputfn(ifp, pair_output);
	if_setioctlfn(ifp, pair_ioctl);
	if_setcapabilities(ifp, IFCAP_HWCSUM | IFCAP_HWCSUM_IPV6);
	if_setcapenable(ifp, IFCAP_HWCSUM | IFCAP_HWCSUM_IPV6);
	if_sethwassist(ifp, PAIR_CSUM_FEATURES | PAIR_CSUM_FEATURES6);

	return (sc);
}

/*
 * Publish one side.  if_attach() (see ifnet(9)) makes the interface
 * reachable by name and index, so the softc - in particular sc_peer,
 * which pair_output() dereferences without a NULL check - must be
 * fully initialized before this is called.  sc_peer is thereby write-once-before-publish and
 * immutable until pair_clone_destroy() has quiesced both sides, which
 * is what makes the lockless read in pair_output() safe.
 */
static void
pair_attach_side(struct pair_softc *sc)
{
	if_t ifp = sc->sc_ifp;

	if_attach(ifp);
	bpfattach(ifp, DLT_NULL, sizeof(uint32_t));
	if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);
}

/*
 * Detach one side.  The caller must already have quiesced the pair
 * (both sides !IFF_DRV_RUNNING, followed by a NET_EPOCH_WAIT()): from
 * that point on no transmit path can reach either softc.
 */
static void
pair_detach_side(struct pair_softc *sc)
{
	if_t ifp = sc->sc_ifp;

	/*
	 * The 'b' side may live in another vnet; detach in its context.
	 */
	CURVNET_SET_QUIET(if_getvnet(ifp));
	bpfdetach(ifp);
	if_detach(ifp);
	CURVNET_RESTORE();
}

static void
pair_free_side(struct pair_softc *sc)
{
	if_free(sc->sc_ifp);
	free(sc, M_PAIR);
}

static int
pair_clone_match(struct if_clone *ifc, const char *name)
{
	const char *cp;

	if (strncmp(pairname, name, sizeof(pairname) - 1) != 0)
		return (0);

	/* Accept "pair" (wildcard) and "pair<unit>". */
	for (cp = name + sizeof(pairname) - 1; *cp != '\0'; cp++) {
		if (*cp < '0' || *cp > '9')
			return (0);
	}
	return (1);
}

static int
pair_clone_create(struct if_clone *ifc, char *name, size_t len,
    struct ifc_data *ifd, if_t *ifpp)
{
	struct pair_softc *sca, *scb;
	int error, unit;

	error = ifc_name2unit(name, &unit);
	if (error != 0)
		return (error);
	/* A name without a unit yields -1: ifc_alloc_unit() picks one. */
	error = ifc_alloc_unit(ifc, &unit);
	if (error != 0)
		return (error);

	sca = pair_alloc_side(unit, PAIR_SIDE_A);
	scb = pair_alloc_side(unit, PAIR_SIDE_B);
	sca->sc_peer = scb;
	scb->sc_peer = sca;

	/* Only publish once both softcs are complete, peer links included. */
	pair_attach_side(sca);
	pair_attach_side(scb);

	/*
	 * The framework links only the returned ifp ('a') into the
	 * cloner list and the "pair" interface group; the 'b' side must
	 * be linked explicitly, as epair(4) does, or destroying by its
	 * name fails with ENXIO (found in VM testing) and pf/ipfw
	 * group rules ("on pair") miss it.
	 */
	if_clone_addif(ifc, scb->sc_ifp);

	/* Report the 'a' side back as the created interface. */
	snprintf(name, len, "%s%da", pairname, unit);
	*ifpp = sca->sc_ifp;

	return (0);
}

static int
pair_clone_destroy(struct if_clone *ifc, if_t ifp, uint32_t flags __unused)
{
	struct pair_softc *sc, *sca, *scb, *other;
	int error, unit;

	/*
	 * Destroying either side destroys both, as with modern
	 * epair(4).  The partner is unlinked from the cloner list via a
	 * nested if_clone_destroyif(), which re-enters here with the
	 * softc already cleared: nothing left to do then.
	 */
	sc = if_getsoftc(ifp);
	if (sc == NULL)
		return (0);

	sca = (sc->sc_side == PAIR_SIDE_A) ? sc : sc->sc_peer;
	scb = sca->sc_peer;
	other = (sc == sca) ? scb : sca;
	unit = sca->sc_unit;

	/*
	 * Quiesce the pair before tearing anything down: with
	 * IFF_DRV_RUNNING cleared on BOTH sides, every new call into
	 * pair_output() bails on its own interface's flag check before
	 * dereferencing sc_peer, and the epoch wait flushes out any
	 * transmit that passed the check earlier.  Only then is it safe
	 * to detach and free the sides one at a time.  epoch(9)
	 * sanctions this wait-then-free pattern and its context rules
	 * (sleepable, no mutexes held - both hold on every path into
	 * the cloner's destroy method).
	 */
	if_setdrvflagbits(sca->sc_ifp, 0, IFF_DRV_RUNNING);
	if_setdrvflagbits(scb->sc_ifp, 0, IFF_DRV_RUNNING);
	NET_EPOCH_WAIT();

	pair_detach_side(scb);
	pair_detach_side(sca);

	/*
	 * Unlink the partner from the cloner list; its nested
	 * destroy_f call sees the cleared softc and does nothing.
	 */
	if_setsoftc(other->sc_ifp, NULL);
	error = if_clone_destroyif(ifc, other->sc_ifp);
	if (error != 0)
		panic("%s: nested if_clone_destroyif() failed: %d",
		    __func__, error);

	pair_free_side(scb);
	pair_free_side(sca);
	ifc_free_unit(ifc, unit);

	return (0);
}

static void
vnet_pair_init(const void *unused __unused)
{
	struct if_clone_addreq req = {
		.match_f = pair_clone_match,
		.create_f = pair_clone_create,
		.destroy_f = pair_clone_destroy,
	};

	V_pair_cloner = ifc_attach_cloner(pairname, &req);
}
VNET_SYSINIT(vnet_pair_init, SI_SUB_PSEUDO, SI_ORDER_ANY,
    vnet_pair_init, NULL);

static void
vnet_pair_uninit(const void *unused __unused)
{
	ifc_detach_cloner(V_pair_cloner);
}
VNET_SYSUNINIT(vnet_pair_uninit, SI_SUB_INIT_IF, SI_ORDER_ANY,
    vnet_pair_uninit, NULL);

static int
pair_modevent(module_t mod, int type, void *data)
{
	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
		/* Per-vnet attach/detach is driven by the VNET_SYSINITs. */
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t pair_mod = {
	"if_pair",
	pair_modevent,
	NULL,
};

DECLARE_MODULE(if_pair, pair_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(if_pair, 1);
