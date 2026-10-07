# kwq documentation

Technical documentation for `kwq(9)`, the kernel work queue service.
Four documents, in the order a new reader should take them:

| document | for | read it when |
|---|---|---|
| [overview.md](overview.md) | the model, the five rules, and a tutorial that builds a working client module | you are meeting kwq for the first time, or writing your first client |
| [cheatsheet.md](cheatsheet.md) | twelve patterns real clients use, each with recommendations and warnings, plus the anti-patterns | you are writing or reviewing client code |
| [reference.md](reference.md) | every function, flag, error, context rule, sysctl, counter and DTrace probe, with ranges and defaults | you need the exact contract or are diagnosing a running system |
| [whitepaper.md](whitepaper.md) | why the design is what it is, what was taken from other systems, what was rejected, and the measurements that decided; section 3.2 compares `netisr(9)` point by point | you are reviewing the design, wondering why a rule exists, or asking why netisr was not simply finished instead |

These are written for the client developer and the reviewer.  The
normative specifications are one level up and remain authoritative where
they differ:

- `../KWQ.md`: objects, lifecycle, API contracts, memory layout.
- `../SCHED.md`: the scheduler, its invariants, bounds and test plan.
- `../GLOSSARY.md`: terminology.
- `../OBSERVABILITY.md`: the operator's view of tunables and
  introspection, with worked examples.
- `../PLAN.txt`: implementation phases and their status.

Measurements quoted in these documents come from `../../NOTES.md` and
the raw logs in `../../samples/`.
