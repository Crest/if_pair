# BULK: stealing, scatter, a crypto client and GELI (PLAN P6)

Report of the BULK class work on branch kwq-bulk, 2026-11-03.  crest's
request: implement the BULK class, write a dummy client that benchmarks
crypto as a test case, copy GEOM ELI as kwq-geli and port it to kwq,
test it with md(4), and measure the impact on the system's
responsiveness and of several busy queues on one CPU, on the guest,
then lab and a07.  The design as built is SCHED.md S16; the port is
../kwq-geli (its README.md); what the measurements mean for GELI's
place among the classes is KWQ.md S14.1.  Raw rows are in
../samples/p6_*_2026-11-03.tsv.

## 1. What was built

| piece | where | tests |
|---|---|---|
| stealing: idle BULK workers take the older half of another CPU's live list of a `KWQ_F_STEALABLE` queue, nearest first in the cache topology, and run it as their own; the steal wake; a stealable pass takes one item while a helper sleeps nearby | kwq/kwq_worker.c, SCHED.md S16 | ATF steal, steal_off, stealdrain, stealdrain_discard |
| `kwq_scatter()` with a client-owned `struct kwq_group`, `kwq_group_leave()` | kwq/kwq.c, KWQ.md S3 | ATF scatter |
| `KWQ_CPU_ANY`: a sleeping worker's empty list near the caller first | kwq/kwq.c | (every kwq-geli case) |
| kwq_crypto.ko: AES-XTS through crypto(9) as GELI issues it, on BULK queues or on GELI's thread model | kwq_crypto/ | ATF verify, onecpu_fair, weight; tests/crypto_bench.sh |
| kwq-geli: GELI's crypto in a BULK queue per provider, class KWQELI beside stock GELI, same on-disk format | ../kwq-geli | 12 ATF cases; tests/geli_bench.sh |
| share limit: a period's first pass clamped only while contended; time beyond the allowance carried into the next period | kwq/kwq_sched.c, SCHED.md S6.3 | simulator cap_long, cut |

## 2. Correctness

| machine | kwq suite | kwq-geli suite |
|---|---|---|
| guest, GENERIC-DEBUG (WITNESS, INVARIANTS) | 34/34 with KWQ_LIST_CHECK (31 kwq_test, 3 kwq_crypto) | 12/12; 11/11 five times in a row with KWQ_LIST_CHECK and the double-enqueue assertion, resume 3/3 |
| lab, GENERIC | 31/31 (before kwq_crypto's cases) | 11/11 (before the resume case) |
| a07 | not run: waits for crest's go-ahead | not run |

The cases that carry the BULK class: `steal` (2000 items of 100 us
onto one CPU: 1506 ran elsewhere on the guest's four CPUs, the ideal
being 1500; steals_in == steals_out machine-wide), `scatter` (200 calls
of 64 items, one completion each, after all its items), `stealdrain`
(100 activate/drain cycles under stealing, nothing lost or doubled),
kwq-geli's `cross_geli` (stock geli reads what kwq-geli wrote and the
other way round) and `resume` (bios cut part way by the budget, read
back by stock geli).

## 3. The crypto client: BULK against GELI's thread model

kwq_crypto on the guest (GENERIC-DEBUG, aesni0, 64 KiB requests in 4
KiB operations, three repetitions; samples/p6_crypto_guest_2026-11-03.tsv):

| | BULK | thread model (PUSER) |
|---|---|---|
| whole machine, 16 requests in flight: MB/s | 4901 | 5016 |
| ... latency p99, us | 383 | 319 |
| N queues busy on one CPU: MB/s, N = 1 / 8 | 1520 / 1486 | 1565 / 1516 |
| ... Jain over the queues, N = 2 / 4 / 8 | 0.997 / 0.997 / 0.999 | 0.5 / 0.25 / 0.125 |
| ... a spin loop pinned there: its share of the CPU | 0.55 | 0.004 |
| ... a 1 kHz timer pinned there: p99 / max lateness, us | 270 / 1376 | 292 / 8 093 682 |

The thread model takes the CPU whole: a kernel thread at PUSER is above
every timeshare thread and, never returning to user mode, never acts on
ULE's slice end, so the first of N such threads keeps it from the
others (Jain 1/N) and from the user (the timer slept through the run in
one 8 s catch-up).  BULK shares the CPU evenly among its queues by
DRR and with the user by the share limit.  `weight` 3 against 1 on one
CPU gives exactly 3.00 times the throughput.  (A first version of the
benchmark started the probe after the load and measured the thread
model's starved probe as running alone: a share of 1.0.  Probes now
start first.)

## 4. kwq-geli against stock GELI

KWQ.md S14.1 has the table: on lab, kwq-geli's writes within 2 % of
stock GELI's; with every CPU busy a user spin loop pinned to a loaded
CPU gets 0.44 of it against 0.25; with four providers' crypto on one
CPU the 1 kHz timer pinned there is 0.3 ms late at the 99th percentile
against 37 ms, and the spin loop gets 52 % against 3 %, for 3 % of the
throughput.

The reads were 18 % slower on lab (22304 against 18321 MB/s, five
repetitions).  What it was not, each tried on lab with the tput test
three times: BULK's priority (at 55, just above PUSER, the reads were
slower still, 17560), the share limit (off: 18724), stealing (off:
18931), preemption (kwq-geli switches 75 % less than stock GELI, 98 k
against 394 k per second: GELI's `wakeup()` wakes every worker of the
provider).  What it was: a kernel profile of 16 readers had the same
sample total for both and `aesni_crypt_xts` 12 % lower for kwq-geli,
with `uma_zalloc_arg` at 879 samples against 315 and `memset_erms` at
4459 against 2878.  The port took all of a bio's crypto requests,
256 zeroed ones per 1 MiB, before the first dispatch, where GELI takes
one, completes it inside the dispatch and reuses it for the next
sector.  The privacy path now dispatches per sector as GELI does
(../kwq-geli README.md); lab went down before it could be measured
there (section 5).

With the per-sector dispatch (HEAD, the guest only, three repetitions,
GENERIC-DEBUG so direction only; samples/p6_geli_guest_head_2026-11-03.tsv),
against stock GELI on the same boot:

| | stock GELI | kwq-geli before | kwq-geli HEAD |
|---|---|---|---|
| write / read, 4 providers, MB/s | 2022 / 2498 | -1 % / +2 % | 2137 / 2708 (+6 % / +8 %) |
| writers looping: timer p99, unpinned / pinned, us | 1703 / 2118 | -31 % / +0 % | 734 / 956 (-57 % / -55 %) |
| ... spin loop's share, unpinned / pinned | 0.82 / 0.67 | -24 % / -17 % | 0.74 / 0.57 (-10 % / -15 %) |
| four providers on one CPU: timer p99 there, us | 130645 | 1220 | 490 |
| ... spin loop's share of it (the load's half second left out) | 0.06 | 0.52 | 0.55 |

The budget check every 16 sectors is what halves the timer's lateness:
a bio no longer holds the worker for its whole length.  The spin loop
beside the spread load still gets less under kwq-geli on the guest than
under stock GELI, where lab showed the opposite; lab's numbers for HEAD
are owed.

## 5. Faults found, and the one that took lab down

| fault | found by | effect | fixed in |
|---|---|---|---|
| the share limit clamped a period's first pass uncontended too | kwq_crypto, four queues on one CPU | shares up to 1 : 2 between equal queues, Jain 0.92 | 3696ce3 |
| passes longer than the share limit's period went uncharged | geli_bench onecpu | a spin loop beside four kwq-geli providers on one CPU got 5-7 %, the timer 149 ms late at p99 | 79d9957 |
| a pass of a stealable queue could take all of a few large items | kwq-geli steal case | four writers' bios never stolen | ec6516b |
| kwq-geli took a bio's crypto requests all at once | lab profile | reads 18 % below stock GELI | 0ad0126 |
| the first per-sector dispatch kept its resume point in `bio_pflags`, 16 bits wide | the guest, twice, then lab | resumed bios restarted at sector 0: reads counted twice (panic "bio_completed can't be greater than bio_length"), writes encrypted twice; on lab bios cut by the budget never finished and BULK workers kept every CPU | never committed; 0ad0126 resumes from `bio_inbed` |
| a probe started under a load that held its CPU measured an idle CPU | crypto_bench | the thread model's starvation reported as a share of 1.0 | 952bb18 |

The lab hang, 2026-11-03 20:34 UTC: the uncommitted first per-sector
dispatch was built and loaded on lab for a read A/B at 20:33:50,
thirteen seconds after the same build had panicked the guest, which
was running its suite in the background unwatched; the panic's file
system rollback undid the copy there and later hid which build had
run.  Its writes ran at 161 MB/s,
the benchmark died part way, and the cleanup's detach of the sixteen
providers did not return: bios the budget cut restarted at sector 0
every pass and never finished, and the BULK workers at `PI_SOFT` kept
the CPUs.  lab answers ping, not ssh; it waits for crest.  Lessons
recorded: a struct field's width is read from the header, not assumed;
a kwq-geli change passes the guest's full suite, the resume case and
`KWQ_LIST_CHECK`, with the result read, before it leaves the guest;
the guest's copies are `sync`ed before a run, so that a panic cannot
roll them back.

## 6. Open

- lab: reboot (crest), then the read A/B and the onecpu and resp tests
  again with the per-sector dispatch, and kwq_crypto's cases.
- a07: crest's go-ahead for the new kernel code, then the suites and
  both benchmarks (128 CPUs, a flat topology: stealing's ring 1 is the
  whole machine, steal_scan 16 bounds it).
- The two share-limit changes alter NET's behaviour under contention
  (the simulator's `cut` case: the competing thread now gets the
  specified half where it got a third); an if_pair A/B on lab before
  kwq-bulk is merged.
- GELI and BLOCKING: the priority argument of KWQ.md S14 does not hold
  against BULK with its share limit; the reserved-worker argument for
  the swap and root-I/O path stands and needs P7.
