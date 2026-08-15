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
#include <sys/cpuset.h>
#include <sys/epoch.h>
#include <sys/hash.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/smp.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/taskqueue.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/netisr.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

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

/*
 * Receive queue, one per pool worker per side.  Transmitters enqueue
 * onto the receiving side's queue; the pinned worker for pq_id drains
 * it.  Modeled on epair(4)'s struct epair_queue, including the
 * IDLE/WAKING/RUNNING state machine that avoids redundant task
 * enqueues.
 *
 * State machine invariant (lost-wakeup freedom): pq_state != IDLE
 * implies pq_task is pending or running, so every enqueued packet is
 * followed by a worker flush.  The invariant is maintained jointly by
 * producer and worker, and both halves must happen under pq_mtx: the
 * producer transitions IDLE->WAKING and enqueues the task in the same
 * lock hold as its mbufq_enqueue(), and the worker re-checks queue
 * emptiness under the lock before declaring IDLE.  Enqueueing the
 * mbuf before the state check, or re-checking emptiness outside the
 * lock, would silently strand packets on an idle queue.
 */
#define	PAIR_QLIMIT	4096	/* epair's RXRSIZE */

struct pair_softc;

struct pair_queue {
	struct mtx		 pq_mtx;
	struct mbufq		 pq_q;
	int			 pq_id;		/* pool worker index */
	enum {
		PAIR_QUEUE_IDLE,
		PAIR_QUEUE_WAKING,
		PAIR_QUEUE_RUNNING,
	}			 pq_state;
	struct task		 pq_task;
	struct pair_softc	*pq_sc;		/* receiving side */
};

struct pair_softc {
	if_t			 sc_ifp;
	struct pair_softc	*sc_peer;
	enum pair_side		 sc_side;
	int			 sc_unit;
	int			 sc_defqid;	/* steering fallback */
	struct pair_queue	*sc_queues;	/* pair_tasks.pt_count of them */
};

/*
 * The worker pool: one taskqueue with one CPU-pinned thread per CPU,
 * created once at load and shared by all pairs.  epair(4) has this
 * shape only on RSS kernels (one unpinned thread otherwise); we use
 * it unconditionally.
 */
static struct {
	int			 pt_count;	/* >= 1: CPU_FOREACH yields at
						   least the BSP; queue sizing
						   and the steering modulo
						   rely on it */
	struct taskqueue	*pt_tq[MAXCPU];
} pair_tasks;

/* Atomic: creates in different vnets are not mutually serialized. */
static u_int pair_next_defq;

/* Random per-boot seed so flow-to-worker mapping is not guessable. */
static uint32_t pair_hash_seed;

VNET_DEFINE_STATIC(struct if_clone *, pair_cloner);
#define	V_pair_cloner	VNET(pair_cloner)

/*
 * Pool lifecycle runs as plain SYSINIT/SYSUNINIT at SI_SUB_TASKQ
 * rather than from the module event handler: kern_linker.c fires
 * MOD_UNLOAD before it runs the file's SYSUNINITs, and file SYSUNINITs
 * run in reverse subsystem order, so this ordering guarantees the pool
 * exists before the first cloner attach (SI_SUB_PSEUDO) and outlives
 * the last pair destroyed by cloner detach.  (epair frees its pool
 * from MOD_UNLOAD, before its own cloner teardown runs.)
 */
static void
pair_pool_init(const void *unused __unused)
{
	char name[32];
	int cpu, i;

	/*
	 * Unlike epair we do NOT sched_bind() ourselves to each CPU for
	 * NUMA-local allocations: with the module preloaded from
	 * loader.conf these SYSINITs run before SI_SUB_SMP has released
	 * the APs, and binding the boot thread to an offline CPU hangs.
	 * The workers themselves are still pinned via cpuset; they
	 * simply wait until their CPU comes online.
	 */
	pair_hash_seed = arc4random();

	i = 0;
	CPU_FOREACH(cpu) {
		cpuset_t mask;

		snprintf(name, sizeof(name), "pair_task_%d", cpu);
		pair_tasks.pt_tq[i] = taskqueue_create(name, M_WAITOK,
		    taskqueue_thread_enqueue, &pair_tasks.pt_tq[i]);
		CPU_SETOF(cpu, &mask);
		taskqueue_start_threads_cpuset(&pair_tasks.pt_tq[i], 1,
		    PI_NET, &mask, "%s", name);
		i++;
	}
	pair_tasks.pt_count = i;
}
SYSINIT(pair_pool_init, SI_SUB_TASKQ, SI_ORDER_ANY, pair_pool_init, NULL);

static void
pair_pool_uninit(const void *unused __unused)
{
	int i;

	for (i = 0; i < pair_tasks.pt_count; i++) {
		taskqueue_drain_all(pair_tasks.pt_tq[i]);
		taskqueue_free(pair_tasks.pt_tq[i]);
	}
}
SYSUNINIT(pair_pool_uninit, SI_SUB_TASKQ, SI_ORDER_ANY,
    pair_pool_uninit, NULL);

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
 * Deliver one packet into the receiving side's protocol input.  Runs
 * in a pool worker with the network epoch held (NET_TASK_INIT tasks
 * are wrapped in NET_EPOCH by the taskqueue) and the side's vnet set
 * by the caller.  There is no link-layer header, so the address
 * family is recovered from the IP version nibble.
 */
static void
pair_input(if_t ifp, struct mbuf *m)
{
	uint32_t af;
	int isr, len;

	M_ASSERTPKTHDR(m);

	/*
	 * The version-nibble read below is a one-byte mtod(): fine for
	 * contiguity once m_len >= 1 (bytes 0..m_len-1 of an mbuf are
	 * contiguous by definition), but only for MAPPED mbufs - for
	 * M_EXTPG mbufs m_data is not a valid pointer.  Every known
	 * producer builds the IP header in a mapped leading mbuf and
	 * chains unmapped pages as payload only (sendfile, kTLS), so
	 * the M_EXTPG half of the guard enforces an invariant rather
	 * than handling an expected case.  m_pullup() would be no
	 * remedy for an unmapped head: it KASSERTs on M_EXTPG.
	 */
	if (__predict_false((m->m_flags & M_EXTPG) != 0 || m->m_len < 1))
		goto bad;
	switch (*mtod(m, const uint8_t *) >> 4) {
#ifdef INET
	case 4:
		af = AF_INET;
		isr = NETISR_IP;
		break;
#endif
#ifdef INET6
	case 6:
		af = AF_INET6;
		isr = NETISR_IPV6;
		break;
#endif
	default:
	bad:
		/*
		 * Counted as input errors on the receiving side: the
		 * Ierrs column of "netstat -id" (or "netstat -I pairNa -d"
		 * for one interface).
		 */
		if_inc_counter(ifp, IFCOUNTER_IERRORS, 1);
		m_freem(m);
		return;
	}

	len = m->m_pkthdr.len;
	bpf_mtap2_if(ifp, &af, sizeof(af), m);
	if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_IBYTES, len);
	netisr_dispatch(isr, m);
}

/*
 * Pool worker: flush this queue once and deliver each packet.  The
 * single flush per task run (rescheduling if more arrived meanwhile)
 * is epair's guard against starving other pairs sharing the worker.
 * if_ref() pins the ifnet across the run, matching epair.
 */
static void
pair_task_deferred(void *arg, int pending __unused)
{
	struct pair_queue *q = arg;
	if_t ifp = q->pq_sc->sc_ifp;
	struct mbuf *m, *n;
	bool resched;

	if_ref(ifp);
	CURVNET_SET(if_getvnet(ifp));

	mtx_lock(&q->pq_mtx);
	m = mbufq_flush(&q->pq_q);
	q->pq_state = PAIR_QUEUE_RUNNING;
	mtx_unlock(&q->pq_mtx);

	while (m != NULL) {
		n = STAILQ_NEXT(m, m_stailqpkt);
		m->m_nextpkt = NULL;
		pair_input(ifp, m);
		m = n;
	}

	/* Emptiness re-check under the lock; see struct pair_queue. */
	mtx_lock(&q->pq_mtx);
	if (!mbufq_empty(&q->pq_q)) {
		resched = true;
		q->pq_state = PAIR_QUEUE_WAKING;
	} else {
		resched = false;
		q->pq_state = PAIR_QUEUE_IDLE;
	}
	mtx_unlock(&q->pq_mtx);
	if (resched)
		taskqueue_enqueue(pair_tasks.pt_tq[q->pq_id], &q->pq_task);

	CURVNET_RESTORE();
	if_rele(ifp);
}

/*
 * Software flow hash for packets that carry no flowid — which on
 * non-RSS kernels is every packet of a purely pair-local connection:
 * no NIC ever stamps one, so inp_flowid never gets learned (observed
 * live: 8 iperf3 streams all riding the static fallback, serialized
 * on one worker).  Hashes source/destination address, IP protocol
 * and, when safely readable, the TCP/UDP port pair, via
 * jenkins_hash32() with a random per-boot seed; same tuple -> same
 * hash preserves per-flow ordering.  Fragments hash without ports so
 * all fragments of a datagram land on one queue (the first fragment
 * would otherwise part ways with the rest), as hardware RSS does.
 * IPv6 extension-header chains are not walked: anything but plain
 * TCP/UDP after the fixed header hashes on addresses and next-header
 * alone.  Headers are read with m_copydata(), which handles split
 * and unmapped (M_EXTPG) chains, so this adds no contiguity or
 * mappedness assumptions.  Returns false for unhashable packets
 * (too short, unknown family); the caller then uses the side's
 * static default queue.
 */
static bool
pair_hash_mbuf(struct mbuf *m, uint32_t af, uint32_t *hashp)
{
	uint32_t key[10];
	uint16_t ports[2];
	int nwords, poff;
	uint8_t proto;
	bool have_ports;

	switch (af) {
#ifdef INET
	case AF_INET: {
		struct ip ip;

		if (m->m_pkthdr.len < (int)sizeof(ip))
			return (false);
		m_copydata(m, 0, sizeof(ip), (caddr_t)&ip);
		poff = ip.ip_hl << 2;
		if (poff < (int)sizeof(ip))
			return (false);
		key[0] = ip.ip_src.s_addr;
		key[1] = ip.ip_dst.s_addr;
		proto = ip.ip_p;
		key[2] = proto;
		nwords = 3;
		have_ports = (ip.ip_off & htons(IP_MF | IP_OFFMASK)) == 0;
		break;
	}
#endif
#ifdef INET6
	case AF_INET6: {
		struct ip6_hdr ip6;

		if (m->m_pkthdr.len < (int)sizeof(ip6))
			return (false);
		m_copydata(m, 0, sizeof(ip6), (caddr_t)&ip6);
		memcpy(&key[0], &ip6.ip6_src, sizeof(ip6.ip6_src));
		memcpy(&key[4], &ip6.ip6_dst, sizeof(ip6.ip6_dst));
		proto = ip6.ip6_nxt;
		key[8] = proto;
		nwords = 9;
		poff = sizeof(ip6);
		have_ports = true;
		break;
	}
#endif
	default:
		return (false);
	}

	if (have_ports && (proto == IPPROTO_TCP || proto == IPPROTO_UDP) &&
	    m->m_pkthdr.len >= poff + (int)sizeof(ports)) {
		m_copydata(m, poff, sizeof(ports), (caddr_t)ports);
		key[nwords++] = (uint32_t)ports[0] << 16 | ports[1];
	}

	*hashp = jenkins_hash32(key, nwords, pair_hash_seed);
	return (true);
}

/*
 * Pick the receiving side's queue.  Steer by the mbuf's flow id when
 * it carries one (commit d4b5cae49bff documents the ordering policy
 * this feeds); otherwise compute one (see pair_hash_mbuf()) and write
 * it back as M_HASHTYPE_OPAQUE_HASH ("has hash properties", mbuf(9))
 * so the peer's stack and any further hop inherit it.  Only truly
 * unhashable packets take the side's static default queue.  Same
 * flow -> same worker always, preserving per-flow ordering.
 */
static struct pair_queue *
pair_select_queue(struct pair_softc *sc, struct mbuf *m, uint32_t af)
{
	uint32_t hash, qid;

	if (M_HASHTYPE_GET(m) != M_HASHTYPE_NONE) {
		qid = m->m_pkthdr.flowid % pair_tasks.pt_count;
	} else if (pair_hash_mbuf(m, af, &hash)) {
		m->m_pkthdr.flowid = hash;
		M_HASHTYPE_SET(m, M_HASHTYPE_OPAQUE_HASH);
		qid = hash % pair_tasks.pt_count;
	} else {
		qid = sc->sc_defqid;
	}
	return (&sc->sc_queues[qid]);
}

/*
 * Transmit: validate, tap BPF (DLT_NULL, 4-byte AF pseudo-header, as in
 * lo(4)/gif(4)), then enqueue onto the peer side's receive queue and
 * wake its pinned pool worker.
 *
 * The packet is ALWAYS handed to a pool worker; it is never delivered
 * inline.
 * Inline delivery would run the peer's entire input path nested
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
 * its own queues.  Deferral also bounds kernel stack usage for
 * chained pairs and routing loops, so no stack-depth guard is
 * needed.  Concurrency comes from the pinned per-CPU pool workers,
 * with flows spread across them by pair_select_queue().
 *
 * Teardown synchronization: every entry path into if_output (the IP
 * stack, netisr and bpfwrite()) runs within the network epoch, and the
 * IFF_DRV_RUNNING check on our own interface below happens before
 * sc_peer is ever dereferenced.  pair_clone_destroy() clears
 * IFF_DRV_RUNNING on both sides and then NET_EPOCH_WAIT()s, so no
 * producer can be at or past the peer dereference once teardown
 * proceeds; it then taskqueue_drain()s and flushes every queue before
 * anything is detached or freed, so the raw rcvif pointers queued
 * mbufs carry never outlive their interface.  Packets the workers
 * push onward into netisr (deferred dispatch policy) are covered by
 * netisr's own rcvif serialization (m_rcvif_serialize(), commit
 * 6871de9363e5).  Epoch coverage of the netisr workers themselves is
 * INTR_TYPE_NET (commit 511d1afb6bfe); our pool workers get theirs
 * from NET_TASK_INIT.
 */
static int
pair_output(if_t ifp, struct mbuf *m, const struct sockaddr *dst,
    struct route *ro __unused)
{
	struct pair_softc *sc;
	struct pair_queue *q;
	if_t peer_ifp;
	uint32_t af;
	int error, len;

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
#endif
#ifdef INET6
	case AF_INET6:
#endif
		break;
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

	m->m_flags &= ~(M_BCAST | M_MCAST);

	/*
	 * snd_tag shares pkthdr union space with rcvif (introduced by
	 * commit f3e7afe2d7b2); release any send tag (e.g. from pf
	 * route-to diverting a ratelimited flow here) before rcvif is
	 * set, as epair(4) does.  Non-persistent mbuf tags may
	 * reference state in the sending vnet and are dropped, also as
	 * epair(4) does.
	 */
	if (m->m_pkthdr.csum_flags & CSUM_SND_TAG) {
		m_snd_tag_rele(m->m_pkthdr.snd_tag);
		m->m_pkthdr.snd_tag = NULL;
		m->m_pkthdr.csum_flags &= ~CSUM_SND_TAG;
	}
	m_tag_delete_nonpersistent(m);
	m->m_pkthdr.rcvif = peer_ifp;
	M_SETFIB(m, if_getfib(peer_ifp));
	pair_csum_vouch(m, af);

	/* Save before the queue owns the mbuf. */
	len = m->m_pkthdr.len;

	q = pair_select_queue(sc->sc_peer, m, af);
	mtx_lock(&q->pq_mtx);
	/* Wake and enqueue in one lock hold; see struct pair_queue. */
	if (q->pq_state == PAIR_QUEUE_IDLE) {
		q->pq_state = PAIR_QUEUE_WAKING;
		taskqueue_enqueue(pair_tasks.pt_tq[q->pq_id], &q->pq_task);
	}
	error = mbufq_enqueue(&q->pq_q, m);
	mtx_unlock(&q->pq_mtx);

	if (error != 0) {
		m_freem(m);
		if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
		return (ENOBUFS);
	}
	if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, len);

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

		/* Link state reporting cannot be turned off. */
		reqcap |= IFCAP_LINKSTATE;
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

	sc->sc_queues = malloc(sizeof(*sc->sc_queues) * pair_tasks.pt_count,
	    M_PAIR, M_WAITOK | M_ZERO);
	for (int i = 0; i < pair_tasks.pt_count; i++) {
		struct pair_queue *q = &sc->sc_queues[i];

		q->pq_id = i;
		q->pq_state = PAIR_QUEUE_IDLE;
		mtx_init(&q->pq_mtx, "pairq", NULL, MTX_DEF | MTX_NEW);
		mbufq_init(&q->pq_q, PAIR_QLIMIT);
		q->pq_sc = sc;
		NET_TASK_INIT(&q->pq_task, 0, pair_task_deferred, q);
	}
	sc->sc_defqid = atomic_fetchadd_int(&pair_next_defq, 1) %
	    pair_tasks.pt_count;

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
	if_setcapabilities(ifp,
	    IFCAP_HWCSUM | IFCAP_HWCSUM_IPV6 | IFCAP_LINKSTATE);
	if_setcapenable(ifp,
	    IFCAP_HWCSUM | IFCAP_HWCSUM_IPV6 | IFCAP_LINKSTATE);
	if_sethwassist(ifp, PAIR_CSUM_FEATURES | PAIR_CSUM_FEATURES6);

	return (sc);
}

/*
 * Both sides report a synthetic carrier: LINK_STATE_UP while attached
 * and running, LINK_STATE_DOWN as the first step of teardown (link
 * down before IFF_DRV_RUNNING clears; the reverse order on the way
 * up), following epair(4)'s epair_set_state().  A peer-reflecting
 * carrier (mirror the other side's administrative state, the truer
 * point-to-point semantic) was considered and deferred: SIOCSIFFLAGS
 * runs outside the network epoch, so dereferencing sc_peer there
 * would reopen the teardown race that pair_output()'s epoch
 * discipline closes.
 */
static void
pair_set_state(if_t ifp, bool running)
{
	if (running) {
		if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);
		if_link_state_change(ifp, LINK_STATE_UP);
	} else {
		if_link_state_change(ifp, LINK_STATE_DOWN);
		if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
	}
}

/*
 * Publish one side.  if_attach() (see ifnet(9)) makes the interface
 * reachable by name and index, so the softc - in particular sc_peer,
 * which pair_output() dereferences without a NULL check - must be
 * fully initialized before this is called.  sc_peer is thereby
 * write-once-before-publish and immutable until pair_clone_destroy()
 * has quiesced both sides, which is what makes the lockless read in
 * pair_output() safe.
 */
static void
pair_attach_side(struct pair_softc *sc)
{
	if_t ifp = sc->sc_ifp;

	if_attach(ifp);
	bpfattach(ifp, DLT_NULL, sizeof(uint32_t));
	pair_set_state(ifp, true);
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
	for (int i = 0; i < pair_tasks.pt_count; i++) {
		struct pair_queue *q = &sc->sc_queues[i];

		MPASS(mbufq_empty(&q->pq_q));
		mtx_destroy(&q->pq_mtx);
	}
	free(sc->sc_queues, M_PAIR);
	free(sc, M_PAIR);
}

/*
 * Drain one side's receive queues.  Producers must already be
 * quiesced (both sides !IFF_DRV_RUNNING + NET_EPOCH_WAIT()), so a
 * worker that reschedules itself settles once its queue is empty and
 * taskqueue_drain() then returns with nothing pending; anything still
 * queued afterward is freed here.
 */
static void
pair_drain_queues(struct pair_softc *sc)
{
	struct mbuf *m, *n;

	for (int i = 0; i < pair_tasks.pt_count; i++) {
		struct pair_queue *q = &sc->sc_queues[i];

		taskqueue_drain(pair_tasks.pt_tq[q->pq_id], &q->pq_task);

		mtx_lock(&q->pq_mtx);
		m = mbufq_flush(&q->pq_q);
		q->pq_state = PAIR_QUEUE_IDLE;
		mtx_unlock(&q->pq_mtx);

		while (m != NULL) {
			n = STAILQ_NEXT(m, m_stailqpkt);
			m->m_nextpkt = NULL;
			m_freem(m);
			m = n;
		}
	}
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
	 *
	 * Serialization invariant: concurrent destroys of the two sides
	 * are assumed serialized by our callers.  SIOCIFDESTROY and
	 * vnet teardown both hold ifnet_detach_sxlock exclusively; the
	 * module-unload path (if_clone_detach()) has not been verified
	 * to take it, so a kldunload racing an ifconfig destroy is a
	 * theoretical double teardown - an exposure shared with
	 * epair(4), whose destroy makes the same assumption.  The
	 * panic() below on nested-destroy failure is "cannot happen"
	 * only under this assumption.
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
	pair_set_state(sca->sc_ifp, false);
	pair_set_state(scb->sc_ifp, false);
	NET_EPOCH_WAIT();

	pair_drain_queues(scb);
	pair_drain_queues(sca);

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
