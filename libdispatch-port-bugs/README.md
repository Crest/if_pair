# devel/libdispatch FreeBSD port: bug report and reproducers

- `BUGREPORT.md` - the report: four issues in the port's FreeBSD-specific
  code, two of them process-killing, with root causes, upstream status and
  a verified fix.
- `libdispatch-freebsd-lock-fixes.diff` - the fix for issues 1 and 2 against
  the port's patched source tree (`src/shims/lock.h`, `src/shims/lock.c`).
- `repro_*.c` - reproducers; `make run` builds and runs them all against the
  installed library, `DISPATCH_LIB=/path/to/build make run` against a
  rebuilt one.  `run.sh` prints PASS/FAIL per case.

| file | issue | what it shows |
|---|---|---|
| `repro_min.c` | 1 | two threads with aliasing IDs; legal `dispatch_sync` -> SIGILL |
| `repro_tid_alias.c` | 1 | eight threads hammering one serial queue -> SIGILL within ms |
| `repro_once.c` | 1 | contended `dispatch_once` -> SIGILL |
| `repro_tid_ctl.c` | 1 (control) | same workload, threads in distinct ID blocks -> passes |
| `repro_group.c` | 2 | `dispatch_group_wait` -> rtld "Undefined symbol _dispatch_ulock_wait" |
| `repro_workq_loop.c` | 3 | the port's monitor loop counts 0 of 3 runnable workers; corrected loop counts 3 |
| `repro_runloop.c` | 4 | main-queue run-loop handle comes back as the pipe's write end; never readable |

Requires `devel/libdispatch` installed and the base toolchain.  The link
step uses `-fuse-ld=lld` because a binutils `ld` refuses to link against the
shipped library at all (that refusal is issue 2 seen from the linker).

`bugzilla/` holds the same findings prepared for bugs.freebsd.org as four
separate PRs (field values, body text, per-issue patch, reproducer to
attach); see `bugzilla/README.md` for the filing order.
