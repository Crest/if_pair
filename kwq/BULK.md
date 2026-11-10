# BULK: stealing, scatter, a crypto client and GELI (PLAN P6)

Report of the BULK class work, 2026-11-03 to 2026-11-07, on branch
kwq-bulk, which was merged into kwq on 2026-11-10.  Sections 1 to 6 are
the first report, with a07's and lab's runs of 2026-11-04; the later
sections carry their dates, and section 7.7 has kwq-geli as it is now.
crest's
request: implement the BULK class, write a dummy client that benchmarks
crypto as a test case, copy GEOM ELI as kwq-geli and port it to kwq,
test it with md(4), and measure the impact on the system's
responsiveness and of several busy queues on one CPU, on the guest,
then lab and a07.  The design as built is SCHED.md S16, and S17 for
the placement that replaced its idle path (section 8, P6g); the port is
../kwq-geli (its README.md); what the measurements mean for GELI's
place among the classes is KWQ.md S14.1.  Raw rows are in
../samples/p6_*.tsv.

## 1. What was built

| piece | where | tests |
|---|---|---|
| stealing: idle BULK workers take the older half of another CPU's live list of a `KWQ_F_STEALABLE` queue, nearest first in the cache topology, and run it as their own; the steal wake; a stealable pass takes one item while a helper sleeps nearby | kwq/kwq_worker.c, SCHED.md S16 | ATF steal, steal_off, stealdrain, stealdrain_discard |
| `kwq_scatter()` with a client-owned `struct kwq_group`, `kwq_group_leave()` | kwq/kwq.c, KWQ.md S3 | ATF scatter |
| `KWQ_CPU_ANY`: a sleeping worker's empty list near the caller first (`place` 0; since section 8 P8 is the default on one NUMA domain, `place` 2) | kwq/kwq.c, kwq/kwq_shard.c | (every kwq-geli case) |
| kwq_crypto.ko: AES-XTS through crypto(9) as GELI issues it, on BULK queues or on GELI's thread model | kwq_crypto/ | ATF verify, onecpu_fair, weight; tests/crypto_bench.sh |
| kwq-geli: GELI's crypto in a BULK queue per provider (two since 2026-11-07, one for reads: section 7.7), class KWQELI beside stock GELI, same on-disk format | ../kwq-geli | 21 ATF cases (two through fail(9) points; 14 at this report's first date); tests/geli_bench.sh |
| share limit: a period's first pass clamped only while contended; time beyond the allowance carried into the next period, forgiven while nothing else is runnable | kwq/kwq_sched.c, SCHED.md S6.3 | simulator cap_long, cap_onset, cut |

## 2. Correctness

| machine | kwq suite | kwq-geli suite |
|---|---|---|
| guest, GENERIC-DEBUG (WITNESS, INVARIANTS) | 34/34 with KWQ_LIST_CHECK (31 kwq_test, 3 kwq_crypto), again after the review's fixes | 14/14 after the review's fixes; before them 12/12, and 11/11 five times in a row with KWQ_LIST_CHECK and the double-enqueue assertion, resume 3/3 |
| lab, GENERIC, amd64, 16 CPUs | 34/34 at 61d10e2; at fbb3019 33/34, the `weight` case failing on lab's fast AES-NI (section 5) | 14/14 at 61d10e2 |
| a07, GENERIC, arm64, 128 CPUs | 33/34 at 4644a3b (31 kwq_test, 3 kwq_crypto), `no_arm_when_idle` skipped: arm64 has no per-CPU timer line in `vmstat -i` | 14/14 at 4644a3b |

The kernel code is the same at 4644a3b, fbb3019 and 61d10e2 (one
comment differs).  Before
the review lab passed 31/31 and 11/11 (7d306f7).  lab's syslog over the
whole run, every case and benchmark stage unloading its modules, has
no leak warning, no UMA zone freed non-empty, no panic and no WITNESS
line (P6g.2's gossip counters later brought one, 128 bytes of `M_KWQ`
per unload from 67bcf06, fixed in 8cc6246).  a07's first run, at 2952ec1, had the same results once
three cases were run again by themselves: a07's kyua crashed in
`yield` and in `providers` ("PID already in all_exec_data"), and the
crashed case's cleanup, still running, unloaded geom_kwqeli under
`enobufs_retry`.  The run script now waits for a crashed case's
processes and runs it once more.

On a07's flat ring of 127 the `steal` case's 2000 items of 100 us,
all queued to one CPU, ran 1929 elsewhere, on 48 CPUs (790 steals in
and out, 4772 items moved in each direction, some of them twice):
stealing reaches across the machine since 2f213fd.

The cases that carry the BULK class: `steal` (2000 items of 100 us
onto one CPU: 1506 ran elsewhere on the guest's four CPUs, the ideal
being 1500; steals_in == steals_out machine-wide), `scatter` (200 calls
of 64 items, one completion each, after all its items), `stealdrain`
(100 activate/drain cycles under stealing, nothing lost or doubled),
kwq-geli's `cross_geli` (stock geli reads what kwq-geli wrote and the
other way round) and `resume` (bios cut part way by the budget, read
back by stock geli).

## 3. The crypto client: BULK against GELI's thread model

kwq_crypto on the guest (GENERIC-DEBUG, aesni0), on lab (GENERIC,
aesni0, 16 CPUs) and on a07 (GENERIC, armv8crypto0, 128 CPUs), 64 KiB
requests in 4 KiB operations, three repetitions, medians;
samples/p6_crypto_guest_2026-11-03.tsv, p6_crypto_lab_2026-11-04.tsv
(61d10e2) and p6_crypto_a07_2026-11-04.tsv (4644a3b):

| | guest BULK | guest thread model (PUSER) | lab BULK | lab thread model | a07 BULK | a07 thread model |
|---|---|---|---|---|---|---|
| whole machine, 4 requests per CPU in flight (16, 64, 512): MB/s | 4901 | 5016 | 52429 | 51668 | 134243 | 74205 |
| ... latency p99, us | 383 | 319 | 127 | 95 | 447 | 767 |
| N queues busy on one CPU: MB/s, N = 1 / 8 | 1520 / 1486 | 1565 / 1516 | 5452 / 5461 | 5464 / 5505 | 1916 / 1911 | 1921 / 1924 |
| ... Jain over the queues, N = 2 / 4 / 8 | 0.997 / 0.997 / 0.999 | 0.5 / 0.25 / 0.125 | 0.999 / 0.999 / 0.999 | 0.942 / 0.795 / 0.451 | 0.999 / 0.999 / 0.999 | 0.5 / 0.25 / 0.125 |
| ... a spin loop pinned there: its share of the CPU | 0.55 | 0.004 | 0.52 | 0.001 | 0.52 | 0.002 |
| ... a 1 kHz timer pinned there, N = 8: p99 / max lateness, us | 270 / 1376 | 292 / 8 093 682 | 491 / 504 | 2 / 8 087 824 | 527 / 869 | 3 / 8 082 651 |

The thread model takes the CPU whole: a kernel thread at PUSER is above
every timeshare thread and, never returning to user mode, never acts on
ULE's slice end, so the first of N such threads keeps it from the
others (Jain 1/N) and from the user (the timer slept through the run in
one 8 s catch-up).  BULK shares the CPU evenly among its queues by
DRR and with the user by the share limit.  `weight` 3 against 1 on one
CPU gives exactly 3.00 times the throughput, given a backlog that
outlasts the three quanta (section 5: on lab a fixed depth did not).
(A first version of the
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
list per CPU; that is the likely cause, not profiled.  lab's 16 CPUs
show no such gap (52 429 against 51 668 MB/s).  On lab's one CPU the
thread model's starvation is less complete than on a07, Jain 0.94 /
0.80 / 0.45 rather than 1/N, but the slowest of eight queues got
11 MB/s, the spin loop 0.1 % and the timer one catch-up 8 s late.

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
there (section 5), and after crest's reboot kwq-geli reads 5 % faster
than stock GELI there (the lab table below).

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
under stock GELI, where lab shows the opposite, at HEAD as before.

On a07 (GENERIC, arm64, 128 CPUs, armv8crypto0; HEAD 4644a3b, three
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
at 2952ec1, which ended in its third repetition (section 5), gives the
same picture: reads 36873 against 46189 MB/s, the spin loop pinned
beside the writers 0.34 against 0.78, the timer on the one CPU 104 ms
against 0.81 ms at p99.

On lab (GENERIC, amd64, 16 CPUs, aesni0; 61d10e2, the kernel code of
4644a3b; 16 providers, five repetitions, medians;
samples/p6_geli_lab_2026-11-04.tsv), against stock GELI on the same
boot; "local" is kwq-geli with each bio queued on the CPU that issued
it (`kern.geom.kwqeli.cpu=-2`):

| | stock GELI | kwq-geli | not stealable | local |
|---|---|---|---|---|
| write, 16 providers at once, MB/s | 7641 | 8014 | 7751 | 7837 |
| read, MB/s | 21740 | 22811 (+5 %) | 22947 | 21921 |
| ... context switches per second while reading | 396 000 | 120 000 | 126 000 | 139 000 |
| writers looping: a spin loop's share, unpinned / pinned | 0.69 / 0.25 | 0.77 / 0.49 | 0.78 / 0.50 | 0.80 / 0.46 |
| ... a 1 kHz timer's p99 lateness, unpinned / pinned, us (idle: 2 / 2) | 431 / 1911 | 431 / 1560 | 384 / 1528 | 486 / 1231 |
| ... fork+exec, mean / max, us (idle: 356 / 385) | 5136 / 8693 | 4875 / 8028 | 4843 / 7294 | 4553 / 7444 |
| four providers' crypto on one CPU: timer p99 there, us | 37869 | 542 | 539 | 546 |
| ... the timer's catch-ups | 3809 | 0 | 0 | 0 |
| ... a spin loop's share of that CPU (the load's half second left out) | 0.033 | 0.52 | 0.52 | 0.52 |
| ... MB/s with the timer / with the spin loop there | 3479 / 3458 | 3495 / 1741 | 3489 / 1739 | 3500 / 1742 |

Against lab's run at 7d306f7 (KWQ.md S14.1, the same tests), the reads
went from 18 % behind stock GELI to 5 % ahead, and the spin loop
pinned beside the writers from 0.44 to 0.49 of its CPU.  Two numbers
of the one-CPU case got worse: the timer's p99 there rose from 301 to
542 us, still 70 times below stock GELI's, and kwq-geli's MB/s beside
the spin loop fell from 1997 to 1741 with the spin loop's share
unchanged.  Several changes lie between the two runs: the per-sector
dispatch, which ends passes on the budget every 16 sectors, the share
limit's carry change (70c2d94) and the ENOMEM path.  Which of them
made the difference is not measured.  fork+exec is slower than on
2026-11-03 under stock GELI too (4226 then), so that is the boot, not
the port.

## 5. Faults found, and the one that took lab down

| fault | found by | effect | fixed in |
|---|---|---|---|
| the share limit clamped a period's first pass uncontended too | kwq_crypto, four queues on one CPU | shares up to 1 : 2 between equal queues, Jain 0.92 | 1bf4b40 |
| passes longer than the share limit's period went uncharged | geli_bench onecpu | a spin loop beside four kwq-geli providers on one CPU got 5-7 %, the timer 149 ms late at p99 | 418cea9 |
| a pass of a stealable queue could take all of a few large items | kwq-geli steal case | four writers' bios never stolen | 456ab19 |
| kwq-geli took a bio's crypto requests all at once | lab profile | reads 18 % below stock GELI | 7b3f2af |
| the first per-sector dispatch kept its resume point in `bio_pflags`, 16 bits wide | the guest, twice, then lab | resumed bios restarted at sector 0: reads counted twice (panic "bio_completed can't be greater than bio_length"), writes encrypted twice; on lab bios cut by the budget never finished and BULK workers kept every CPU | never committed; 7b3f2af resumes from `bio_inbed` |
| a probe started under a load that held its CPU measured an idle CPU | crypto_bench | the thread model's starvation reported as a share of 1.0 | 3b0d2ae |
| steal rings ordered from the next CPU id up: the reach was one-way on rings wider than `steal_scan` | review | on a07's flat ring of 127 the helpers woken for CPU v (v+1..v+16) searched v+2..v+32: no stealing there, and every pass of a stealable queue cut to one item for helpers that could not help | 2f213fd |
| the carry charged time used while nothing else was runnable | review | an uncontended worker carried a whole period into every next one; contention's start cost the rest of that period and two more (simulator `cap_onset`: 0.81-0.87 of the steady service, now 1.15-1.20) | 70c2d94 |
| a failed `M_NOWAIT` key allocation drops the key-cache lock; the fallback then replaced the LRU key without looking the key number up again | review | a key in the tree twice under memory pressure on a provider larger than the key cache | 5fd79c8 |
| a bio short of a crypto request part way was requeued at once | review | its worker retried it at `PI_SOFT` for as long as the shortage lasted; now `ENOMEM` wherever it happens, and GEOM paces the retry (ATF `nomem`) | 5a5a555 |
| resume asserted that its sessions could be made | review | after a failure a session freed at suspend was used again on a production kernel; now the resume fails and the provider stays suspended (ATF `resume_fail`) | 8e68a3c |
| resume propagated the keys with the queue mutex held, as stock GELI does, and the key cache allocates with `M_WAITOK` there | the guest's dmesg, 2026-11-04 | a WITNESS warning on every resume on GENERIC-DEBUG since the port; on GENERIC a panic if the allocation slept while another thread waited for the mutex | 4a4cd77 |
| kwq_crypto turned `result_state` "done" before its teardown | a07 | the module refuses to unload (EBUSY) until the teardown ends, and crypto_bench unloads within milliseconds of "done": on a07, after the last run (eight thread-mode queues), it left kwq and kwq_crypto loaded | c9c51af |
| geli_bench took stock GELI workers still exiting from the previous test for the new providers' | a07 | 8192 workers (64 providers x 128 CPUs) exit for seconds; `cpuset` failed with ESRCH and ended the benchmark in its third repetition | 4644a3b |
| kwq_crypto's weight case took 32 requests of 256 KiB for several quanta of work | lab | aesni does the 8 MiB in 1.5 ms, less than queue 0's three quanta (a07: 4.4 ms); a pass serves what was queued when it began (SCHED.md S4.3), so queue 0 ran dry every round and got its 32 requests to queue 1's 21, a ratio of 1.52, the specified behaviour; the case now measures the CPU's rate first and takes a depth holding six quanta | 61d10e2 |

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

## 6. The share-limit changes and if_pair

The share limit changed three times on this branch (1bf4b40, 418cea9,
70c2d94; section 1, SCHED.md S6.3), and the simulator's `cut` case,
a NET flood of 20 us items beside an always-runnable thread, said NET
would change under contention: the thread now gets the specified half
where it got a third.  An A/B of if_pair on lab, 2026-11-04: A = kwq
d1e8d3a (kwq before this branch), B = 61d10e2, if_pair.c and
tests/compare.sh the same in both; the default eight tests interleaved
three times, then the single flow out of the box eight times
(samples/p6_sl_pin_lab_2026-11-04.tsv, p6_sl_oob_lab_2026-11-04.tsv).
Medians, the runs where they matter:

| | A | B | runs |
|---|---|---|---|
| a spin loop pinned to the flow's worker CPU: its share | 0.509 | 0.509 | 0.502-0.509 in all six |
| ... the flow meanwhile, Gbit/s | 105 | 110 | A 100-110, B 110, 110, 78 |
| a 1 kHz timer pinned there: p99 lateness, us | 518 | 515 | 515-518, one B run 358 |
| fork+exec pinned there: mean, us | 830 | 877 | A 671-858, B 873-885 |
| UDP request-reply beside the flow: p99, us | 15 | 14 | |
| the timer unpinned under 32 connections: p99, us | 25 900 | 25 900 | |
| single flow, out of the box: Gbit/s | 140 | 151 | A 76-151, B 137-152 |

The spin loop gets half the worker's CPU under both: the overshoot A
left uncharged is one item per cut pass, 20 us in the simulator and
about a microsecond for if_pair (a 16 KiB packet at 150 Gbit/s).
Nothing else moves beyond the spread but fork+exec, whose three B runs
all lie above A's three, 6 % at the median: three runs each, so it may
be noise.  B's third pinned repetition had the flow at 78 and 79
Gbit/s beside the spin loop and the timer, where every other run had
100 to 148, and the timer's p99 at 358 us; not traced.

## 7. Many providers, and a SATA SSD model

crest's question of 2026-11-04: what stock GELI's worker per CPU per
provider, n x m threads, does to the whole machine against kwq-geli's
queue per provider.  geli_bench.sh's `scale` test attaches 1 to 128
providers on md(4) devices of 32 MiB and loads them two ways, one
request in flight per provider from its own thread (iolat.c): 4 KiB
reads at random offsets and 1 MiB sequential writes; the probes run
unpinned beside the load.  Its `ssd` test puts a gnop(8) provider
under each of four 256 MiB devices that holds a request to the next
clock tick (0 to 1 ms) with a set probability, a model of a SATA SSD
in which the device sets the pace (../kwq-geli/tests/geli_bench.sh
has the calibration).  lab, d055f23, three repetitions, medians
(samples/p6_scale_lab_2026-11-04.tsv, p6_ssd_lab_2026-11-04.tsv); the
charts are in crest's dashboard "GELI Threads vs kwq Queues".

What the providers hold once attached, idle:

| providers | 1 | 16 | 128 |
|---|---|---|---|
| threads added, stock GELI / kwq-geli | 16 / 0 | 256 / 0 | 2048 / 0 |
| kernel memory, MiB | 0.3 / 0.1 | 5.1 / 2.1 | 40.6 / 16.6 |
| `ps -axH`, ms | 3.4 / 3.4 | 4.9 / 3.5 | 57.5 / 3.4 |
| attach, all of them, ms | 4 / 4 | 42 / 46 | 321 / 356 |

GELI's memory is its workers' processes, threads and kernel stacks;
kwq-geli's is the per-CPU sysctl nodes of each queue (about 6 KiB per
CPU), which the memory audit of the same day also found.  Idle, neither
costs CPU: GELI's workers sleep without a timeout.

Under load (`s` = 4 KiB reads, `b` = 1 MiB writes):

| providers | 1 | 16 | 32 | 128 |
|---|---|---|---|---|
| s: requests/s, GELI / kwq-geli | 194 k / 232 k | 676 k / 1.50 M | 686 k / 1.39 M | 663 k / 549 k |
| s: crypto worker wakeups per request | 12.4 / 1.0 | 15.6 / 1.3 | 15.9 / 2.1 | 15.9 / 2.1 |
| s: kernel CPU per request, us | 44 / 3.3 | 23 / 9.5 | 23 / 11 | 23 / 28 |
| s: request p99, us | 6.1 / 4.7 | 63 / 20 | 199 / 73 | 1688 / 1655 |
| s: CPU-bound loop's share | 0.83 / 1.00 | 0.28 / 0.78 | 0.25 / 0.55 | 0.25 / 0.36 |
| s: fork+exec, us | 504 / 342 | 2317 / 1035 | 2403 / 1267 | 2611 / 2777 |
| b: MB/s | 3191 / 3392 | 13250 / 13350 | 10050 / 8230 | 9832 / 5396 |
| b: kernel CPU per MiB, us | | 1254 / 1229 | 1595 / 1997 | 1685 / 3006 |
| b: fork+exec, us | 386 / 384 | 11460 / 6048 | 9044 / 57530 | 18240 / 48550 |

On the quiet machine fork+exec takes 358 us and the loop gets 1.00.
GELI queues each request and wakes every worker of the provider
(`wakeup(sc)`): 12 to 16 of its 16 bound workers wake per request,
one finds work, and a single provider's 4 KiB reads cost 44 us of
kernel time each against 3.3.  Up to one provider per CPU that
leaves the machine's other users far more: at 16, the loop's share
0.78 against 0.28, fork+exec 1.0 against 2.3 ms, at twice the reads.

Past two providers per CPU kwq-geli falls behind, and with the 1 MiB
writes far behind: its kernel CPU per MiB grows from 1.2 to 3.0 ms
between 16 and 128 providers where GELI's stays near 1.6, its
throughput drops to 55 % of GELI's, and a fork+exec beside the
writers waits 49 to 58 ms.  Not traced at this date; section 7.1
traces it and section 7.7 closes it on simulated drives.

The SSD model, four devices, the device the limit (MB/s and requests/s
equal for gnop alone, GELI and kwq-geli: 2096 MB/s sequential, 376 k
random reads/s):

| | gnop only | stock GELI | kwq-geli |
|---|---|---|---|
| sequential read: kernel CPU per 128 KiB, us | 19 | 80 | 68 |
| sequential write: same | 31 | 132 | 89 |
| random 4 KiB read: same | 3.4 | 12.9 | 8.1 |
| ... median latency, us | 26 | 50 | 39 |
| ... crypto worker wakeups per request | | 5.6 | 1.8 |
| ... CPU-bound loop's share | 1.00 | 0.74 | 0.83 |

What the encryption adds to a request's kernel time, above gnop alone,
is 20 % less with kwq-geli for sequential reads, 43 % for writes and
50 % for random reads, and its wakeups a third to a sixth of GELI's.
The model's p99 is its own tick, 1.1 ms, for all three.

### 7.1 With P8, against stock GELI on one boot (2026-11-05)

crest's question of 2026-11-05: how the new placement compares with
stock GELI's n x m threads.  geli_bench interleaved stock GELI, kwq-geli
under today's placement and under P8 at the G2 defaults (f57a679), three
repetitions, medians (../samples/p6g3/lab7, a07g).  4 KiB reads at
random offsets and 1 MiB sequential writes, one request in flight per
provider; CPU per request from the machine's sys share:

| lab, 16 CPUs | 4 | 16 | 32 | 64 | 128 |
|---|---|---|---|---|---|
| reads/s, stock GELI / today / P8 | 530 k / 804 k / 818 k | 701 k / 1.51 M / 1.65 M | 675 k / 1.40 M / 1.64 M | 658 k / 1.05 M / 1.54 M | 659 k / 542 k / 1.52 M |
| CPU per read, us, stock GELI / P8 | 22.6 / 3.8 | 22.1 / 8.3 | 23.0 / 9.0 | 23.6 / 9.7 | 23.6 / 9.8 |
| writes, MB/s, stock GELI / today / P8 | 10133 / 11607 / 11630 | 13074 / 13473 / 13363 | 10056 / 8240 / 8122 | 9788 / 5912 / 5790 | 9659 / 5452 / 5383 |
| CPU per MiB written, ms, stock GELI / P8 | 0.42 / 0.34 | 1.21 / 1.17 | 1.52 / 1.93 | 1.56 / 2.68 | 1.64 / 2.87 |

| a07, 128 CPUs | 4 | 16 | 64 | 128 |
|---|---|---|---|---|
| reads/s, stock GELI / P8 | 68 k / 288 k | 90 k / 1.06 M | 73 k / 1.45 M | 63 k / 945 k |
| CPU per read, us, stock GELI / P8 | 1809 / 13.9 | 1391 / 15.0 | 1702 / 45.9 | 1990 / 131 |
| writes, MB/s, stock GELI / P8 | 3391 / 3568 | 6420 / 6310 | 7040 / 7040 | 7425 / 7372 |
| a spin loop's share beside the reads, stock GELI / P8 | 0.45 / 1.00 | 0.15 / 1.00 | 0.08 / 1.00 | 0.06 / 0.97 |

P8 reads faster than stock GELI at every provider count on both
machines, 1.5 to 2.4 times on lab and 4 to 20 times on a07, where every
request wakes all 128 workers of its provider and stock GELI's CPU per
read is 1.4 to 2 ms.  One provider per CPU (tput and resp): lab level
(reads 22.2 to 22.4 GB/s, writes 8.4 to 8.6 for all three; under the
writers fork+exec p99 6.7 ms under P8 against 7.7, the spin loop 0.78
against 0.70); a07 reads 38.6 against 31.1 GB/s, writes level, fork+exec
p99 19.6 against 66.7 ms, the spin loop 0.60 against 0.30.  a07's SSD
model: sequential loads level at a fifth of stock GELI's CPU, random
reads 1.34 M against 68 k, level with gnop alone (1.37 M).

The writes are the exception, on lab only: level up to one provider
per CPU, then kwq-geli falls behind, to 56 % of stock GELI at 128, its
CPU per MiB rising where stock GELI's stays near 1.6 ms; P8 changes
nothing there (within 2 % of today's).  a07 never has more providers
than CPUs here and shows no gap.

**Why (lab, 128 providers; ../samples/p6g3/lab7/trace).**  A trace of
both stages at 16 and 128 providers (bulk.d: kwq probes per phase and a
kernel profile at 997 Hz) and an A/B of the candidates:

| 128 providers, 1 MiB writes | MB/s | CPU per MiB, ms | write p99, ms | spin loop beside |
|---|---|---|---|---|
| stock GELI | 9804 | 1.61 | 66.6 | 0.21 |
| P8 | 5326 | 2.90 | 133.2 | 0.09 |
| P8, queues not stealable | 5223 | 2.96 | 108.0 | 0.10 |
| P8, every bio on its caller's CPU | 5447 | 2.84 | 153.1 | 0.09 |
| P8, share limit off (`cap_permille` 1000) | 8527 | 1.85 | 72.4 | 0.20 |

Stealing and placement are not it; the share limit is most of it.
With md(4)'s threads and the load's own runnable on every CPU, BULK's
limit binds in nearly every 1 ms period (89 k hand-overs in 11 s at 128
providers, 4.6 k at 16), and its allowance per period, 500 us, is less
than one 1 MiB bio's encryption (about 0.75 ms on lab).  The passes
start clamped, kwq-geli's per-sector budget check cuts the bio, and the
average bio is encrypted in three pieces between hand-overs and other
providers' bios (106 k requeued remainders for 56 k bios at 128
providers, 50 k for 139 k at 16).  Each piece runs cold: per MiB the
same `aesni_crypt_xts` takes 0.54 kernel samples against 0.22 at 16
providers, `memmove_erms` 1.07 against 0.40 (though not by the
pieces: a write's copy is never cut, section 7.2), `fpu_kern_leave`
0.22 against 0.04, where stock GELI's workers, at PUSER under ULE's slices,
encrypt a bio in one piece (0.25 and 0.57 at 128).  The handler time
per bio went from 0.75 to 1.88 ms.  At this load the limit does not
even serve its purpose: the CPU it lends goes mostly to md(4), and the
spin loop beside the writers gets 0.09 of its CPU with the limit and
0.20 without.  With the limit off 13 % of the gap remains (1.85 against
1.61 ms per MiB), which section 7.5 traces to BULK outranking md(4).

### 7.2 Overruns: a write's first piece (2026-11-05)

An overrun (SCHED.md S4.3) is a pass longer than its budget by more
than one quantum x weight, 1 ms for kwq-geli's queues.  The trace of
section 7.1 counted 1.9 k and 2.6 k of them; `overrun.d`
(samples/p6g5) sized them on lab, P8 at the defaults, geli_bench
scale, one run:

| | 16 providers | 128 providers |
|---|---|---|
| passes | 226,844 | 184,281 |
| overruns | 2,822 (1.2 %) | 2,666 (1.4 %) |
| their budget | 256-1023 us in 92 % | 256-1023 us in 76 % |
| beyond it | 1-2 ms in 83 %, at most 4-8 ms | 1-2 ms in 86 %, at most 4-8 ms |
| items per overrun pass | 1, cut and requeued, in every one | the same |
| longest stretch between budget checks, overrun passes | 1-4 ms | 1-4 ms |
| any stretch between budget checks | 8-63 us in 93 % | 8-127 us in 93 % |
| DRR parks (rounds skipped) | 1,561 | 1,536 |
| deficit at the park | 64 us-2 ms in 87 %, at most 4-8 ms | 64 us-2 ms in 81 %, at most 4-8 ms |
| park to the queue's next pass | 1-16 us | median 4-8 ms, at most 65-131 ms |

kwq's charge (CPU time) agrees with dtrace's `vtimestamp` and with
wall time in these passes, so they are work, not preemption or a
ticker fault.  The scheduler does nothing to them beyond its ordinary
accounting: the counter and the `overrun` probe are diagnostics, the
pass is charged to the queue's deficit like any other, and a queue
still at or below 0 after the round's refill of 1 ms skips the round.
The debt is clamped at `penalty_rounds` x Q x w, 32 ms, which no park
came near, and it survives the queue going idle (KWQ_DEBT: the next
doorbell goes to the ring, without the boost).  At 16 providers a park
cost nothing: the same queue's next pass came 1-16 us later, so no
other queue was waiting.  At 128 the parked
queue waited a median 4-8 ms, which includes the share limit's
hand-overs and the other queues' passes that an unparked queue waits
for too; the park's own part is not separated.

The stretch is the first piece of a write.  `g_eli_crypto_run()`
allocates the 1 MiB output buffer (`g_eli_alloc_data()`, malloc(9)'s
large path) and copies the plaintext into it before its sector loop,
whose first budget check comes at sector 16.  Under the load both are
slow: of the workers' samples more than 300 us after a pass's last
budget check (about 9 % of all CPU time at 16 providers, 18 % at 128),
47 % and 11 % spin on a lock under `kmem_malloc_domainset()`
(`lock_delay()`), and 37 % and 73 % are the copy (`memmove_erms`).
Stock GELI runs the same allocation and copy, in threads ULE slices.
Fixes, none tried at this date, all in kwq-geli: copy 16 sectors at a
time inside the loop, before they are encrypted (the stretch shrinks to
16 sectors and the cipher reads the copy from cache); or drop the copy,
with the bio's data as crypto(9)'s input and the new buffer as its
output (`CSP_F_SEPARATE_OUTPUT`, `crypto_use_output_buf()`), as GELI's
own comment there proposes; and for the allocation, a cache of MAXPHYS
buffers in place of malloc(9)'s large path.

Built on 2026-11-07, both defaults (section 7.7): the separate output
(`kern.geom.kwqeli.sep_out`), so a write without authentication is no
longer copied first, and zones for the buffers (`big_bufs`).

### 7.3 BULK's 5 ms quantum and 10 ms share-limit period (2026-11-05)

c2f3a35 raised BULK's `quantum_us` from 1000 to 5000 and its
`cap_period_us` from 1000 to 10000, an allowance of 5 ms per period at
`cap_permille` 500, so that a pass holds a 1 MiB item with slack for
any well-behaved client (SCHED.md S8).  One boot of each machine at
bb6b1f2, stock GELI (n providers x m bound workers) against P8 at the
new defaults and at the old values (geli_bench's `kwqshard1`), three
repetitions interleaved, medians (../samples/p6g6/lab, a07).  "Cuts"
are kwq-geli's requeued remainders per 1 MiB bio handed over, from the
BULK queues' `requeued` and `items` counters: 0 means every bio ran
whole in one pass.

| lab, 16 CPUs, scale | 4 | 16 | 32 | 64 | 128 |
|---|---|---|---|---|---|
| cuts per 1 MiB write, new / old | 0.00 / 0.00 | 0.08 / 0.56 | 0.81 / 1.70 | 0.83 / 1.87 | 0.85 / 1.89 |
| 1 MiB writes, MB/s, stock GELI / new / old | 10220 / 11631 / 11733 | 13272 / 13419 / 13461 | 10094 / 8837 / 8269 | 9812 / 6234 / 5768 | 9774 / 6053 / 5336 |
| CPU per MiB written, ms, stock GELI / new / old | 0.44 / 0.36 / 0.36 | 1.25 / 1.23 / 1.22 | 1.59 / 1.85 / 1.99 | 1.69 / 2.59 / 2.82 | 1.70 / 2.66 / 3.03 |
| write p99, ms, stock GELI / new / old | 0.6 / 0.4 / 0.4 | 2.9 / 3.0 / 3.0 | 7.9 / 8.5 / 8.8 | 31.2 / 29.1 / 47.7 | 66.6 / 101.7 / 140.5 |
| timer p99 beside the writes, us, stock GELI / new / old | 10 / 10 / 10 | 315 / 188 / 188 | 2407 / 2538 / 1640 | 3001 / 7041 / 2680 | 3319 / 7368 / 2940 |
| fork+exec p99 beside the writes, ms, stock GELI / new / old | 0.8 / 0.8 / 0.7 | 16.3 / 7.8 / 7.6 | 409 / 122 / 103 | 966 / 181 / 126 | 34 / 184 / 149 |
| 4 KiB reads/s, k, stock GELI / new / old | 533 / 824 / 820 | 706 / 1637 / 1641 | 683 / 1666 / 1644 | 666 / 1541 / 1532 | 663 / 1531 / 1529 |

| a07, 128 CPUs, scale | 4 | 16 | 64 | 128 |
|---|---|---|---|---|
| cuts per 1 MiB write, new / old | 0.00 / 0.00 | 0.01 / 0.80 | 0.38 / 1.18 | 0.64 / 1.23 |
| 1 MiB writes, MB/s, stock GELI / new / old | 3284 / 3482 / 3495 | 6429 / 6403 / 6393 | 6999 / 6997 / 6995 | 7330 / 7293 / 7290 |
| 4 KiB reads/s, k, stock GELI / new / old | 67 / 289 / 305 | 85 / 1091 / 1080 | 69 / 1354 / 1352 | 63 / 944 / 904 |
| fork+exec p99 beside the writes, ms, stock GELI / new / old | 1.7 / 1.1 / 1.1 | 1.9 / 1.2 / 1.2 | 2.1 / 1.3 / 1.2 | 40.6 / 16.9 / 21.5 |

| one provider per CPU (tput, resp) and four on one CPU (onecpu) | lab: stock GELI / new / old | a07: stock GELI / new / old |
|---|---|---|
| cuts per 1 MiB write, new / old | 0.05 / 0.59 | 0.59 / 1.17 |
| cuts per 1 MiB read, new / old | 0.04 / 0.20 | 0.11 / 0.91 |
| write MB/s | 8416 / 8413 / 8386 | 7134 / 7236 / 7231 |
| read MB/s | 21476 / 21943 / 22082 | 31318 / 37972 / 38088 |
| the same at 32 KiB sectors, write / read MB/s | 8439 / 8350 / 8367, 21310 / 21572 / 21955 | 7075 / 7080 / 7036, 31658 / 38405 / 38684 |
| resp: spin loop pinned beside the writers, share | 0.25 / 0.37 / 0.49 | 0.01 / 0.07 / 0.07 |
| resp: fork+exec p99, ms | 8.5 / 7.1 / 7.5 | 46.2 / 18.2 / 28.8 |
| onecpu: timer p99 on the loaded CPU, us | 37766 / 4925 / 547 | 107821 / 4993 / 833 |
| onecpu: timer max, us | 40470 / 5092 / 862 | 108554 / 6761 / 1078 |
| onecpu: spin loop's share, Jain over the providers | 0.09, 1.000 / 0.54, 1.000 / 0.55, 1.000 | 0.09, 1.000 / 0.54, 0.999 / 0.55, 1.000 |

a07's SSD model (16 devices): seqread and seqwrite level for all four
stages (8383-8387 MB/s, stock GELI's seqwrite 7435), randread 1.35 M/s
new, 1.32 M/s old, 62 k/s stock GELI; no cuts.

Up to one provider per CPU a 1 MiB bio now runs whole: 0.00 to 0.11
cuts per bio, on both machines and at 4 and 32 KiB sectors, where the
old values cut 0.2 to 0.9 at 4 KiB sectors (at 32 KiB sectors kwq-geli
checks its budget only every 16 sectors, 512 KiB, so the old values cut
there too only 0.02 to 0.14).  a07's tput writes, 0.59, are the
exception: at 128 providers on 128 CPUs every CPU is contended.  Past
one provider per CPU the cuts fall to a third to a half but stay, 0.4
to 0.85 per bio: with every period contended
the allowance is shared by all of the worker's queues, and the bio
that meets the period's end is cut; at 1 to 4 ms per bio under that
load (section 7.2) 5 ms hold one to three of them (inferred, not
traced).  Throughput moves only where the old values cut: lab's writes
at 32 to 128 providers gain 7 to 13 % (8837, 6234, 6053 MB/s against
8269, 5768, 5336) at 7 to 12 % less CPU per MiB, still 62 to 88 % of
stock GELI's; the write p99 at 128 providers falls from 141 to 102 ms
(stock GELI 67).  The other throughput rows are level within a few
percent; the latency rows follow.

The price is the bound the share limit gives the other threads on a
saturated CPU, as SCHED.md S8 says: a thread that shares a CPU with a
busy BULK worker now waits up to the allowance, 5 ms, at a time.
onecpu's timer, pinned to the loaded CPU, has its p99 at 4.9 and
5.0 ms on lab and a07 (0.55 and 0.83 ms with the old values; stock
GELI's bound workers 38 and 108 ms), while the spin loop's share stays
an equal one (0.54) and the providers' shares stay fair (Jain 1.000).
The same shows where lab's CPUs are all contended: the timer's p99
beside the writes at 64 and 128 providers 7.0 and 7.4 ms against 2.7
and 2.9 (stock GELI 3.0 and 3.3), fork+exec 181 and 184 ms against 126
and 149, and resp's pinned spin loop 0.37 of its CPU against 0.49.

### 7.4 Time per GELI block (2026-11-05)

AES-XTS-256, the configuration GELI is used in (`geli onetime -e
AES-XTS -l 256`, no authentication), one CPU, three repetitions,
medians (../samples/p6g6/blocks_*.txt, geliblocks_*.txt).

The crypto alone: kwq_crypto with one crypto(9) operation per block as
GELI issues them, each carrying its key (`rekey`, d08fd1a) so the
driver expands the key schedule per block as under GELI, on one BULK
queue on CPU 1 with 16 requests in flight, the data hot in the cache.
The key setup is 0.03 us of each on lab and 0.17 us on a07.

| per block, encrypt / decrypt | lab (Ryzen 7 5800X, aesni) | a07 (Ampere Altra, armv8crypto) | guest (Xeon D-1518, debug kernel) |
|---|---|---|---|
| 4 KiB | 0.82 / 0.82 us | 2.39 / 2.57 us | 3.08 / 3.07 us |
| 32 KiB | 4.47 / 4.48 us | 15.9 / 17.6 us | 18.4 / 18.3 us |
| 1 MiB of 4 KiB blocks | 200 / 199 us | 587 / 636 us | 748 / 746 us |
| 1 MiB of 32 KiB blocks | 142 / 142 us | 508 / 560 us | 589 / 585 us |
| 1 MiB as one block | 133 / 133 us | 497 / 549 us | 563 / 563 us |

Inside GELI: the worker's CPU time per block (ps), stock GELI with one
worker (`kern.geom.eli.threads=1`) pinned to CPU 1 and kwq-geli with
every bio on CPU 1's BULK worker, iolat from the other CPUs, 16 in
flight, on a 1 GiB swap-backed md(4).  This adds GELI's own work per
bio (queueing, wakeups, a write's buffer and copy) and data that
another CPU last touched:

| per block, stock GELI / kwq-geli | lab, encrypt (write) | lab, decrypt (read) | a07, encrypt (write) | a07, decrypt (read) |
|---|---|---|---|---|
| 4 KiB, in 4 KiB bios | 1.26 / 1.27 us | 1.07 / 1.08 us | 4.41 / 4.96 us | 7.01 / 7.06 us |
| 4 KiB, in 1 MiB bios | 1.00 / 0.99 us | 0.79 / 0.80 us | 3.26 / 3.28 us | 3.02 / 3.04 us |
| 32 KiB, in 32 KiB bios | 5.70 / 5.73 us | 5.97 / 6.02 us | 21.4 / 22.1 us | 38.5 / 40.2 us |
| 32 KiB, in 1 MiB bios | 6.16 / 6.09 us | 5.89 / 6.43 us | 23.5 / 23.7 us | 22.0 / 22.2 us |
| a 1 MiB bio of 4 KiB blocks | 0.26 / 0.25 ms | 0.20 / 0.20 ms | 0.84 / 0.84 ms | 0.77 / 0.78 ms |
| a 1 MiB bio of 32 KiB blocks | 0.20 / 0.20 ms | 0.19 / 0.21 ms | 0.75 / 0.76 ms | 0.70 / 0.71 ms |

Stock GELI and kwq-geli cost the same per block, within 5 % but for
a07's 4 KiB writes (12 %) and lab's 32 KiB reads in 1 MiB bios (9 %):
the crypto and GELI's code around it are the
same, and what separates the two is where and when the work runs.
Encryption and decryption cost the same on lab; on a07 decryption is
8 to 11 % slower.  lab is three to four times faster per byte.  32 KiB
blocks cost per byte about 30 % less than 4 KiB ones on lab and 15 %
on a07; inside GELI, in 1 MiB bios, they save 23 % of lab's writes,
nothing of lab's reads and 9 to 11 % on a07.  a07's
small reads pay the most beside the crypto: 7.0 us per 4 KiB read and
38.5 us per 32 KiB read against 3.0 and 22.0 us per block in 1 MiB
bios; probably the decrypted data arriving from md(4)'s copy on
another CPU, not traced.  For the BULK defaults of section 7.3: a
1 MiB bio is at most 0.26 ms of CPU on lab and 0.84 ms on a07, quiet;
the 5 ms allowance is 19 and 6 times that.

### 7.5 Where lab's write throughput goes (2026-11-05)

Section 7.3 leaves lab's 1 MiB writes behind stock GELI at 32 providers
and more.  At 96 % sys the machine is CPU-bound, so throughput follows
CPU per MiB, and the question is what the extra CPU is.  A 997 Hz
profile of every CPU (where.d), Zen 3's counters during the bulk phase
(pmcstat, all CPUs), and the wait of each write's data (waits.d), one
run each; then an A/B of BULK's priority, three repetitions, medians
(../samples/p6g8):

| lab, 1 MiB writes | 32: MB/s | CPU per MiB, ms | 128: MB/s | CPU per MiB, ms | 128: IPC | 128: L1D fills from DRAM per MiB |
|---|---|---|---|---|---|---|
| stock GELI | 10176 | 1.58 | 9807 | 1.69 | 0.40-0.42 | 0.87 MiB |
| BULK above md(4), limit on (the default) | 8855 | 1.85 | 6084 | 2.65 | 0.25 | 1.13 MiB |
| BULK above md(4), limit off | 8614 | 1.89 | 8661 | 1.91 | 0.36 | 0.93 MiB |
| BULK below md(4), limit on | 10276 | 1.61 | 6631 | 2.50 | 0.28 | 1.06 MiB |
| BULK below md(4), limit off | 11740 | 1.41 | 9949 | 1.66 | 0.43 | 0.85 MiB |

"Below md(4)" is `kern.kwq.bulk.priority=55`, against md's PRIBIO (43)
and BULK's default PI_SOFT (2); stock GELI's workers, at PUSER (56), are
below it too.

It is not extra work.  Instructions per MiB written are the same in all
setups, 2.86 to 2.99 M, and kwq's own functions come to 0.008 to
0.021 ms per MiB.  The extra time is the same copy and cipher taking more
cycles: at 128 providers, in the default setup against stock GELI, the
workers' `memmove_erms` 0.53 ms per MiB against 0.28, `aesni_crypt_xts`
0.51 against 0.27, `fpu_kern_leave` 0.19 against 0.07, and md(4)'s own
`memmove_erms` 0.53 against 0.32.  IPC falls from 0.40 to 0.25, with
30 % more L1D fills from DRAM and 25 % fewer from L3: the data has left
the cache.

Two separate causes, though not exactly additive:

- The share limit, at many providers per CPU: 6084 to 8661 MB/s at 128
  providers.  With the limit, a write's first piece to its ciphertext
  handed to md(4) takes 12.6 ms on average (p90 33-65 ms) against
  0.94 ms under stock GELI and 1.13 ms without the limit, because 0.8
  cuts per bio send the remainder behind the other queues' turns and
  the hand-overs (the cuts alone, section 7.6).  At 6 GB/s, 5 ms is as much data as lab's 32 MB L3,
  so the buffer just copied is gone when the cipher returns to it, and
  md(4) finds the ciphertext gone too.  At 32 providers the same cuts
  cost nothing while BULK is above md(4) (8855 against 8614), the bios
  finishing within 1.6 ms on average; below it the limit costs 12 %
  there as well (10276 against 11740).  What matters is that gap, not the wait before the copy:
  below md(4) with the limit off the plaintext waits 7 ms and the
  throughput is stock GELI's.
- BULK outranking md(4): 8661 to 9949 MB/s at 128 providers, 8614 to
  11740 at 32, limit off.  Below md(4), IPC and DRAM fills are stock
  GELI's or better, and kwq-geli passes stock GELI (15 % at 32
  providers).  Section 7.6 splits this in two: BULK's preempting at all,
  and its rank.  (This section's guess, that under stock GELI md preempts
  the cipher, was wrong: 43 is past ULE's preemption threshold.)  md's
  locality does not explain it: md copies on another core in 88 to 95 %
  of the cases in all setups.

The 4 KiB reads do not mind: below md(4) they run 5 % faster (1.74 M
against 1.65 M/s at 32 providers, 1.61 M against 1.53 M at 128), with
the same spin share and timer.  The price is on the latency rows, at
128 providers with the limit off: timer p99 11.2 ms (above md(4) 3.1,
stock GELI 3.4), fork+exec p99 57 ms (45, 32), while the spin loop gets
0.46 of its CPU (0.20, 0.22).  All of this is lab, at
`kern.kwq.bulk.priority=55` only with geli_bench's scale test and a
guest smoke; a07's writes are level with stock GELI already and were
not run at it.  Section 7.6 runs both machines, and finds what the scale
test could not: lab's 1 MiB reads lose 14 % at this priority.

### 7.6 Section 7.5's two steps, one at a time and together (2026-11-06)

The first step ends a pass between bios only: kwq-geli's
`kern.geom.kwqeli.budget_sectors` at 0, against 16, the knob of c8403c9
(geli_bench's stage kwqbio).  A write then runs from its copy to its
hand-over to md(4) in one go.  The second puts BULK below md(4):
`kern.kwq.bulk.priority=55`, as in section 7.5.  Neither is a default
yet (and neither became one: section 7.7).
Debug guest first, then the suites on both machines at all four setups:
the kwq suite at priority 55 39 of 39 (a07 38, `no_arm_when_idle` skipped
as always there), kwq-geli's 15 of 15 (at 0 `resume` skips, nothing being
cut), no leak, witness or panic line.  Then steps.sh on one boot of each
machine: three repetitions, in each stock GELI alone with kwq.ko unloaded
(the two never run together, geom_eli stays loaded for swap, a07's swap
unused), then kwq-geli as it is (base), bio, prio, both; medians
(../samples/p6g9).

| 1 MiB writes, MB/s | stock | base | bio | prio | both |
|---|---|---|---|---|---|
| lab, 32 providers | 10056 | 8177 | 8178 | 9352 | 10181 |
| lab, 64 | 9822 | 6139 | 8198 | 6650 | 9412 |
| lab, 128 | 9804 | 6103 | 8592 | 6637 | 9055 |
| lab, tput (16) | 8596 | 7822 | 8436 | 7929 | 8214 |
| a07, 64 | 6993 | 6973 | 6978 | 6995 | 7069 |
| a07, 128 | 7372 | 7313 | 7208 | 7228 | 7247 |
| a07, tput (128) | 6794 | 6856 | 7165 | 6797 | 7140 |

| reads | stock | base | bio | prio | both |
|---|---|---|---|---|---|
| lab, 1 MiB, MB/s | 21571 | 22609 | 22498 | 19338 | 19663 |
| lab, 1 MiB at 32 KiB sectors | 21268 | 21635 | 22080 | 19832 | 19873 |
| lab, 4 KiB, 128 providers, k/s | 660 | 1528 | 1527 | 1609 | 1613 |
| a07, 1 MiB, MB/s | 30916 | 38889 | 38043 | 38503 | 38271 |
| a07, 4 KiB, 128 providers, k/s | 61 | 977 | 971 | 1006 | 1002 |
| a07, SSD model randread, k/s | 60 | 1167 | 1085 | 1384 | 1383 |

| beside the writes, 128 providers | stock | base | bio | prio | both |
|---|---|---|---|---|---|
| lab timer p99, ms | 3.5 | 7.7 | 7.3 | 8.8 | 9.1 |
| lab fork+exec p99, ms | 32 | 202 | 226 | 55 | 133 |
| lab spin share | 0.22 | 0.06 | 0.16 | 0.20 | 0.07 |
| a07 timer p99, ms | 0.64 | 0.51 | 0.34 | 0.010 | 0.010 |
| a07 fork+exec p99, ms | 66 | 19 | 9.9 | 1.6 | 1.7 |
| a07 spin share | 0.26 | 0.64 | 0.95 | 0.99 | 0.99 |

| one CPU, beside the timer | stock | base | bio | prio | both |
|---|---|---|---|---|---|
| lab timer p99 / max, ms | 37.8 / 38.6 | 5.0 / 5.1 | 5.2 / 5.4 | 5.0 / 5.2 | 5.2 / 5.4 |
| a07 timer p99 / max, ms | 105 / 108 | 5.0 / 5.4 | 5.5 / 8.3 | 5.0 / 5.2 | 5.7 / 10.0 |

Lab's write repetitions spread by up to 14 %, in stock GELI's rows as well;
base, bio and both differ by more than that at 64 and 128 providers, and
per repetition both is level with stock GELI to 8 % behind it at 128.
a07's spread 1 %.  Fork+exec p99 is noisy: stock GELI's on lab reads 328,
318 and 32 ms at 32, 64 and 128 providers.  a07's randread is the bimodal
row of section 8; all six runs at priority 55 (1319-1405 k) lie above all
six at priority 2 (579-1269 k), which three repetitions cannot make more
than a tendency.  bio's lower median at priority 2 is not the check: a 4 KiB
bio has one sector and never reaches it.

**The first step is the cuts, not the hand-overs.**  Section 7.5 could not
separate the two.  With the check between bios the limit stays on and the
cuts go, 0.83 per bio to 0, and lab's writes at 128 providers rise 41 %, to
what the limit off reached.  One traced run per setup (where3_lab.sh, dtrace
and counters on, so its MB/s are low), 128 providers:

| | stock | base | bio | limit off | prio | both | prio, limit off |
|---|---|---|---|---|---|---|---|
| first piece to md(4), avg ms | 0.99 | 11.4 | 1.16 | 1.32 | 9.84 | 1.07 | 1.09 |
| MB/s | 9054 | 6178 | 8363 | 7503 | 6537 | 9124 | 9000 |
| IPC | 0.39 | 0.26 | 0.35 | 0.33 | 0.28 | 0.38 | 0.40 |
| L1D fills from DRAM per MiB | 0.90 | 1.13 | 0.92 | 0.99 | 1.07 | 0.88 | 0.90 |

The share limit itself, hand-overs included, costs nothing measurable once
it stops cutting bios.  At up to 32 providers bio changes little, as
section 7.5's "the same cuts cost nothing" says.  On a07 tput writes rise
5 % in every repetition, past stock GELI, with CPU per MiB 5 % down; the
scale writes do not follow (1.4 % down at 128 providers).  The price is the one bio a pass can overrun
by: a timer beside one busy CPU 0.2 ms later at p99 on lab, 0.5 ms on a07,
and on a07 its maximum 8.3 against 5.4 ms, stock GELI's being 108 ms.  At
16 and 128 providers on lab overrun.d found one overrun per phase in more
than 100 k passes, no park.

**The second step is two effects, and the larger is preemption.**
Section 7.5 read it as BULK against md(4), and its guess that under stock
GELI md(4) preempts the cipher does not hold.  ULE preempts for
`pri <= preempt_thresh` (sched_ule.c, `sched_shouldpreempt()`), and
`kern.sched.ule.preempt_thresh` is 40, PRI_MIN_KERN, on both machines: a
thread at 43 preempts nothing but the idle class and, from another CPU, a
batch-priority thread.  A sweep at budget_sectors 0, on lab, three
repetitions (prio_sweep.sh; priority 2 and 40 from their own interleaved
run, whose 2 reads 8169, 8492 and 23032):

| BULK priority | 32 providers, writes MB/s | 128 | 1 MiB reads, MB/s | read CPU per MiB, ms | |
|---|---|---|---|---|---|
| 2 | 7996 | 8485 | 22197 | 0.73 | preempts, above md(4) |
| 40 | 8138 | 8530 | 22515 | 0.70 | preempts, above md(4) |
| 41 | 9443 | 8549 | 18959 | 0.84 | does not preempt, above md(4) |
| 43 | 9469 | 8925 | 18962 | 0.84 | md's own |
| 44 | 10523 | 9408 | 19839 | 0.77 | below md(4) |
| 55 | 10344 | 9385 | 19570 | 0.81 | below md(4) |

Nothing moves from 2 to 40.  Past the threshold, at 41, lab's 32-provider
writes gain 18 % and its 1 MiB reads lose 15 %, the second number new here:
section 7.5 ran the scale test only, whose reads are 4 KiB.  The rank below
md(4) adds 10 to 11 % to the writes at 128 and 32 providers and gives some
of the reads back.  Why reads pay is not traced; their CPU per MiB rises as the
writes' falls, which looks like the same cooling on the other side, the
worker no longer taking md's data the moment it is there.

On a07 the reads do not pay, and the workers' not preempting is worth much
more: beside writes at 128 providers the timer's p99 falls from 0.51 to
0.010 ms and fork+exec's from 19 to 1.6 ms, with the spin loop at 0.99 of
its CPU.  On lab, saturated, fork+exec improves too (202 to 55 ms), but the
timer gets worse.  The one a07 row against it is resp's fork+exec p99, 42 ms against
17, stock GELI's 40.

**Both**, against the bar: on a07 kwq-geli is level with stock GELI or
ahead everywhere, by 5 % on writes in tput, 24 % on 1 MiB reads, 16 times on
4 KiB reads.  On lab it is ahead on 4 KiB reads (2.4 times), level at 32
and within 8 % at 64 and 128 providers on writes, and 9 % behind on
1 MiB reads, 7 % at 32 KiB sectors; the timer beside writes stays
2.6 times stock GELI's, as it was at base (2.2).

All of this stands on md(4), whose copying thread is what BULK competes
with at PRIBIO.  A real disk has no such thread, so what the rank below
md(4) buys here may not carry over; the preemption effect does not depend
on it.  No real SSD has been run.

The overruns of section 7.2 are lab's.  a07 has many more, at every setup:
at 128 providers 20 % of the passes at base (26.7 k of 131 k; the same
in section 7.3's round), 31 % at bio, 13 % at prio, 20 % at both; 8 %, 14 %,
7 %, 12 % at 64; none at 16.  CPU per MiB written there is 17 ms at 128
providers against 1.2 at 4, under stock GELI as well, so the passes are
long for the same reason the machine is slow; what the DRR does with them
on a07 has not been traced.

### 7.7 kwq-geli's defaults: where a bio stops, reads, buffers (2026-11-07)

Sections 7.2 to 7.6 left a write cut into cold pieces by the share
limit, its first piece stretched by malloc(9)'s lock and a copy of the
whole bio, and two steps of which neither became a default.  What was
built instead is the default now.  The first two rows are kwq's and know
nothing of GELI (GEOM uzip or any other client whose items can stop part
way takes the same calls); the rest are kwq-geli's, in
`kern.geom.kwqeli`:

| change | default | what | why |
|---|---|---|---|
| `kwq_item_begin()`, `kwq_item_stop()`, `kern.kwq.<class>.item_min_us` (651a7e9, SCHED.md S4.9) | BULK 2000, the others 0 | a handler asks inside an item whether to stop there; a higher class stops it at any such point, the pass's budget and the share limit only once the item has run this long in the pass | section 7.6's first step without a pass overrunning by a whole bio: a bio's pieces stay warm, and NET still stops it at the next 16 sectors |
| `KWQ_F_PROMPT` (651a7e9, SCHED.md S4.8) | - | a queue served ahead of the ring and the new list, a quantum per ring slot at most | for `read_queue` |
| `read_queue` (d25b01f) | 2 | a provider's reads on a queue of their own (`kwelir-<provider>`); 2: created `KWQ_F_PROMPT` | a read otherwise waits for a writer's pass |
| `big_bufs` | 1 | buffers above the swap zone's 132 KiB from UMA zones, one per power of two up to `maxphys`, without per-CPU caches | malloc(9)'s lock, about a third of lab's CPU in 1 MiB writes (section 7.2); with per-CPU caches eight writers reuse about 88 MiB, past lab's L3, and every buffer is cold |
| `sep_out` | 1 | a write is encrypted from the bio into the buffer (`CSP_F_SEPARATE_OUTPUT`), without the copy of the whole bio; not with authentication, and not where the driver has no separate output | section 7.2's copy, 36 % of the CPU once the lock was gone |
| `write_hold` | 2 | while reads and this many writes are below, one encrypted write per provider waits for the next write below to complete (`write_holds` counts them) | a drive that serves in order makes a read wait behind every write ahead of it; one write fewer there was all of stock GELI's lead on reads beside writes |

`budget_sectors` stays at 16 and BULK's priority at `PI_SOFT`: section
7.6's two steps were candidates in the battery's earlier rounds (stage
kwqbio and the group p55, lab/out2 there) and were not taken.  Tried and
rejected by measurement: a read that stops a running write
(`prompt_cut`: lab's reads 3.6 % lower with 76 % more cuts, since the
reads wait in the drive's queue, not for the CPU) and a pass that keeps
its turn when it is cut (`keep_turn`), both in
../samples/p6g11/prompt_cut.patch; and `write_hold` 1, which looked
better on the mixed set only because it moved CPU from HDD-like to
NVMe-like drives (HDD-like drives alone lose a third of their writes).
The time rule was chosen in a model first: sim/geli (gelisim, b1e598e).

The drives are simulated.  ../kwq-geli/tests/simdisk is geom_simdisk, a
provider that keeps no data and models time only: a rate per direction,
a latency, a seek, requests served in arrival order (geli_bench's
`MD_TYPE=sim`).  md(4)'s copying thread, which sections 7 to 7.6
measured against, is gone with it, so lab and a07 stand in for machines
with more and faster drives than their RAM could hold.  It does not
reorder, and the read-beside-write rows rest on that.  **No real drive
has been tested.**

The battery (../samples/p6g11, battery2.sh; out15 of each machine):
three repetitions with the stages' order rotated, one GELI at a time,
stock GELI with kwq.ko unloaded; scale, tput and resp, tput at 32 KiB
sectors, and drive sets: storage (8 NVMe-like and 90 HDD-like drives),
storage64 (the same at 64 KiB requests), on lab home (2 SATA-like), on
a07 compute (4 NVMe-like, every CPU spinning).  Medians, MB/s, as
score.py and final.py print them; "both" is reads beside writes:

| row | lab, stock | lab, kwq-geli | a07, stock | a07, kwq-geli |
|---|---|---|---|---|
| scale, 16 providers, 1 MiB writes | 23551 | 46915 | 7050 | 25587 |
| scale, 128 providers, 1 MiB writes | 21623 | 36342 | 7933 | 26620 |
| scale, 128 providers, 4 KiB reads | 2895 | 8177 | 280 | 4054 |
| tput, writes | 16238 | 21456 | 7642 | 44950 |
| tput, reads | 39749 | 40191 | 41178 | 96211 |
| storage, writes | 9819 | 12320 | 6719 | 33899 |
| storage, reads | 32072 | 37625 | 7775 | 25978 |
| storage, both: writes | 10842 | 14814 | 5950 | 22420 |
| storage, both: reads | 2135 | 3741 | 2949 | 5929 |
| home (lab), compute (a07), both: writes | 842 | 839 | 5807 | 11522 |
| home (lab), compute (a07), both: reads | 67 | 70 | 585 | 1012 |

Of the 25 scored rows per machine kwq-geli is at or above stock GELI on
all 25 of a07 and 24 of lab; the last is home's writes beside reads,
0.3 % behind (`python3 final.py lab/out15` in ../samples/p6g11).  Two
more rows of lab's home set sit at the drives' ceiling for every setup
and are not scored.  NET keeps its priority beside the load (net_ab and
net_final there; lab/net_hold is a run with a stale CPU pin, not a
result).  Suites on the guest's GENERIC-DEBUG: kwq 39 of 39 (36
kwq_test, 3 kwq_crypto), kwq-geli 21 of 21.

What these rows do not say: lab's storage rows reward `write_hold` 1
(reads beside writes 46 % above the default's), for the reason above;
with two writers per drive the hold never engages and stock GELI still
reads more (141 against 102 MB/s) for 4 % fewer writes; a07's 1 MiB
writes at 128 providers spread 18 % between repetitions; and on the
stand-in for a small machine with slow crypto (guest_slow.sh: the guest
with aesni detached) kwq-geli's writes beside reads are about 20 % below
stock GELI's for 3.4 times the reads, a trade crest accepted.  Section 9
has what is open.

## 8. Placement: P6g (2026-11-05)

The steal attempt of S16's idle path reads every stealable list in
reach before every sleep: with 128 kwq-geli providers on lab's 16 CPUs
it cost two thirds of the small-read rate (section 7; SPEC-p6g.md S0).
PLAN P6g designed placement and stealing anew (SPEC-p6g.md), simulated
the candidates in kwq/sim/place against today's code and an oracle
(gate G1, report https://claude.ai/artifact/U246kQmWvgBj6TYDJYyDYz),
and on crest's decision (D9) built two of them into the kernel behind
`kern.kwq.bulk.place`: 1 the gossip scheme, 2 P8, S16's placement with
the idle mask in group lines, a donor line in place of the scan, the
starvation watchdog, and since P6g.3 the mailbox and the chain (SCHED.md
S17).  Both were measured against today's code on one boot of lab and
of a07 (gate G2, report https://claude.ai/artifact/Jz2qDdxhD1qvUefHqXp9tY;
scripts, rows and the report's datasets in ../samples/p6g3).  Medians
of three repetitions unless stated; the kwq_crypto table pools every
round's runs of a configuration, 6 to 21 (today 18 on lab and 21 on
a07, P8 9, the mailbox 6 on lab and 9 on a07, the chain and gossip 6).

The kernel criteria (SPEC-p6g.md S1), with the thresholds the G2
report judges them by (../samples/p6g3/g2_report.py; S1 says "no worse
than today" for K2 and K3 and names further K4 loads): K1, 128
providers' small reads
within 3 % of no stealing, CPU per read within 5 %; K2, 128 providers'
bulk writes at least 98 % of today's; K3, 16 providers on lab and 64 on
a07, reads and writes at least 98 % of today's; K4, kwq_crypto with one
and with four stealable queues fed on CPU 1 (16 requests of 64 KiB in
flight each), at least 90 % of today's rate; K5, the probes beside the
load within today's range.

| scheme | lab | a07 |
|---|---|---|
| gossip (place=1) | K2 (0.943), K4 (0.10, 0.10) fail | K1 (0.942), K2 (0.9795), K3 (reads 0.909), K4 (0.07, 0.50) fail |
| P8 | K4 (0.09, 0.50) fails | K4 (0.28, 1.53) fails |
| P8, spill_idle=1 | all pass | K4 (0.45, 2.31) fails |
| P8 with the chain (`directed=2`) | all pass: K1 0.984 (CPU 1.019), K2 0.989, K3 1.085 and 0.993, K4 1.00 and 0.98, K5 fork+exec p99 6.3 ms (today 7.1) | all pass: K1 1.015 (CPU 0.984), K2 1.002, K3 1.048 and 0.999, K4 0.98 and 1.70, K5 fork+exec p99 16.8 ms (today 15.4) |

K4's pairs are one queue and four; lab's K2 for the chain is the
interleaved A/B below.  At 128 providers (K1, K2):

| | lab today | lab no stealing | lab gossip | lab P8 | a07 today | a07 no stealing | a07 gossip | a07 P8 |
|---|---|---|---|---|---|---|---|---|
| 4 KiB reads/s | 542 k | 1.56 M | 1.57 M | 1.53 M | 891 k | 886 k | 835 k | 928 k |
| kernel CPU per read, us | 28.5 | 9.5 | 9.4 | 9.8 | 139.6 | 140.2 | 146.4 | 133.7 |
| 1 MiB writes, MB/s | 5333 | 5191 | 5028 | 5277 | 7326 | 7354 | 7176 | 7367 |

P8 removes what the scan cost on lab and is level with or better than
today elsewhere; gossip's hand-off waits for a backlog of H (2 ms),
which a closed loop of a few requests never holds, and its tables are
a guess about cores the producer has not heard from (SPEC-p6g.md S2).

**The chain.**  K4's one queue fed on CPU 1 ran at 9 % of today on lab
and 28 % on a07 under P8: 16 requests in flight hold less than a
donor's 500 us, so nothing was stolen until a wake.  The mailbox (a
wake names its victim, `directed=1`) lifted lab to 91 %, but a07 stayed
at 56 % whatever `donor_us` (500, 2000 and 5000 alike), with p50 79 us
against today's 39: every item waited one service time.  A lone item
behind running work never makes its CPU a donor, so a thief that
finished its batch found no donor and slept, and the next item waited
for the next wake; S16's scan chains its thieves instead.  The chain
(an idle thief retries the victim of its last batch, d6fae5e):

| kwq_crypto, of today | lab, 1 queue | lab, 4 queues | a07, 1 queue | a07, 4 queues |
|---|---|---|---|---|
| today, MB/s | 53549 | 54234 (Jain 0.32) | 27368 | 21237 (Jain 0.42) |
| P8 | 0.09 | 0.50 | 0.28 | 1.53 |
| P8, mailbox | 0.91 | 0.90 | 0.56 | 1.63 |
| P8, chain | 1.00 | 0.98 | 0.98 | 1.70 |
| gossip | 0.10 | 0.10 | 0.07 | 0.50 |

On lab the chain's single queue needed 165 k wakes per run where the
mailbox alone needed 1.99 M (today 142 k), and P8 spreads four queues
evenly (Jain 1.00) where today's lets one of them take the machine.
`spill_idle` (an item that would wait goes to an idle CPU), tried
before the chain, halved a07's single queue beside it (0.46) and was
removed (D14), with `helper_donor`.

**Interleaved A/B rounds.**  kwq.ko loaded once, the knob settings in
turn in blocks of three runs of today's stage and three of P8's (lab4,
a07d, a07e under ../samples/p6g3):

| round | metric | today | P8 | P8, chain | P8, chain, `directed_us=10` |
|---|---|---|---|---|---|
| lab | 1 MiB writes at 128, MB/s | 5409 (12 runs) | 0.987 | 0.989 | |
| a07, 1 | ssd randread, IOPS | 1.15 M (12) | 1.067 | 1.003 | |
| a07, 2 | same | 1.24 M (18) | 0.817 | 0.667 | 0.884 |

Lab's K2 for the chain, 0.979 in its own round, is the edge of a
spread the A/B places at 0.989: the chain leaves P8's writes where they
were, about 1 % below today's.  a07's ssd randread (16 SSD models,
section 7) is not resolved: within one configuration the runs spread
from 0.50 M to 1.39 M IOPS (round 2; in round 1 today's from 0.90 M to
1.39 M, P8's one slow run at 0.71 M), and P8 without the chain came to
between 0.82 and 1.15 of today in three rounds.  The chain is not the cause of any
difference there: in round 2 it took 0 to 2 batches per block of
randread and the mailbox 229 to 641, while each block's steals came to
1.12 M to 1.38 M, today's stage and P8's together, which the file does
not separate (a07e/ssdab2_counters.txt); the lists' median item cost
was 7.5 to 8.5 us and their p90 under 10 (a07e/cost_*.txt), below the
gate.  Six runs per setting cannot resolve ten per cent;
crest took it as level with today (D15).

The code as it stands, measured at its defaults (f57a679: place=2,
directed=2, directed_us=10, the glitch fix and the mailbox's line)
against today's code and no stealing in the same round, on lab and a07
(../samples/p6g3/lab6, a07f; the criteria tab's "P8, G2 defaults"):

| | lab | a07 |
|---|---|---|
| K1, 128 providers' reads, of no stealing; CPU per read | 0.977; 1.026 | 1.108; 0.898 |
| K2, 128 providers' writes, of today | 0.988 | 1.005 |
| K3, reads and writes, of today (lab 16, a07 64 providers) | 1.089, 1.000 | 0.993, 1.001 |
| K4, kwq_crypto 1 and 4 queues, of today | 0.994, 0.984 | 1.003, 1.680 (Jain 1.00, today 0.42) |
| K5, fork+exec p99 under load, ms (today) | 6.7 (7.0) | 21.0 (12.9) |

All five hold on both machines.  K1 on lab has the least margin (0.977
against 0.97), as in every round before.  K5 holds by its rule (within
today's range), but on a07 P8's median fork+exec p99 under load was
above today's in four of five rounds (30.4 against 17.7, 33.6 against
22.3, 16.8 against 15.4, 21.0 against 12.9 ms; 17.7 against 26.9 the
other way), the ranges overlapping widely (today 11 to 38 ms, P8 9 to
40); the timer's lateness and the spin loop's share are level or better
under P8.  a07's randread with thirty runs of each, in ten interleaved
blocks: medians 0.98 M (today) and 1.03 M (P8), runs 0.47 M to 1.39 M
under both, a rank test finds no shift (p 0.94), and fourteen of P8's
runs and eleven of today's reach the fast mode near 1.3 M.  The modes
decide a median of thirty (its ratio's bootstrap 95 % interval runs
from 0.70 to 1.51), so the test bounds no difference smaller than
that; the chain took no batch in any block (the gate), the mailbox 3
to 73 per block (a07f/ssdab_final_counters.txt).

**Found on the way.**  The steal wake built the mailbox word from the
queue after the enqueue had dropped the list lock, a window in which a
drain and destroy may complete; the word is now built under the lock
(c26bbc8; the thief's side was safe, the word naming its queue by slot
and id under `kwq_steal_rm`).  The item cost's average took a07's
glitched CPU-time deltas: some lists' cost reached 2^63 ns, overflowing
the donor rule's backlog sums (a07e/cost_*.txt); it now leaves
glitches out (d1816b9).

**G2** (D12-D15, b96e660): P8 with the chain (`directed=2`,
`directed_us=10`) is the default on a machine of one NUMA domain,
today's code on more until a machine of several packages is measured
(the simulator's two-socket model saw P8 collapse near saturation);
`spill_idle` and `helper_donor` are gone; gossip stays for the hybrid
H4 (P8 within a package, gossip's hand-off across), which needs a
two-socket machine.

## 9. Open

- kwq-geli on real drives: every result since section 7.7 is on
  geom_simdisk, which serves in arrival order; the rows of reads
  beside writes, and `write_hold`'s threshold, rest on that model and
  are to be measured again on hardware (crest arranges it; nothing
  here writes to a real drive).
- `BIO_ORDERED`: neither stock GELI nor kwq-geli keeps a barrier
  write in its place among the others (kwq-geli's held write neither
  waits for one nor overtakes it, and that is all).  UFS sends one
  for a cylinder group's new inode blocks.  From reading the code, not
  tested; a fix would hold a barrier write until the earlier writes
  are sent down, and the later ones until it is.
- The FPU switch per crypto request: aesni and armv8crypto enter and
  leave the FPU context for every sector (`fpu_kern_enter()` with
  `FPU_KERN_NOCTX`) and do not look for a thread that keeps it
  (`is_fpu_kern_thread()`, as ossl does); up to 31 % of the CPU in
  lab's storage writes by the profile, less in truth.  With that base
  change a BULK worker could keep the context for its life.  A base
  change, so crest's call.
- `write_hold` with two writers per drive never engages, and stock
  GELI then reads more for 4 % fewer writes (section 7.7).
- kwq-geli's writes past one provider per CPU (sections 7, 7.1):
  closed on simulated drives by section 7.7's defaults.  As it stood
  on 2026-11-05: on lab at 128 providers 56 % of stock GELI's rate;
  the share limit's 500 us allowance per 1 ms period was shorter than
  one 1 MiB bio, so with md(4)'s threads runnable the bios were
  encrypted in cold pieces, and with the limit off the writes reached
  87 % of stock GELI's.  Of the candidates named then, the longer
  `cap_period_us` for BULK is the default since section 7.3, and the
  share-limit cut that a client honours only where it chooses is
  `kwq_item_stop()` with `item_min_us` (section 7.7).  Not built: a
  limit that counts only the competitors it is for, which would need an
  exported scheduler KPI such as "is anything runnable on this CPU
  below priority p" (`sched_runnable()` says only "something").  BULK
  past ULE's preemption threshold (41 and up, section 7.6) trades lab's
  1 MiB reads for its writes and for a07's latency and was not taken;
  why the reads pay is not traced.
- kwq-geli's write overruns (section 7.2): a write's first piece
  allocated and copied 1 MiB before its first budget check, 1-4 ms
  under load, 1.2-1.4 % of the passes on lab at the 1 ms quantum, about
  none at 5 ms; the copy and malloc(9) are gone from the default path
  since section 7.7, and the overruns have not been counted again.  On
  a07 at 128 providers 20 % of the passes overran at the defaults of
  section 7.6, not traced.
- Placement on a machine of two or more packages (section 8, D12):
  today's code stays there until one is measured, with P8 and the
  hybrid H4.
- a07's ssd randread under P8 against today (section 8): no shift in
  thirty runs each, but bimodal for both, so a median bounds nothing
  below about 30 %; the modes come with today's placement as well, so
  they are not the placement's; their cause is not traced (formerly:
  unresolved at six runs per setting).
- K5 on a07: P8's median fork+exec p99 beside the load above today's
  in four of five rounds, within the run-to-run range each time (section
  8); not traced.
- lab's one-CPU case got worse between 7d306f7 and 61d10e2 (section 4:
  the timer's p99 301 to 542 us, kwq-geli's MB/s beside the spin loop
  13 % lower); which of the changes between them, not traced.
- Stock GELI's resume holds `sc_queue_mtx` over the key cache's
  `M_WAITOK` allocations (releng/15.1 g_eli_ctl.c, `g_eli_ctl_resume()`):
  kwq-geli no longer does (4a4cd77); a report upstream is crest's call.
- kwq-bulk was merged into kwq on 2026-11-10, the share-limit changes
  with it (section 6: if_pair on lab is unchanged by them within the
  spread, fork+exec beside a flow perhaps 6 % slower).
- GELI and BLOCKING: the priority argument of KWQ.md S14 does not hold
  against BULK with its share limit; the reserved-worker argument for
  the swap and root-I/O path stands and needs P7.
