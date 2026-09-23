# Bugzilla submissions (bugs.freebsd.org), one PR per bug

Each `NN-*.txt` is a complete report: Bugzilla field values at the top,
body below.  Bodies are kept short for the maintainer; the full analysis
is in `../BUGREPORT.md`.

| PR | report | attach | depends on |
|---|---|---|---|
| 1 | `01-tid-alias.txt` | `01-tid-alias.patch`, `01-tid-alias-guard.patch` (optional), `../repro_min.c` | - |
| 2 | `02-ulock-wait.txt` | `02-ulock-wait.patch`, `../repro_group.c` | - |
| 3 | `03-workq-loop.txt` | `03-workq-loop.patch`, `../repro_workq_loop.c` | PR 1 |
| 4 | `04-runloop-handle.txt` | `04-runloop-handle.patch`, `../repro_runloop.c` | - |

File 1, 2 and 4 in any order, then 3 with "Depends on" set to PR 1's
number; afterwards replace the "filed separately" phrases with the PR
numbers.

Context the reports share: the port builds 6.1.1, whose tarball has no
FreeBSD support, plus the port's own 2022 FreeBSD patches.  Upstream's
separate FreeBSD implementation ships since swift-6.3-RELEASE
(2026-02-19).  Updating the port to 6.3.1 and dropping the shim patches
resolves PRs 1, 2 and 4; PR 3's loop typo is present in 6.3.x too (fixed
upstream 2026-09-02, unreleased) and still needs its patch.

The patches are unified diffs against the extracted and patched WRKSRC
(`work/swift-corelibs-libdispatch-swift-6.1.1-RELEASE`): the source-level
change, for review.  The maintainer turns them into `files/patch-*` and
commits as they see fit.

Verification (last re-run 2026-09-22, after the PR 2 clock change and
the `repro_tid_ctl.c` rewrite): all five patches apply individually and
together (`patch -p0`), a library built with all of them applied (port's
own `CMAKE_ARGS`) passes every row of the reproducer matrix in the parent
directory (`make run`, or `DISPATCH_LIB=/path/to/build make run`), while
the shipped package fails 7 of 8 rows.

`01-followup.txt` is a comment to post on the already-filed thread-aliasing
PR only if the earlier `repro_tid_ctl.c` was attached there.
