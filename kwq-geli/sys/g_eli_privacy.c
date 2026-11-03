/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2005-2011 Pawel Jakub Dawidek <pawel@dawidek.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/bio.h>
#include <sys/fail.h>
#include <sys/sysctl.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/vnode.h>

#include <vm/uma.h>

#include <geom/geom.h>
#include <geom/geom_dbg.h>
#include "g_eli.h"
#include "pkcs5v2.h"

/*
 * Code paths:
 * BIO_READ:
 *	g_eli_start -> g_eli_crypto_read -> g_io_request -> g_eli_read_done -> g_eli_crypto_run -> g_eli_crypto_read_done -> g_io_deliver
 * BIO_WRITE:
 *	g_eli_start -> g_eli_crypto_run -> g_eli_crypto_write_done -> g_io_request -> g_eli_write_done -> g_io_deliver
 */

/*
 * Copy data from a (potentially unmapped) bio to a kernelspace buffer.
 *
 * The buffer must have at least as much room as bp->bio_length.
 */
static void
g_eli_bio_copyin(struct bio *bp, void *kaddr)
{
	struct uio uio;
	struct iovec iov[1];

	iov[0].iov_base = kaddr;
	iov[0].iov_len = bp->bio_length;
	uio.uio_iov = iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = 0;
	uio.uio_resid = bp->bio_length;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_READ;
	uiomove_fromphys(bp->bio_ma, bp->bio_ma_offset, bp->bio_length, &uio);
}

/*
 * The function is called after we read and decrypt data.
 *
 * g_eli_start -> g_eli_crypto_read -> g_io_request -> g_eli_read_done -> g_eli_crypto_run -> G_ELI_CRYPTO_READ_DONE -> g_io_deliver
 */
static int
g_eli_crypto_read_done(struct cryptop *crp)
{
	struct g_eli_softc *sc;
	struct bio *bp;

	if (crp->crp_etype == EAGAIN) {
		if (g_eli_crypto_rerun(crp) == 0)
			return (0);
	}
	bp = (struct bio *)crp->crp_opaque;
	bp->bio_inbed++;
	if (crp->crp_etype == 0) {
		G_ELI_DEBUG(3, "Crypto READ request done (%d/%d).",
		    bp->bio_inbed, bp->bio_children);
		bp->bio_completed += crp->crp_payload_length;
	} else {
		G_ELI_DEBUG(1, "Crypto READ request failed (%d/%d) error=%d.",
		    bp->bio_inbed, bp->bio_children, crp->crp_etype);
		if (bp->bio_error == 0)
			bp->bio_error = crp->crp_etype;
	}
	sc = bp->bio_to->geom->softc;
	if (sc != NULL && crp->crp_cipher_key != NULL)
		g_eli_key_drop(sc, __DECONST(void *, crp->crp_cipher_key));
	crypto_freereq(crp);
	/*
	 * Do we have all sectors already?
	 */
	if (bp->bio_inbed < bp->bio_children)
		return (0);

	if (bp->bio_error != 0) {
		G_ELI_LOGREQ(0, bp, "Crypto READ request failed (error=%d).",
		    bp->bio_error);
		bp->bio_completed = 0;
	}
	/*
	 * Read is finished, send it up.
	 */
	g_io_deliver(bp, bp->bio_error);
	if (sc != NULL)
		atomic_subtract_int(&sc->sc_inflight, 1);
	return (0);
}

/*
 * The function is called after data encryption.
 *
 * g_eli_start -> g_eli_crypto_run -> G_ELI_CRYPTO_WRITE_DONE -> g_io_request -> g_eli_write_done -> g_io_deliver
 */
static int
g_eli_crypto_write_done(struct cryptop *crp)
{
	struct g_eli_softc *sc;
	struct g_geom *gp;
	struct g_consumer *cp;
	struct bio *bp, *cbp;

	if (crp->crp_etype == EAGAIN) {
		if (g_eli_crypto_rerun(crp) == 0)
			return (0);
	}
	bp = (struct bio *)crp->crp_opaque;
	bp->bio_inbed++;
	if (crp->crp_etype == 0) {
		G_ELI_DEBUG(3, "Crypto WRITE request done (%d/%d).",
		    bp->bio_inbed, bp->bio_children);
	} else {
		G_ELI_DEBUG(1, "Crypto WRITE request failed (%d/%d) error=%d.",
		    bp->bio_inbed, bp->bio_children, crp->crp_etype);
		if (bp->bio_error == 0)
			bp->bio_error = crp->crp_etype;
	}
	gp = bp->bio_to->geom;
	sc = gp->softc;
	if (crp->crp_cipher_key != NULL)
		g_eli_key_drop(sc, __DECONST(void *, crp->crp_cipher_key));
	crypto_freereq(crp);
	/*
	 * All sectors are already encrypted?
	 */
	if (bp->bio_inbed < bp->bio_children)
		return (0);
	bp->bio_inbed = 0;
	bp->bio_children = 1;
	cbp = bp->bio_driver1;
	bp->bio_driver1 = NULL;
	if (bp->bio_error != 0) {
		G_ELI_LOGREQ(0, bp, "Crypto WRITE request failed (error=%d).",
		    bp->bio_error);
		g_eli_free_data(bp);
		g_destroy_bio(cbp);
		g_io_deliver(bp, bp->bio_error);
		atomic_subtract_int(&sc->sc_inflight, 1);
		return (0);
	}
	cbp->bio_data = bp->bio_driver2;
	/* 
	 * Clear BIO_UNMAPPED, which was inherited from where we cloned the bio
	 * in g_eli_start, because we manually set bio_data
	 */
	cbp->bio_flags &= ~BIO_UNMAPPED;
	cbp->bio_done = g_eli_write_done;
	cp = LIST_FIRST(&gp->consumer);
	cbp->bio_to = cp->provider;
	G_ELI_LOGREQ(2, cbp, "Sending request.");
	/*
	 * Send encrypted data to the provider.
	 */
	g_io_request(cbp, cp);
	return (0);
}

/*
 * The function is called to read encrypted data.
 *
 * g_eli_start -> G_ELI_CRYPTO_READ -> g_io_request -> g_eli_read_done -> g_eli_crypto_run -> g_eli_crypto_read_done -> g_io_deliver
 */
void
g_eli_crypto_read(struct g_eli_softc *sc, struct bio *bp, boolean_t fromworker)
{
	struct g_consumer *cp;
	struct bio *cbp;

	if (!fromworker) {
		/*
		 * We are not called from the worker thread, so check if
		 * device is suspended.
		 */
		mtx_lock(&sc->sc_queue_mtx);
		if (sc->sc_flags & G_ELI_FLAG_SUSPEND) {
			/*
			 * If device is suspended, we place the request onto
			 * the queue, so it can be handled after resume.
			 */
			G_ELI_DEBUG(0, "device suspended, move onto queue");
			bioq_insert_tail(&sc->sc_queue, bp);
			mtx_unlock(&sc->sc_queue_mtx);
			wakeup(sc);
			return;
		}
		atomic_add_int(&sc->sc_inflight, 1);
		mtx_unlock(&sc->sc_queue_mtx);
	}
	G_ELI_SETWORKER(bp->bio_pflags, 0);
	bp->bio_driver2 = NULL;
	cbp = bp->bio_driver1;
	cbp->bio_done = g_eli_read_done;
	cp = LIST_FIRST(&sc->sc_geom->consumer);
	cbp->bio_to = cp->provider;
	G_ELI_LOGREQ(2, cbp, "Sending request.");
	/*
	 * Read encrypted data from provider.
	 */
	g_io_request(cbp, cp);
}

/*
 * This is the main function responsible for cryptography (ie. communication
 * with crypto(9) subsystem).
 *
 * BIO_READ:
 *	g_eli_start -> g_eli_crypto_read -> g_io_request -> g_eli_read_done -> G_ELI_CRYPTO_RUN -> g_eli_crypto_read_done -> g_io_deliver
 * BIO_WRITE:
 *	g_eli_start -> G_ELI_CRYPTO_RUN -> g_eli_crypto_write_done -> g_io_request -> g_eli_write_done -> g_io_deliver
 */
/*
 * kwq-geli: returns true when the bio is not yet fully dispatched and
 * must be requeued by the caller, which then runs it again from where it
 * stopped.  That happens, for synchronous sessions (software and
 * accelerated software drivers, where every request completes inside its
 * dispatch), when the pass's budget is spent, checked every
 * G_ELI_BUDGET_SECTORS sectors so that a long bio does not hold the
 * worker past its quantum or the share limit.  The sector it goes on from
 * is bio_inbed, the completions so far, exact because they are
 * synchronous.  A crypto request or key that cannot be had without
 * sleeping fails the bio with ENOMEM wherever it happens, and GEOM issues
 * it again after a pause, as for GELI's buffer shortage (g_io_deliver());
 * the sectors done so far are done again.  (Requeueing it instead, as the
 * first version did after the first sector, retried it at once and at
 * PI_SOFT for as long as the shortage lasted; found in review.)  Requests are taken one per sector as
 * GELI takes them, M_NOWAIT where GELI said M_WAITOK, and each completes
 * inside its dispatch, so the next allocation reuses it cache-hot.  (The
 * first port took all of a bio's requests before the first dispatch: 256
 * zeroed requests per 1 MiB bio emptied UMA's per-CPU cache, and lab's
 * reads ran 18 % below stock GELI with uma_zalloc and memset three and one
 * and a half times as busy in a profile.  The second kept the resume
 * point in the upper half of bio_pflags, which is 16 bits wide: resumed
 * bios restarted at sector 0, the guest panicked on a read's completed
 * count and lab hung on bios that never finished.)  An asynchronous
 * session takes all requests and keys first (g_eli_getreqs()) and never
 * stops part way, its completions running behind its dispatches.
 */
static void
g_eli_crypto_nomem(struct g_eli_softc *sc, struct bio *bp)
{

	G_ELI_LOGREQ(1, bp, "Crypto request failed (ENOMEM).");
	/* A read's clone is gone already (g_eli_read_done()). */
	if (bp->bio_cmd == BIO_WRITE && bp->bio_driver1 != NULL) {
		g_destroy_bio(bp->bio_driver1);
		bp->bio_driver1 = NULL;
	}
	g_eli_free_data(bp);
	bp->bio_completed = 0;
	g_io_deliver(bp, ENOMEM);
	atomic_subtract_int(&sc->sc_inflight, 1);
}

bool
g_eli_crypto_run(struct g_eli_worker *wr, struct bio *bp)
{
	struct g_eli_softc *sc;
	struct cryptopq crpq;
	struct cryptop *crp;
	vm_page_t *pages;
	struct cryptopq pool;
	u_int i, first, nsec, secsize;
	off_t dstoff;
	u_char *data = NULL;
	int error __diagused, pages_offset;
	bool batch, sync, partial = false;

	G_ELI_LOGREQ(3, bp, "%s", __func__);

	G_ELI_SETWORKER(bp->bio_pflags, wr->w_number);
	sc = wr->w_softc;
	secsize = LIST_FIRST(&sc->sc_geom->provider)->sectorsize;
	nsec = bp->bio_length / secsize;
	sync = (crypto_ses2caps(wr->w_sid) & CRYPTOCAP_F_SYNC) != 0;
	first = (bp->bio_pflags & G_ELI_STARTED) != 0 ? bp->bio_inbed : 0;
	KASSERT(first < nsec, ("%s: bio %p resumed at %u of %u", __func__, bp,
	    first, nsec));

	if (first == 0) {
		bp->bio_pflags |= G_ELI_STARTED;
		bp->bio_inbed = 0;
		bp->bio_children = nsec;

		/*
		 * If we write the data we cannot destroy current bio_data content,
		 * so we need to allocate more memory for encrypted data.
		 */
		if (bp->bio_cmd == BIO_WRITE) {
			if (!g_eli_alloc_data(bp, bp->bio_length)) {
				G_ELI_LOGREQ(0, bp, "Crypto request failed (ENOMEM).");
				if (bp->bio_driver1 != NULL) {
					g_destroy_bio(bp->bio_driver1);
					bp->bio_driver1 = NULL;
				}
				bp->bio_error = ENOMEM;
				g_io_deliver(bp, bp->bio_error);
				if (sc != NULL)
					atomic_subtract_int(&sc->sc_inflight, 1);
				return (false);
			}
			/* 
			 * This copy could be eliminated by using crypto's output
			 * buffer, instead of using a single overwriting buffer.
			 */
			if ((bp->bio_flags & BIO_UNMAPPED) != 0)
				g_eli_bio_copyin(bp, bp->bio_driver2);
			else
				bcopy(bp->bio_data, bp->bio_driver2, bp->bio_length);
		}
	}
	if (bp->bio_cmd == BIO_WRITE) {
		data = (u_char *)bp->bio_driver2 + (size_t)first * secsize;
	} else {
		if ((bp->bio_flags & BIO_UNMAPPED) != 0) {
			pages_offset = bp->bio_ma_offset + first * secsize;
			pages = bp->bio_ma + (pages_offset >> PAGE_SHIFT);
			pages_offset &= PAGE_MASK;
		} else {
			data = (u_char *)bp->bio_data + (size_t)first * secsize;
		}
	}

	TAILQ_INIT(&crpq);
	TAILQ_INIT(&pool);
	batch = atomic_load_int(&g_eli_batch) != 0;
	if (!sync && g_eli_getreqs(sc, wr->w_sid, nsec, bp->bio_offset, secsize,
	    (sc->sc_flags & G_ELI_FLAG_SINGLE_KEY) == 0, &pool) != 0) {
		g_eli_crypto_nomem(sc, bp);
		return (false);
	}

	for (i = first, dstoff = bp->bio_offset + (off_t)first * secsize;
	    i < nsec; i++, dstoff += secsize) {
		if (!sync) {
			/* all taken, keys held (g_eli_getreqs()) */
			crp = TAILQ_FIRST(&pool);
			TAILQ_REMOVE(&pool, crp, crp_next);
			goto fill;
		}
		if (i > first && i % G_ELI_BUDGET_SECTORS == 0 &&
		    kwq_budget_left(sc->sc_kwq) == 0) {
			partial = true;
			break;
		}
		crp = crypto_getreq(wr->w_sid, M_NOWAIT);
		/* kwq-geli: the shortage on demand, for the nomem case */
		KFAIL_POINT_CODE_FLAGS(DEBUG_FP, kwqeli_getreq,
		    FAIL_POINT_NONSLEEPABLE, if (crp != NULL) {
			crypto_freereq(crp);
			crp = NULL;
		});
		if (crp != NULL && (sc->sc_flags & G_ELI_FLAG_SINGLE_KEY) == 0) {
			crp->crp_cipher_key = g_eli_key_hold(sc, dstoff,
			    secsize, M_NOWAIT);
			if (crp->crp_cipher_key == NULL) {
				crypto_freereq(crp);
				crp = NULL;
			}
		}
		if (crp == NULL) {
			/*
			 * Sync: the sectors before have completed, those of a
			 * batch once it is dispatched, none of them the last,
			 * so nothing is in flight once it returns.
			 */
			if (batch)
				crypto_dispatch_batch(&crpq, 0);
			g_eli_crypto_nomem(sc, bp);
			return (false);
		}
fill:
		if (data) {
			crypto_use_buf(crp, data, secsize);
			data += secsize;
		} else {
			MPASS(pages != NULL);
			crypto_use_vmpage(crp, pages, secsize, pages_offset);
			pages_offset += secsize;
			pages += pages_offset >> PAGE_SHIFT;
			pages_offset &= PAGE_MASK;
		}
		crp->crp_opaque = (void *)bp;
		if (bp->bio_cmd == BIO_WRITE) {
			crp->crp_op = CRYPTO_OP_ENCRYPT;
			crp->crp_callback = g_eli_crypto_write_done;
		} else /* if (bp->bio_cmd == BIO_READ) */ {
			crp->crp_op = CRYPTO_OP_DECRYPT;
			crp->crp_callback = g_eli_crypto_read_done;
		}
		crp->crp_flags = CRYPTO_F_CBIFSYNC;
		crp->crp_payload_start = 0;
		crp->crp_payload_length = secsize;
		if (g_eli_ivlen(sc->sc_ealgo) != 0) {
			crp->crp_flags |= CRYPTO_F_IV_SEPARATE;
			g_eli_crypto_ivgen(sc, dstoff, crp->crp_iv,
			    sizeof(crp->crp_iv));
		}

		/*
		 * The last sector's completion delivers the bio, which is not
		 * ours to touch after this dispatch; the loop ends with it.
		 */
		if (batch) {
			TAILQ_INSERT_TAIL(&crpq, crp, crp_next);
		} else {
			error = crypto_dispatch(crp);
			KASSERT(error == 0,
			    ("crypto_dispatch() failed (error=%d)", error));
			/*
			 * The resume point and the ENOMEM path rely on this:
			 * no synchronous driver in the tree returns ERESTART,
			 * the one way crypto(9) defers a request.
			 */
			KASSERT(!sync || i + 1 == nsec || bp->bio_inbed == i + 1,
			    ("%s: bio %p sector %u of %u not completed inline",
			    __func__, bp, i, nsec));
		}
	}

	if (batch)
		crypto_dispatch_batch(&crpq, 0);
	return (partial);
}
