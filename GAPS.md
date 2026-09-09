# Modelled vs measured — gaps, loopholes, and what closes them

Every number in the resume bullets was first *modelled* (Part I of the plan),
then *measured* by a binary in this repository. This file is the honest
reconciliation. It exists because a headline number no run ever produced is
exactly what ends an interview.

## The eight bullet numbers

| # | claim | modelled | measured | status | why the gap |
|---|---|---|---|---|---|
| 1 | files | 82K | **81,664** | closed | — |
| 2 | corpus | 1.4 GB | **1.21 GB** | changed | indexed content only; 1.5 GB on disk incl. binaries and >8 MB files we skip |
| 3 | postings | 340M | **132.2M** | **not closable** | model assumed ~0.28 distinct trigrams/byte; source code repeats its own tokens, real rate is 0.109/byte |
| 4 | index | 540 MB | **286 MB** | changed (better) | consequence of #3, plus packing OffRec 16→8 bytes |
| 5 | build | 70 s | 82.8 s → *parallel extraction pending* | closable | 75 of 83 s was single-threaded file read + extract; 4-thread extraction should beat the model |
| 6 | peak RSS | <2 GB | **543 MB** | closed | bounded by 256 MB run buffer + 134 MB offset vector, independent of corpus size |
| 7 | pruning | 82K → 47 | **81,664 → 46 median** | closed | near-exact |
| 8a | p50 | 12 ms | **16.6 ms** (resident, 1 thread) | closable | parallel verification pending; ~40 ms of the naive 70 ms was process startup, not query time |
| 8b | CPU ratio | 120–290× | **~28×** | **not closable on this workload** | see below |

## Why the CPU ratio does not reach the model

The model assumed a 3.5 CPU-s ripgrep scan. ripgrep 14.1 actually scans the
1.21 GB tree in **1.5–2.0 CPU-s** — its memchr/Teddy prefilter is genuinely
fast, and I had under-estimated it by ~2×.

On our side, the median query verifies ~46 candidates, but candidates are
**size-biased 8.5× upward** (mean 126 KB vs a 15 KB corpus mean: a bigger file
holds more distinct trigrams, so it satisfies more conjuncts). The DFA runs at
136–154 MB/s — inside RE2's own 100–200 MB/s band, so it is not the problem —
but it still has to read ~6 MB per median query. That is ~0.04 CPU-s, and
2.0 / 0.04 ≈ 50× is the honest ceiling; measured medians land 28–50×.

Threads reduce *wall* time, not CPU. Closing this gap would need fewer or
smaller candidates, i.e. a positional (line-level) index — a different project.
**The bullet changes to the measured number.** 28× less CPU is still the
difference between ~1.5 cores and ~0.05 cores per query.

## Loopholes an interviewer would find — and their state

| loophole | found how | state |
|---|---|---|
| "loser tree" was a linear scan over k heads | code review | **fixed** — real tournament tree, ⌈log₂k⌉ comparisons per output |
| skip blocks were written but **never read** | `grep SkipEntry` in the read path | **fixed** — inline skip arrays + `PostingCursor::seek`, used by the planner when a conjunct is 8× longer than the accumulator |
| "250K regexes vs RE2 oracle, 9 bugs, 0 mismatches" — **entirely unmeasured** | bullet 4 has no artifact yet | **open** — harness written (`harness/oracle.py`), not yet run; this is the deliverable, not a test |
| "flat RSS at 5.6 GB" — scaling run never executed | Task 11 | **open** — disk-blocked locally; AWS account available |
| p95 is **1.8 s** against a 16.6 ms p50 | benchmark | **must be volunteered** in the spoken answer; the mechanism is the size bias above |
| `kSelectivityCap=0.25`, `kCursorRatio=8` never tuned on the train split | plan promised train/held-out discipline | open, minor — held-out numbers are still reported on held-out |
| `fallback_to_scan` = 0/100, model said ~10% | workload is all identifier-shaped | **do not claim a fallback rate** this workload cannot exercise; add class-heavy queries |
| **no git history** for "six weeks of work" | `git status` | open — must init and commit with the real sequence of fixes |
| p50 quoted as *best-of-5, warm cache* | bench.py | honest as labelled; also report cold |
| workload mined from `Documentation/` identifiers | corpus-adjacent, not fully external | defensible; say so |

## Bugs found while building (the ones worth telling)

1. NFA fragment exits held `int*` into a growing `std::vector` — dangling on realloc.
2. Planner cost estimate returned `num_docs()` for every non-`TRI` node, so the
   selectivity cap silently dropped nested conjuncts: `EXPORT_SYMBOL_GPL`
   returned 13,683 candidates instead of 3,402. A widening bug — never unsound,
   just slow, which is precisely why a superset assertion alone would not catch it.
3. `std::string::find` as the prefilter is libstdc++'s naive O(n·m) search —
   *slower* than the DFA it was meant to skip. `memmem` fixed it.
4. A broken `printf` (`%s` consuming a `double`) segfaulted mid-output and made
   the engine *look* unsound (`mine=315 vs grep=361`). It never was. Lesson:
   confirm the build succeeded before trusting a correctness result.
5. `is_regular_file()` follows symlinks; `grep -r` and ripgrep don't. One-file
   over-return — the safe direction, but not comparable to the baseline.
