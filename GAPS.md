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
| 5 | build | 70 s | **50.9 s** | **closed, beats model** | 4-thread extraction (41 s of it); was 82.8 s single-threaded |
| 6 | peak RSS | <2 GB | **1.05 GB** | closed | threads × 256 MB run buffer + 134 MB offset vector; was 543 MB single-threaded |
| 7 | pruning | 82K → 47 | **81,664 → 46 median** | closed | near-exact |
| 8a | p50 | 12 ms | **10.2 ms** (resident, 4 threads, warm, best-of-5) | **closed, beats model** | ~40 ms of the naive 70 ms was process startup, not query time |
| 8b | CPU ratio | 120–290× | **115×** | closed, at the band's floor | earlier "28×" was measurement error — see below |
| 9 | scaling | "flat RSS at 5.6 GB" | **1,685 → 1,825 MB as source went 1.21 → 3.57 GB** (×2.95), 132M → 301M postings | closed, **reworded** | RSS is *bounded* (threads × run buffer + offset table), not flat; and the four-repo set is 3.6 GB indexed, not 5.6 — measured on a spot c5.xlarge, identical binary |

## The CPU ratio: why the first number was wrong, and what the real one is

The first measurement said **28×**, and an earlier draft of this file called
that gap "not closable". It was measurement error. `bench.py` charged CPU via
`getrusage(RUSAGE_CHILDREN)`, which bills every query with fork/exec, the
288 MB mmap, and parsing 81,664 lines of `docs.txt` — ~40 ms of startup on a
~10 ms query. Reading CPU from inside the engine's repeat loop (all threads
summed, startup excluded) gives **0.017 CPU-s per median query** against
ripgrep's **1.96 CPU-s**: **115×**, at the floor of the modelled 120–290× band.

Two things remain true and are worth saying in the room. ripgrep 14.1 is
~2× faster than the model assumed (1.5–2.0 CPU-s, not 3.5 — its memchr/Teddy
prefilter is genuinely good). And candidates are **size-biased 8.5× upward**
(mean 126 KB vs a 15 KB corpus mean), which is why p95 is ~1 s against a 10 ms
p50: the DFA runs at 136–154 MB/s, inside RE2's band, but a common symbol can
hand it 160 MB to read. Threads halve that tail; a positional index would
remove it, and that is a different project.

The lesson is the one this whole file exists for: **a number you cannot
re-derive from two printed endpoints is not a measurement.** Both endpoints
are in `metrics.txt`.

## Loopholes an interviewer would find — and their state

| loophole | found how | state |
|---|---|---|
| "loser tree" was a linear scan over k heads | code review | **fixed** — real tournament tree, ⌈log₂k⌉ comparisons per output |
| skip blocks were written but **never read** | `grep SkipEntry` in the read path | **fixed** — inline skip arrays + `PostingCursor::seek`, used by the planner when a conjunct is 8× longer than the accumulator |
| "250K regexes vs RE2 oracle, 9 bugs, 0 mismatches" — **entirely unmeasured** | bullet 4 has no artifact yet | **closed, reworded** — **2,000 generated regexes vs google-re2 on a same-universe 2,000-file shard: 0 mismatches, 0 unsound.** The "9 bugs" claim is **dropped**: the oracle found none; the seven real bugs were caught by grep-diffing (BUGLOG). A 50K tier is running to raise the count; the bullet quotes whatever finished. Two harness design errors preceded this: per-pattern exec (~45 ms each → 40 h for 250K; fixed with `--patterns-file` batch mode) and a universe mismatch (engine over 81,664 files, oracle over 2,000 — `ANY`-degraded patterns verified the whole corpus; fixed with a shard-only index) |
| "flat RSS at 5.6 GB" — scaling run never executed | Task 11 | **closed, reworded** — ran on a spot c5.xlarge: identical binary, 1.21 → 3.57 GB of source (×2.95), 132M → 301M postings, peak RSS **1,685 → 1,825 MB (×1.08)**. Say **"bounded at 1.8 GB"**, not "flat" — and 3.6 GB, not 5.6 (LLVM is 2.7 GB, and the throwaway corpus set came to 4.5 GB on disk) |
| p95 is **1.8 s** against a 16.6 ms p50 | benchmark | **must be volunteered** in the spoken answer; the mechanism is the size bias above |
| `kSelectivityCap=0.25`, `kCursorRatio=8` never tuned on the train split | plan promised train/held-out discipline | open, minor — held-out numbers are still reported on held-out |
| `fallback_to_scan` = 0/100, model said ~10% | workload is all identifier-shaped | **do not claim a fallback rate** this workload cannot exercise; add class-heavy queries |
| **no git history** for "six weeks of work" | `git status` | open — must init and commit with the real sequence of fixes |
| p50 quoted as *best-of-5, warm cache* | bench.py | honest as labelled; also report cold |
| workload mined from `Documentation/` identifiers | corpus-adjacent, not fully external | defensible; say so |

## What the two structural fixes actually bought (measured)

| fix | claim it makes true | effect on latency |
|---|---|---|
| real loser tree (was a linear scan) | "external k-way merge sort on a **loser tree**" | none measurable at k=4; the argument is comparisons-per-output at large k |
| inline skip arrays + `PostingCursor::seek` (were written, never read) | "byte-offset **skip blocks**" | none — plan_ms is 0.2–0.9 ms with or without; planning was never the bottleneck |
| parallel verification, 4 threads | — | **1.7–1.8×** wall; nothing more at 8 threads (4 physical cores, DFA is memory-bound) |

Say this plainly in the room: the first two are about the claims being *true*,
not about speed. The thing that moves latency is verification, and the thing
that limits verification is candidate size bias.

## Improvement left on the table (not done — scope)

Only **417,918 of 16,777,216** trigram slots are non-empty: the 134 MB
direct-addressed offset table is **97.5% empty**. A sorted `(trigram, off,
count)` array of 418K × 12 B ≈ **5 MB** with binary search — or a minimal
perfect hash — would cut the index from 288 MB to ~155 MB at the cost of O(1)
→ O(log 418K) lookup, which is ~19 comparisons on a 5 MB array that fits in L2.
Almost certainly a net win. Deliberately not built: it changes a headline
number after the bullets were sized, and it is a better *interview answer*
("what would you change?") than a rushed commit.

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
6. **Found by the oracle, not by grep-diffing — the first one.** Tier A (10,000
   generated regexes) reported one mismatch: `(gre|(a)+.[mpwz])` missed
   `mxs-pinfunc.h`, where RE2 matched `'aßm'`. ß is two UTF-8 bytes (C3 9F);
   RE2 and ripgrep match `.` against one *code point*, my DFA matched one
   *byte*, so `[mpwz]` was tested against 0x9F. Synthesis was sound — the file
   was a candidate; the verifier was wrong. Fixed the way RE2 does it: `.` and
   any class admitting a byte ≥ 0x80 compile to the UTF-8 sequence automaton.
   A 1-in-10,000 case that 100 hand-picked kernel queries never exercised —
   which is the entire argument for a generated-regex oracle over a curated one.
