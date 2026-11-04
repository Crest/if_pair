# kwq-geli: GEOM_ELI on kwq(9) BULK queues

A copy of FreeBSD's GELI (`sys/geom/eli` and `lib/geom/eli` of
releng/15.1, 88e7371d9dc2) ported to run its crypto in kwq's BULK
workers instead of its own per-provider kernel processes.  It is the
proof client for the BULK class (../kwq/PLAN.txt P6): long CPU-bound
items, a reliability contract (no bio may be lost), stealing across
CPUs, and the question of what bulk crypto at an interrupt-thread
priority does to the rest of the machine (../kwq/KWQ.md S14).

It is a test vehicle, loadable beside the stock module, never a
replacement for it: it does not attach at boot and changes nothing in
the base system.

## Layout

| path | what |
|---|---|
| `sys/` | `geom_kwqeli.ko`: the kernel half, class `KWQELI` |
| `lib/` | `geom_kwqeli.so`: the userland half of geli(8), for `geom kwqeli ...` |
| `tests/` | ATF suite on md(4) providers (`Kyuafile`, `kwqeli_test.sh`); `geli_bench.sh`, the benchmark against stock GELI (its header lists the tests: `tput`, `resp`, `onecpu`, `scale` for many providers, `ssd` for a gnop(8) SATA SSD model), with `iolat.c`, the load of the last two, and `geli_report.sh` for its tables |

The history is reviewable against upstream: the first commit is the
unchanged copy, the second the rename, the third the port.

## Build and use

    make -C kwq-geli SYSDIR=/usr/src/sys          # or KERNBUILDDIR=... as for kwq
    kldload ../kwq/kwq/kwq.ko kwq-geli/sys/geom_kwqeli.ko
    mdconfig -a -t malloc -s 1g                    # md0
    env GEOM_LIBRARY_PATH=$(make -C kwq-geli/lib -V .OBJDIR) \
        geom kwqeli onetime -e AES-XTS -l 256 -s 4096 md0
    dd if=/dev/zero of=/dev/md0.kweli bs=1m count=512
    sysctl kern.kwq.bulk.queue.kweli-md0           # the provider's queue

Every geli(8) verb works as `geom kwqeli VERB`; the provider of `md0`
is `md0.kweli`.  `resume` takes the provider below (`md0`), as geli's
does: its metadata read through the suspended provider would park like
any other bio.

## What changed against GELI

Renamed so both modules load together: class `KWQELI`, providers
`<name>.kweli`, module `g_kwqeli` in `geom_kwqeli.ko`, sysctl tree
`kern.geom.kwqeli`, malloc type `kwqeli_data`, log prefix
`GEOM_KWQELI`, metadata backups `<name>.kweli`.  The on-disk format and
the metadata magic stay GELI's, so stock geli reads what kwq-geli wrote
and the other way round (the `cross_geli` case).  Because of that the
class never tastes: GELI's taste attaches boot-marked providers and asks
for their passphrases on the console, and a test class must not offer
itself the machine's own disks.  The loader passphrase cache and the key
intake wipe at mountroot are gone with it; they belong to geom_eli.ko.

The port (all marked `kwq-geli:` in the source):

- **Workers become one BULK queue per provider**
  (`kern.kwq.bulk.queue.kweli-<provider>`), created `KWQ_F_STEALABLE`
  by default.  The bios are the items, linked through `bio_queue`,
  which GELI's own queue used.  They enter at GELI's two queueing
  points: `g_eli_start()` for writes and authenticated reads, and
  `g_eli_read_done()` for decryption.
- **A refused bio is retried by GEOM.**  kwq refuses an item only at its
  per-CPU limit (`ENOBUFS`) or while the queue drains (`ENXIO`).  The
  first becomes `ENOMEM`, which `g_io_deliver()` answers by issuing the
  bio again after 1 ms, as for GELI's own buffer shortage (the
  `enobufs_retry` case).
- **Nothing on the request path sleeps.**  BULK handlers run under
  `THREAD_NO_SLEEPING()`.  GELI took a crypto request per sector with
  `M_WAITOK` inside its dispatch loop, and a key with `M_WAITOK` on a
  key-cache miss.  The privacy path takes them one per sector with
  `M_NOWAIT`: a shortage at any sector fails the bio with `ENOMEM`,
  and GEOM issues it again after a pause, the sectors done so far
  done again.  The pass's budget running out (checked every 16
  sectors) leaves the bio partly dispatched instead, and the handler
  requeues it in front of the rest to go on from the sector after the
  last completion (`bio_inbed`).  A 1 MiB bio is then no longer one
  uninterruptible item.  (The first version requeued a bio after a
  shortage too, and the worker retried it at once, at `PI_SOFT`, for
  as long as the shortage lasted.)  The integrity path takes all of
  a bio's requests and keys first (`g_eli_getreqs()`), so a shortage
  fails it whole.  `kern.geom.eli.blocking_malloc` is gone.  (Taking
  all requests first in the privacy path too, as the first version of
  the port did, emptied UMA's per-CPU cache, 256 zeroed requests per
  1 MiB bio, and cost lab's reads 18 %.)
- **One crypto session per CPU**, where GELI had one per worker thread.
  A bio is run on the session of the CPU whose worker takes it.
- **Suspend parks new bios** on the softc's queue and waits for the ones
  in flight, then frees the sessions.  Resume makes the sessions again
  and sends the parked bios through.  It sets up the keys outside the
  queue mutex, which GELI holds over the key cache's `M_WAITOK`
  allocations (a WITNESS warning on every resume).
- **Destroy drains the queue**, which runs what is left as GELI's
  workers did, then cancels the bios parked while suspended.

New knobs, in `kern.geom.kwqeli`:

| knob | default | meaning |
|---|---|---|
| `cpu` | -1 | CPU a bio is queued to: -1 `KWQ_CPU_ANY` (an idle BULK worker near the caller), -2 the caller's CPU, N CPU N for every provider (the several-busy-queues-on-one-CPU case) |
| `stealable` | 1 | queues of providers attached from now on are `KWQ_F_STEALABLE` |
| `weight` | 1 | DRR weight 1..8 of the queues of providers attached from now on |

`threads` is gone (the workers are kwq's, one per CPU).  CPU ids
above 254 are refused at attach: a bio carries its worker in eight bits
of `bio_pflags`, as in GELI.

## Known limits

- **A crypto driver that detaches under an attached provider.**
  crypto(9) moves the session to another driver inside the next
  dispatch (`crypto_invoke()`), and `crypto_newsession()` there
  allocates with `M_WAITOK`, in a BULK handler that must not sleep.
  GELI's workers could sleep there.  Unload `aesni` or `ossl` only
  with no kwq-geli provider attached.
- **A refusal at a full list recurses.**  GEOM issues an `ENOMEM` bio
  again at once by direct dispatch, so a bio refused in the caller's
  thread because its CPU's list is at `kern.kwq.bulk.limit` (4096)
  goes through `g_eli_start()` again, nested, until GEOM's half-stack
  guard sends it to `g_down`, which paces the retries.  Bounded, but
  it costs that thread's stack.
- **A resume whose sessions cannot be made** (the crypto driver gone
  since the suspend) fails, the provider stays suspended and its keys
  are wiped again; GELI asserted that it could not happen.

## Not changed

Key handling and the crypto itself (`g_eli_crypto.c`, `g_eli_hmac.c`,
`g_eli_key.c`, `pkcs5v2.c`), the metadata and every control verb but
suspend and resume.  Also hardware crypto drivers, which complete
asynchronously through crypto(9)'s own threads exactly as before.
