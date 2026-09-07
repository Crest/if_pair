/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jan Bramkamp <crest+freebsd@rlwinm.de>
 *
 * tcp_tso_chop() - software TCP segmentation of a CSUM_TSO frame.
 * See tcp_tso.c for the contract; CHOPPER.txt for the plan.
 */
#ifndef _NETINET_TCP_TSO_H_
#define	_NETINET_TCP_TSO_H_

struct mbuf;

/*
 * The egress computes TCP checksums in hardware: emit segments
 * carrying the checksum request bits and a per-segment
 * pseudo-header sum in th_sum.  Without this flag full checksums
 * are computed in software (the in_delayed_cksum pattern).
 */
#define	TSO_CHOP_HWCSUM	0x0001

/*
 * maxlen: when nonzero, an upper bound on each emitted segment's
 * length measured from the start of the IP header (i.e. the
 * egress L3 MTU); the payload size is clamped to fit.  Zero
 * means "trust tso_segsz" (origin frames sized by the stack).
 */
int	tcp_tso_chop(struct mbuf **m0, unsigned int l2hlen,
	    unsigned int maxlen, int flags, struct mbuf **chainp);

#endif /* !_NETINET_TCP_TSO_H_ */
