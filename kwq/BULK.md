# BULK: stealing, scatter, a crypto client and GELI (PLAN P6)

Report of the BULK class work on branch kwq-bulk, 2026-11-03, with
a07's runs of 2026-11-04.  crest's
request: implement the BULK class, write a dummy client that benchmarks
crypto as a test case, copy GEOM ELI as kwq-geli and port it to kwq,
test it with md(4), and measure the impact on the system's
responsiveness and of several busy queues on one CPU, on the guest,
then lab and a07.  The design as built is SCHED.md S16; the port is
../kwq-geli (its README.md); what the measurements mean for GELI's
place among the classes is KWQ.md S14.1.  Raw rows are in
../samples/p6_*.tsv.

## 1. What was built

| piece | where | tests |
|---|---|---|
| stealing: idle BULK workers take the older half of another CPU's live list of a `KWQ_F_STEALABLE` queue, nearest first in the cache topology, and run it as their own; the steal wake; a stealable pass takes one item while a helper sleeps nearby | kwq/kwq_worker.c, SCHED.md S16 | ATF steal, steal_off, stealdrain, stealdrain_discard |
| `kwq_scatter()` with a client-owned `struct kwq_group`, `kwq_group_leave()` | kwq/kwq.c, KWQ.md S3 | ATF scatter |
| `KWQ_CPU_ANY`: a sleeping worker's empty list near the caller first | kwq/kwq.c | (every kwq-geli case) |
| kwq_crypto.ko: AES-XTS through crypto(9) as GELI issues it, on BULK queues or on GELI's thread model | kwq_crypto/ | ATF verify, onecpu_fair, weight; tests/crypto_bench.sh |
| kwq-geli: GELI's crypto in a BULK queue per provider, class KWQELI beside stock GELI, same on-disk format | ../kwq-geli | 14 ATF cases (two through fail(9) points); tests/geli_bench.sh |
| share limit: a period's first pass clamped only while contended; time beyond the allowance carried into the next period, forgiven while nothing else is runnable | kwq/kwq_sched.c, SCHED.md S6.3 | simulator cap_long, cap_onset, cut |

## 2. Correctness

| machine | kwq suite | kwq-geli suite |
|---|---|---|
| guest, GENERIC-DEBUG (WITNESS, INVARIANTS) | 34/34 with KWQ_LIST_CHECK (31 kwq_test, 3 kwq_crypto), again after the review's fixes | 14/14 after the review's fixes; before them 12/12, and 11/11 five times in a row with KWQ_LIST_CHECK and the double-enqueue assertion, resume 3/3 |
| lab, GENERIC | 31/31 (before kwq_crypto's cases) | 11/11 (before the resume case) |
| a07, GENERIC, arm64, 128 CPUs | 33/34 at 0a37c05 (31 kwq_test, 3 kwq_crypto), `no_arm_when_idle` skipped: arm64 has no per-CPU timer line in `vmstat -i` | 14/14 at 0a37c05 |

The lab rows predate the review's fixes (section 5) and the per-sector
dispatch.  a07's first run, at f9120cb, had the same results once
three cases were run again by themselves: a07's kyua crashed in
`yield` and in `providers` ("PID already in all_exec_data"), and the
crashed case's cleanup, still running, unloaded geom_kwqeli under
`enobufs_retry`.  The run script now waits for a crashed case's
processes and runs it once more.

On a07's flat ring of 127 the `steal` case's 2000 items of 100 us,
all queued to one CPU, ran 1929 elsewhere, on 48 CPUs (790 steals in
and out, 4772 items moved in each direction, some of them twice):
stealing reaches across the machine since 50fbecd.

The cases that carry the BULK class: `steal` (2000 items of 100 us
onto one CPU: 1506 ran elsewhere on the guest's four CPUs, the ideal
being 1500; steals_in == steals_out machine-wide), `scatter` (200 calls
of 64 items, one completion each, after all its items), `stealdrain`
(100 activate/drain cycles under stealing, nothing lost or doubled),
kwq-geli's `cross_geli` (stock geli reads what kwq-geli wrote and the
other way round) and `resume` (bios cut part way by the budget, read
back by stock geli).

## 3. The crypto client: BULK against GELI's thread model

kwq_crypto on the guest (GENERIC-DEBUG, aesni0) and on a07 (GENERIC,
armv8crypto0, 128 CPUs), 64 KiB requests in 4 KiB operations, three
repetitions, medians; samples/p6_crypto_guest_2026-11-03.tsv and
p6_crypto_a07_2026-11-04.tsv (0a37c05):

| | guest BULK | guest thread model (PUSER) | a07 BULK | a07 thread model |
|---|---|---|---|---|
| whole machine, 4 requests per CPU in flight (16, 512): MB/s | 4901 | 5016 | 134243 | 74205 |
| ... latency p99, us | 383 | 319 | 447 | 767 |
| N queues busy on one CPU: MB/s, N = 1 / 8 | 1520 / 1486 | 1565 / 1516 | 1916 / 1911 | 1921 / 1924 |
| ... Jain over the queues, N = 2 / 4 / 8 | 0.997 / 0.997 / 0.999 | 0.5 / 0.25 / 0.125 | 0.999 / 0.999 / 0.999 | 0.5 / 0.25 / 0.125 |
| ... a spin loop pinned there: its share of the CPU | 0.55 | 0.004 | 0.52 | 0.002 |
| ... a 1 kHz timer pinned there, N = 8: p99 / max lateness, us | 270 / 1376 | 292 / 8 093 682 | 527 / 869 | 3 / 8 082 651 |

The thread model takes the CPU whole: a kernel thread at PUSER is above
every timeshare thread and, never returning to user mode, never acts on
ULE's slice end, so the first of N such threads keeps it from the
others (Jain 1/N) and from the user (the timer slept through the run in
one 8 s catch-up).  BULK shares the CPU evenly among its queues by
DRR and with the user by the share limit.  `weight` 3 against 1 on one
CPU gives exactly 3.00 times the throughput.  (A first version of the
benchmark started the probe after the load and measured the thread
model's starved probe as running alone: a share of 1.0.  Probes now
start first.)  The thread model's low p99 on a07 is the same
starvation: the timer's 500 periods before the load started were on
time, and the rest went in one catch-up 8 s late, one late sample in
about 500.

On the whole of a07 BULK delivers 1.8 times the thread model's
throughput (the thread model 54 000 to 77 000 MB/s over six
repetitions in two runs, BULK 134 060 to 134 290).  The thread model
keeps one list per queue under one mutex for its 128 threads, BULK a
list per CPU; that is the likely cause, not profiled.

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

On a07 (GENERIC, arm64, 128 CPUs, armv8crypto0; HEAD 0a37c05, three
repetitions, medians; samples/p6_geli_a07_2026-11-04.tsv), 64
providers, against stock GELI on the same boot:

| | stock GELI | kwq-geli | kwq-geli, not stealable |
|---|---|---|---|
| write, 64 providers at once, MB/s | 6692 | 6754 | 6770 |
| read, MB/s | 35514 | 46830 (+32 %) | 47325 |
| ... the machine's sys+intr share while reading | 0.73 | 0.40 | 0.39 |
| ... context switches per second while reading | 4 560 000 | 319 000 | 323 000 |
| writers looping: a spin loop's share, unpinned / pinned | 0.77 / 0.34 | 1.00 / 0.74 | 1.00 / 0.78 |
| ... a 1 kHz timer's p99 lateness, unpinned / pinned, us (idle: 3 / 3) | 49 / 10799 | 11 / 10234 | 11 / 10210 |
| ... fork+exec, mean / max, us (idle: 1238 / 1397) | 2619 / 9342 | 1428 / 2025 | 1449 / 2568 |
| ... steals per resp test (about 26 s) | | 120 | 0 |
| four providers' crypto on one CPU: timer p99 there, us | 105296 | 820 | 818 |
| ... the timer's catch-ups | 1586 | 0 | 0 |
| ... a spin loop's share of that CPU (the load's half second left out) | 0.029 | 0.52 | 0.52 |
| ... MB/s with the timer / with the spin loop there | 1251 / 1252 | 1222 / 607 | 1221 / 607 |

The writes are the same for all three, 105 to 119 MB/s per provider
with the CPUs far from saturated (sys+intr 0.45 to 0.65): bound below
the crypto, most likely by md(4), not measured.  The reads are not: stock
GELI's `wakeup()` wakes every worker of a provider, 128 here, and it
switches 14 times as often as kwq-geli for 32 % less (not profiled).
The pinned timer is 10 ms late at p99 under both: that is not the
crypto, but something else the writers keep busy on that CPU, most
likely md(4)'s worker threads, not measured.  Stealing changes nothing
measurable here: `KWQ_CPU_ANY` already places the bios on idle
workers, and the queues without stealing do as well.  The first run,
at f9120cb, which ended in its third repetition (section 5), gives the
same picture: reads 36873 against 46189 MB/s, the spin loop pinned
beside the writers 0.34 against 0.78, the timer on the one CPU 104 ms
against 0.81 ms at p99.

## 5. Faults found, and the one that took lab down

| fault | found by | effect | fixed in |
|---|---|---|---|
| the share limit clamped a period's first pass uncontended too | kwq_crypto, four queues on one CPU | shares up to 1 : 2 between equal queues, Jain 0.92 | 3696ce3 |
| passes longer than the share limit's period went uncharged | geli_bench onecpu | a spin loop beside four kwq-geli providers on one CPU got 5-7 %, the timer 149 ms late at p99 | 79d9957 |
| a pass of a stealable queue could take all of a few large items | kwq-geli steal case | four writers' bios never stolen | ec6516b |
| kwq-geli took a bio's crypto requests all at once | lab profile | reads 18 % below stock GELI | 0ad0126 |
| the first per-sector dispatch kept its resume point in `bio_pflags`, 16 bits wide | the guest, twice, then lab | resumed bios restarted at sector 0: reads counted twice (panic "bio_completed can't be greater than bio_length"), writes encrypted twice; on lab bios cut by the budget never finished and BULK workers kept every CPU | never committed; 0ad0126 resumes from `bio_inbed` |
| a probe started under a load that held its CPU measured an idle CPU | crypto_bench | the thread model's starvation reported as a share of 1.0 | 952bb18 |
| steal rings ordered from the next CPU id up: the reach was one-way on rings wider than `steal_scan` | review | on a07's flat ring of 127 the helpers woken for CPU v (v+1..v+16) searched v+2..v+32: no stealing there, and every pass of a stealable queue cut to one item for helpers that could not help | 50fbecd |
| the carry charged time used while nothing else was runnable | review | an uncontended worker carried a whole period into every next one; contention's start cost the rest of that period and two more (simulator `cap_onset`: 0.81-0.87 of the steady service, now 1.15-1.20) | 1dc88f6 |
| a failed `M_NOWAIT` key allocation drops the key-cache lock; the fallback then replaced the LRU key without looking the key number up again | review | a key in the tree twice under memory pressure on a provider larger than the key cache | 605838e |
| a bio short of a crypto request part way was requeued at once | review | its worker retried it at `PI_SOFT` for as long as the shortage lasted; now `ENOMEM` wherever it happens, and GEOM paces the retry (ATF `nomem`) | f6e2f04 |
| resume asserted that its sessions could be made | review | after a failure a session freed at suspend was used again on a production kernel; now the resume fails and the provider stays suspended (ATF `resume_fail`) | b52dc07 |
| resume propagated the keys with the queue mutex held, as stock GELI does, and the key cache allocates with `M_WAITOK` there | the guest's dmesg, 2026-11-04 | a WITNESS warning on every resume on GENERIC-DEBUG since the port; on GENERIC a panic if the allocation slept while another thread waited for the mutex | 5f72da2 |
| kwq_crypto turned `result_state` "done" before its teardown | a07 | the module refuses to unload (EBUSY) until the teardown ends, and crypto_bench unloads within milliseconds of "done": on a07, after the last run (eight thread-mode queues), it left kwq and kwq_crypto loaded | 7b8310b |
| geli_bench took stock GELI workers still exiting from the previous test for the new providers' | a07 | 8192 workers (64 providers x 128 CPUs) exit for seconds; `cpuset` failed with ESRCH and ended the benchmark in its third repetition | 0a37c05 |

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
roll them back.  Since 2026-11-04 the guest's dmesg is read too: the
resume's WITNESS warning had been there in every run with a resume
case.

The five rows from the steal rings to resume's sessions came from an
independent review of the whole branch, 2026-11-03.  The first matters most: stealing as first built
could not work on a07, and nothing the guest or lab ran could have
shown it, since their rings fit within `steal_scan`.  A model of the
ring order in userspace now checks the reach for flat 4 and 128 CPU
and SMT-pair topologies.  Two of the faults needed fault injection to
test: kwq-geli has two fail(9) points, `debug.fail_point.kwqeli_getreq`
(a sector's crypto request dropped) and `kwqeli_resume_session`, and
the ATF cases `nomem` (on the guest 29 `ENOMEM`s writing and 26
reading, the data intact for kwq-geli and stock geli) and `resume_fail`
use them.  The review also checked, and found sound, the list and
depth bookkeeping of cuts, steals and scatters, the drain fence, the
lock order, the group's fences, the `bio_pflags` widths and
`sc_inflight` on every exit.  What it found and is not fixed is in
../kwq-geli README.md, "Known limits": a crypto driver detaching under
an attached provider, and a refusal at a full list recursing up to
GEOM's stack guard.

## 6. Open

- lab: reboot (crest), then the read A/B and the onecpu and resp tests
  again with the per-sector dispatch, and kwq_crypto's cases.
- Stock GELI's resume holds `sc_queue_mtx` over the key cache's
  `M_WAITOK` allocations (releng/15.1 g_eli_ctl.c, `g_eli_ctl_resume()`):
  kwq-geli no longer does (5f72da2); a report upstream is crest's call.
- The two share-limit changes alter NET's behaviour under contention
  (the simulator's `cut` case: the competing thread now gets the
  specified half where it got a third); an if_pair A/B on lab before
  kwq-bulk is merged.
- GELI and BLOCKING: the priority argument of KWQ.md S14 does not hold
  against BULK with its share limit; the reserved-worker argument for
  the swap and root-I/O path stands and needs P7.
