# BULK: stealing, scatter, a crypto client and GELI (PLAN P6)

Report of the BULK class work on branch kwq-bulk, 2026-11-03, with
a07's and lab's runs of 2026-11-04.  crest's
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
| `KWQ_CPU_ANY`: a sleeping worker's empty list near the caller first | kwq/kwq.c | (every kwq-geli case) |
| kwq_crypto.ko: AES-XTS through crypto(9) as GELI issues it, on BULK queues or on GELI's thread model | kwq_crypto/ | ATF verify, onecpu_fair, weight; tests/crypto_bench.sh |
| kwq-geli: GELI's crypto in a BULK queue per provider, class KWQELI beside stock GELI, same on-disk format | ../kwq-geli | 14 ATF cases (two through fail(9) points); tests/geli_bench.sh |
| share limit: a period's first pass clamped only while contended; time beyond the allowance carried into the next period, forgiven while nothing else is runnable | kwq/kwq_sched.c, SCHED.md S6.3 | simulator cap_long, cap_onset, cut |

## 2. Correctness

| machine | kwq suite | kwq-geli suite |
|---|---|---|
| guest, GENERIC-DEBUG (WITNESS, INVARIANTS) | 34/34 with KWQ_LIST_CHECK (31 kwq_test, 3 kwq_crypto), again after the review's fixes | 14/14 after the review's fixes; before them 12/12, and 11/11 five times in a row with KWQ_LIST_CHECK and the double-enqueue assertion, resume 3/3 |
| lab, GENERIC, amd64, 16 CPUs | 34/34 at 9186555; at f953f11 33/34, the `weight` case failing on lab's fast AES-NI (section 5) | 14/14 at 9186555 |
| a07, GENERIC, arm64, 128 CPUs | 33/34 at 0a37c05 (31 kwq_test, 3 kwq_crypto), `no_arm_when_idle` skipped: arm64 has no per-CPU timer line in `vmstat -i` | 14/14 at 0a37c05 |

The kernel code is the same at 0a37c05, f953f11 and 9186555 (one
comment differs).  Before
the review lab passed 31/31 and 11/11 (01a87c3).  lab's syslog over the
whole run, every case and benchmark stage unloading its modules, has
no leak warning, no UMA zone freed non-empty, no panic and no WITNESS
line.  a07's first run, at f9120cb, had the same results once
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

kwq_crypto on the guest (GENERIC-DEBUG, aesni0), on lab (GENERIC,
aesni0, 16 CPUs) and on a07 (GENERIC, armv8crypto0, 128 CPUs), 64 KiB
requests in 4 KiB operations, three repetitions, medians;
samples/p6_crypto_guest_2026-11-03.tsv, p6_crypto_lab_2026-11-04.tsv
(9186555) and p6_crypto_a07_2026-11-04.tsv (0a37c05):

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

On lab (GENERIC, amd64, 16 CPUs, aesni0; 9186555, the kernel code of
0a37c05; 16 providers, five repetitions, medians;
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

Against lab's run at 01a87c3 (KWQ.md S14.1, the same tests), the reads
went from 18 % behind stock GELI to 5 % ahead, and the spin loop
pinned beside the writers from 0.44 to 0.49 of its CPU.  Two numbers
of the one-CPU case got worse: the timer's p99 there rose from 301 to
542 us, still 70 times below stock GELI's, and kwq-geli's MB/s beside
the spin loop fell from 1997 to 1741 with the spin loop's share
unchanged.  Several changes lie between the two runs: the per-sector
dispatch, which ends passes on the budget every 16 sectors, the share
limit's carry change (1dc88f6) and the ENOMEM path.  Which of them
made the difference is not measured.  fork+exec is slower than on
2026-11-03 under stock GELI too (4226 then), so that is the boot, not
the port.

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
| kwq_crypto's weight case took 32 requests of 256 KiB for several quanta of work | lab | aesni does the 8 MiB in 1.5 ms, less than queue 0's three quanta (a07: 4.4 ms); a pass serves what was queued when it began (SCHED.md S4.2), so queue 0 ran dry every round and got its 32 requests to queue 1's 21, a ratio of 1.52, the specified behaviour; the case now measures the CPU's rate first and takes a depth holding six quanta | 9186555 |

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

The share limit changed three times on this branch (3696ce3, 79d9957,
1dc88f6; section 1, SCHED.md S6.3), and the simulator's `cut` case,
a NET flood of 20 us items beside an always-runnable thread, said NET
would change under contention: the thread now gets the specified half
where it got a third.  An A/B of if_pair on lab, 2026-11-04: A = kwq
d1e8d3a (kwq before this branch), B = 9186555, if_pair.c and
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
has the calibration).  lab, 0ef1ebc, three repetitions, medians
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
writers waits 49 to 58 ms.  Not traced; section 9.

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
(an idle thief retries the victim of its last batch, 777b372):

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

**Found on the way.**  The steal wake built the mailbox word from the
queue after the enqueue had dropped the list lock, a window in which a
drain and destroy may complete; the word is now built under the lock
(b40b3a6; the thief's side was safe, the word naming its queue by slot
and id under `kwq_steal_rm`).  The item cost's average took a07's
glitched CPU-time deltas: some lists' cost reached 2^63 ns, overflowing
the donor rule's backlog sums (a07e/cost_*.txt); it now leaves
glitches out (946967d).

**G2** (D12-D15, 77e80dc): P8 with the chain (`directed=2`,
`directed_us=10`) is the default on a machine of one NUMA domain,
today's code on more until a machine of several packages is measured
(the simulator's two-socket model saw P8 collapse near saturation);
`spill_idle` and `helper_donor` are gone; gossip stays for the hybrid
H4 (P8 within a package, gossip's hand-off across), which needs a
two-socket machine.

## 9. Open

- kwq-geli past two providers per CPU (section 7): with 64 and 128
  providers of 1 MiB writes on lab's 16 CPUs its kernel CPU per MiB
  is nearly twice GELI's and fork+exec beside it 50 ms.  Candidates,
  none measured: the share limit yielding to the md(4) threads,
  stealing among 128 backlogged queues, the pass's per-sector budget
  checks suspending bios, the idle-worker scan of `KWQ_CPU_ANY`.  P8
  (section 8) removed the small reads' share of it (the idle path's
  scan), not the writes': at 128 providers P8's writes are level with
  today's.
- Placement on a machine of two or more packages (section 8, D12):
  today's code stays there until one is measured, with P8 and the
  hybrid H4.
- a07's ssd randread under P8 against today (section 8), unresolved at
  six runs per setting.
- lab's one-CPU case got worse between 01a87c3 and 9186555 (section 4:
  the timer's p99 301 to 542 us, kwq-geli's MB/s beside the spin loop
  13 % lower); which of the changes between them, not traced.
- Stock GELI's resume holds `sc_queue_mtx` over the key cache's
  `M_WAITOK` allocations (releng/15.1 g_eli_ctl.c, `g_eli_ctl_resume()`):
  kwq-geli no longer does (5f72da2); a report upstream is crest's call.
- Keeping the share-limit changes and merging kwq-bulk into kwq are
  crest's decisions (section 6: if_pair on lab is unchanged by them
  within the spread, fork+exec beside a flow perhaps 6 % slower).
- GELI and BLOCKING: the priority argument of KWQ.md S14 does not hold
  against BULK with its share limit; the reserved-worker argument for
  the swap and root-I/O path stands and needs P7.
