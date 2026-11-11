/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * geom_simdisk: dummy drives for benchmarking what sits above them.  A
 * provider keeps no data: writes are dropped and reads complete without
 * touching the buffer, so it costs no memory and no copy, whatever its size,
 * and whatever runs on it does its real work on dummy data.  What it does
 * model is time.  Requests are served in arrival order at the device's rate
 * for their direction, a request that does not continue the last one pays
 * the seek, and each completes the latency after its service ends.  The
 * completion comes from a callout, the nearest thing to a drive's interrupt.
 * A rate of 0 is no limit, and with a latency and a seek of 0 too a request
 * completes inside its start.
 *
 * md(4) of type null is no substitute: it takes no unmapped bios, so every
 * read pays a transient mapping, and its worker thread runs below BULK.
 *
 *   sysctl kern.geom.simdisk.add="NAME MIB WRITE_MBPS READ_MBPS LAT_US SEEK_US"
 *   sysctl kern.geom.simdisk.del=NAME
 *
 * For benchmarks only: nothing written can be read back.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <geom/geom.h>

#define	G_SIMDISK_CLASS_NAME	"SIMDISK"

static MALLOC_DEFINE(M_SIMDISK, "simdisk", "dummy drives");

struct sd_softc {
	struct mtx		sd_mtx;
	struct callout		sd_co;
	TAILQ_HEAD(, bio)	sd_q;		/* in service order */
	sbintime_t		sd_free;	/* when the device is next free */
	off_t			sd_next;	/* where the last request ended */
	uint64_t		sd_wns_mib, sd_rns_mib;	/* ns per MiB, 0: no limit */
	sbintime_t		sd_lat, sd_seek;
	int			sd_cpu;
};

static struct g_class g_simdisk_class;
static int sd_ncreated;

SYSCTL_DECL(_kern_geom);
static SYSCTL_NODE(_kern_geom, OID_AUTO, simdisk, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "dummy drives for benchmarks");

static void
sd_fire(void *arg)
{
	struct sd_softc *sc = arg;
	TAILQ_HEAD(, bio) done;
	struct bio *bp;
	sbintime_t now = sbinuptime();

	TAILQ_INIT(&done);
	mtx_assert(&sc->sd_mtx, MA_OWNED);
	while ((bp = TAILQ_FIRST(&sc->sd_q)) != NULL &&
	    (sbintime_t)(uintptr_t)bp->bio_driver1 <= now) {
		TAILQ_REMOVE(&sc->sd_q, bp, bio_queue);
		TAILQ_INSERT_TAIL(&done, bp, bio_queue);
	}
	if (bp != NULL)
		callout_reset_sbt_on(&sc->sd_co, (sbintime_t)(uintptr_t)bp->bio_driver1,
		    0, sd_fire, sc, sc->sd_cpu, C_ABSOLUTE);
	mtx_unlock(&sc->sd_mtx);
	while ((bp = TAILQ_FIRST(&done)) != NULL) {
		TAILQ_REMOVE(&done, bp, bio_queue);
		bp->bio_driver1 = NULL;
		g_io_deliver(bp, 0);
	}
	mtx_lock(&sc->sd_mtx);
}

static void
sd_start(struct bio *bp)
{
	struct sd_softc *sc = bp->bio_to->geom->softc;
	sbintime_t now, start, svc, due;
	uint64_t ns_mib;
	bool first;

	switch (bp->bio_cmd) {
	case BIO_READ:
	case BIO_WRITE:
		break;
	case BIO_FLUSH:
	case BIO_DELETE:
		g_io_deliver(bp, 0);
		return;
	default:
		g_io_deliver(bp, EOPNOTSUPP);
		return;
	}
	bp->bio_completed = bp->bio_length;
	bp->bio_resid = 0;
	ns_mib = bp->bio_cmd == BIO_WRITE ? sc->sd_wns_mib : sc->sd_rns_mib;
	if (ns_mib == 0 && sc->sd_lat == 0 && sc->sd_seek == 0) {
		g_io_deliver(bp, 0);
		return;
	}
	svc = nstosbt(ns_mib * (uint64_t)bp->bio_length / (1024 * 1024));
	now = sbinuptime();
	mtx_lock(&sc->sd_mtx);
	start = sc->sd_free > now ? sc->sd_free : now;
	if (bp->bio_offset != sc->sd_next)
		svc += sc->sd_seek;
	sc->sd_next = bp->bio_offset + bp->bio_length;
	sc->sd_free = start + svc;
	due = sc->sd_free + sc->sd_lat;
	bp->bio_driver1 = (void *)(uintptr_t)due;
	first = TAILQ_EMPTY(&sc->sd_q);
	TAILQ_INSERT_TAIL(&sc->sd_q, bp, bio_queue);
	if (first)
		callout_reset_sbt_on(&sc->sd_co, due, 0, sd_fire, sc, sc->sd_cpu,
		    C_ABSOLUTE);
	mtx_unlock(&sc->sd_mtx);
}

static int
sd_access(struct g_provider *pp, int r, int w, int e)
{
	return (0);
}

struct sd_args {
	char		name[32];
	u_int		mib, wmbps, rmbps, lat_us, seek_us;
	int		error;
};

static uint64_t
sd_ns_mib(u_int mbps)
{
	return (mbps == 0 ? 0 : 1048576ULL * 1000 / mbps);
}

static void
sd_add_event(void *arg, int flag)
{
	struct sd_args *a = arg;
	struct g_geom *gp;
	struct g_provider *pp;
	struct sd_softc *sc;

	g_topology_assert();
	if (flag == EV_CANCEL) {
		a->error = ECANCELED;
		return;
	}
	LIST_FOREACH(gp, &g_simdisk_class.geom, geom) {
		if (strcmp(gp->name, a->name) == 0) {
			a->error = EEXIST;
			return;
		}
	}
	sc = malloc(sizeof(*sc), M_SIMDISK, M_WAITOK | M_ZERO);
	mtx_init(&sc->sd_mtx, "simdisk", NULL, MTX_DEF);
	callout_init_mtx(&sc->sd_co, &sc->sd_mtx, 0);
	TAILQ_INIT(&sc->sd_q);
	sc->sd_wns_mib = sd_ns_mib(a->wmbps);
	sc->sd_rns_mib = sd_ns_mib(a->rmbps);
	sc->sd_lat = ustosbt(a->lat_us);
	sc->sd_seek = ustosbt(a->seek_us);
	sc->sd_cpu = sd_ncreated++ % mp_ncpus;
	gp = g_new_geomf(&g_simdisk_class, "%s", a->name);
	gp->softc = sc;
	pp = g_new_providerf(gp, "%s", a->name);
	pp->flags |= G_PF_ACCEPT_UNMAPPED | G_PF_DIRECT_SEND | G_PF_DIRECT_RECEIVE;
	pp->mediasize = (off_t)a->mib * 1024 * 1024;
	pp->sectorsize = 512;
	g_error_provider(pp, 0);
	a->error = 0;
}

static void
sd_free(struct g_geom *gp)
{
	struct sd_softc *sc = gp->softc;

	gp->softc = NULL;
	callout_drain(&sc->sd_co);
	mtx_destroy(&sc->sd_mtx);
	free(sc, M_SIMDISK);
}

static int
sd_destroy_geom(struct gctl_req *req, struct g_class *mp, struct g_geom *gp)
{
	struct g_provider *pp;

	g_topology_assert();
	pp = LIST_FIRST(&gp->provider);
	if (pp != NULL && (pp->acr != 0 || pp->acw != 0 || pp->ace != 0))
		return (EBUSY);
	sd_free(gp);
	g_wither_geom(gp, ENXIO);
	return (0);
}

static void
sd_del_event(void *arg, int flag)
{
	struct sd_args *a = arg;
	struct g_geom *gp;

	g_topology_assert();
	if (flag == EV_CANCEL) {
		a->error = ECANCELED;
		return;
	}
	a->error = ENOENT;
	LIST_FOREACH(gp, &g_simdisk_class.geom, geom) {
		if (strcmp(gp->name, a->name) == 0) {
			a->error = sd_destroy_geom(NULL, &g_simdisk_class, gp);
			return;
		}
	}
}

static int
sd_sysctl_add(SYSCTL_HANDLER_ARGS)
{
	char buf[96] = "";
	struct sd_args a;
	int error;

	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	memset(&a, 0, sizeof(a));
	if (sscanf(buf, "%31s %u %u %u %u %u", a.name, &a.mib, &a.wmbps, &a.rmbps,
	    &a.lat_us, &a.seek_us) != 6 || a.mib == 0)
		return (EINVAL);
	error = g_waitfor_event(sd_add_event, &a, M_WAITOK, NULL);
	return (error != 0 ? error : a.error);
}
SYSCTL_PROC(_kern_geom_simdisk, OID_AUTO, add,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, sd_sysctl_add, "A",
    "NAME MIB WRITE_MBPS READ_MBPS LAT_US SEEK_US: a new drive");

static int
sd_sysctl_del(SYSCTL_HANDLER_ARGS)
{
	char buf[32] = "";
	struct sd_args a;
	int error;

	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	memset(&a, 0, sizeof(a));
	strlcpy(a.name, buf, sizeof(a.name));
	error = g_waitfor_event(sd_del_event, &a, M_WAITOK, NULL);
	return (error != 0 ? error : a.error);
}
SYSCTL_PROC(_kern_geom_simdisk, OID_AUTO, del,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, sd_sysctl_del, "A",
    "NAME: remove a drive nothing has open");

static struct g_class g_simdisk_class = {
	.name = G_SIMDISK_CLASS_NAME,
	.version = G_VERSION,
	.start = sd_start,
	.access = sd_access,
	.destroy_geom = sd_destroy_geom,
};

DECLARE_GEOM_CLASS(g_simdisk_class, g_simdisk);
MODULE_VERSION(geom_simdisk, 1);
