# Results

Every number below was printed by a binary in this repository. The raw,
append-only log is [`metrics.txt`](metrics.txt); this file is the summary.

## Setup

| | |
|---|---|
| Corpus | Linux 6.6 source tree (`linux-6.6.tar.xz`, sha256 in `corpus_sha256.txt`) |
| Indexed content | **81,664 files, 1.21 GB** after skipping binaries, files > 8 MB, and symlinks |
| Build machine | WSL2 Ubuntu 24.04, 2 vCPU, 3.9 GB RAM, ext4; `g++ 13.3 -O2 -march=native` |
| Scaling machine | AWS spot `c5.xlarge`, 4 vCPU, 8 GB, Ubuntu 24.04 |
| Baseline | ripgrep 14.1.0, default threads, `--no-ignore --hidden --max-filesize 8M` (the files the indexer walks), warm page cache |
| Oracle | google-re2 (Python bindings), multi-line anchors, file-set equality, exact superset check |
| Sanitizers | `make sanitize`: smoke test under ASan + UBSan, in CI; 1,000 oracle patterns under both, 0 reports |

## Index build

| | Linux 6.6 (WSL, 2 vCPU) | Linux 6.6 (c5.xlarge) | Linux + LLVM + PostgreSQL + CPython (c5.xlarge) |
|---|---|---|---|
| files | 81,664 | 81,664 | **274,321** |
| indexed bytes | 1.21 GB | 1.21 GB | **3.57 GB** |
| distinct trigrams | 417,918 | 417,918 | 937,981 |
| postings | 132,232,738 | 132,232,738 | **300,782,058** |
| bytes / posting | 1.07 | 1.07 | 1.08 |
| index size | **288 MB** | 288 MB | **488 MB** |
| build time | **62.5 s** | 13.2 s | 46.2 s |
| peak RSS | 918 MB | 1,685 MB | **1,825 MB** |

The WSL column is the current binary (October 2026, 2 threads). The two
`c5.xlarge` columns are the September binary; the sort, merge and index
format are unchanged since, and the current and September builders produce
byte-identical index files. In September the WSL build took 50.9 s on a less
loaded host; back to back on one host, the September builder took 64.5 s and
the current one 62.5 s.

**The scaling claim:** with the identical binary, indexed source grew ×2.95 and
postings ×2.27 while peak RSS grew **×1.08**. Memory depends on thread count,
not corpus size. That is what the external merge sort is for.

**The memory bound, corrected.** Earlier versions of this file said
`threads × 256 MB + 134 MB`. That left out the radix sort's 256 MB
ping-pong buffer, which each thread allocated while it spilled, so the true
bound was `threads × 512 MB + 134 MB` (the measured 1,825 MB on 4 threads
already exceeded the stated 1,158 MB). The current build shares one scratch
buffer that spills take turns on, so the bound is now
`(threads + 1) × 256 MB + 134 MB` plus per-file bookkeeping: 902 MB on 2
threads before bookkeeping (measured: 918 MB), 1,414 MB on 4.

Index layout: 141 MB delta+varint postings, 12.7 MB inline skip blocks
(8 bytes per 128 postings, 9.0%), 134 MB direct-addressed offset table
(2²⁴ × 8 bytes; only 2.5% of slots are non-empty — see *Not done* below).

## Query performance

Two workloads of 100 queries, measured on 2026-10-16 with the current binary:

- **identifiers:** 100 held-out identifiers mined from the kernel's own
  `scripts/`, `Documentation/` and Coccinelle patches (train/held-out split
  200/100). This is literal search.
- **regexes:** the same 100, each turned into a regex by one of five fixed
  rules in `harness/regex_workload.py`: call site, anchored statement,
  alternation of tails, optional suffixes, identifier-class prefix.

Both tools are measured the same way: 5 runs per query, wall time is the best
of 5 and CPU is the mean of 5, warm page cache. trigrep runs with the index
resident and 2 verification threads (the VM has 2 vCPUs); its CPU is measured
inside the engine's repeat loop, so process startup (fork, 288 MB mmap,
parsing 81,664 paths) is excluded, which is the steady-state cost a resident
service pays. ripgrep 14.1 runs with its default threads and `--no-ignore
--hidden --max-filesize 8M`, so both tools search exactly the same files.

| | identifiers: trigrep | ripgrep | regexes: trigrep | ripgrep |
|---|---|---|---|---|
| p50 latency | **9.8 ms** | 781 ms | **10.8 ms** | 822 ms |
| p95 latency | 803 ms | 1,091 ms | 737 ms | 1,612 ms |
| CPU per query (median) | **0.0135 s** | 1.71 s | **0.0179 s** | 1.92 s |
| candidate files (median / p90) | 46 / 1,127 of 81,664 | all | 52 / 1,127 | all |
| fallback to full scan | 0 / 100 | | 0 / 100 | |
| file sets identical to ripgrep | **100 / 100** | | **100 / 100** | |

Ratios at the median: identifiers **126× less CPU, 80× less wall time**;
regexes **107× less CPU, 76× less wall time.** Both endpoints are printed so
every ratio can be re-derived.

**Against the first published table** (September: p50 10.2 ms, 115× CPU, 100×
wall, identifiers only): that run timed ripgrep once against trigrep's best of
5, let the two tools search different file sets (ripgrep skipped hidden files
and read files over 8 MB), and labelled 2 verification threads as 4. The
numbers above replace it. An earlier rerun the same day, on a host under heavy
load (ripgrep itself 2.3× slower, and the WSL VM failed shortly after), gave
21 to 23 ms and 101× to 104×; it is logged in `metrics.txt` and not used.

The p95 tail is real and explained (figures from the September analysis):
candidates are **size-biased ~8.5× upward**
(mean 126 KB vs a 15 KB corpus mean), because a larger file holds more distinct
trigrams and so satisfies more conjuncts. Common kernel symbols hand the DFA
100+ MB to read. The DFA itself runs at 136–154 MB/s, inside RE2's own band.

## Correctness

**Kernel queries vs `grep -rlE`** — file-set equality on the full 81,664-file index:

| pattern | trigrep | grep | |
|---|---|---|---|
| `kmalloc\(.*GFP_ATOMIC` | 361 | 361 | exact |
| `EXPORT_SYMBOL_GPL\(` | 3,352 | 3,352 | exact |
| `netif_(rx\|receive_skb)` | 427 | 427 | exact |
| `spin_lock_irqsave` | 3,755 | 3,755 | exact |
| `CONFIG_SMP` | 923 | 923 | exact |
| `(foo\|bar)baz` | 5 | 5 | exact |

**Differential oracle vs google-re2**: grammar-generated regexes over a
2,000-file shard, with the engine and the oracle reading exactly the same
files and bytes. Both sides use multi-line anchors, files are compared as raw
UTF-8 (no text-mode newline translation), and soundness is checked exactly:
every file the oracle matches must be in the engine's candidate set.

| generator | binary | patterns | mismatches | unsound | superset check |
|---|---|---|---|---|---|
| v1 (tiers 0, A) | before bug #8 | 10,000 | 1 | 0 | count only |
| v1 | after bug #8 | 10,000 | **0** | **0** | count only |
| **v2** | before bugs #9 to #14 | 10,000 | **568** | **241** | count only |
| **v2** | **fixed** | **10,000** | **0** | **0** | **exact** |

The v1 generator only joined pieces with `.`, so it never put a group directly
next to a literal, and it never produced anchors or escapes. That is why it
passed a build with three bug classes in it. The v2 generator concatenates
terms directly and adds `^ $`, `\d \w \s \D \W \S`, ranges, negated classes,
digits and upper case. Run against the pre-fix binary, the same 10,000 v2
patterns split by pattern feature:

| cause | unsound | wrong answer |
|---|---|---|
| alternation with an unknown branch (bug #9) | 121 | 32 |
| `\D \W \S`, or `\d` inside a class, read as letters (bug #10) | 85 | 140 |
| `^` and `$` ignored by the verifier (bug #11) | 35 | 396 |

v2 composition on the fixed binary: 3,788 of 10,000 patterns (37.9%) degrade
to `ANY` and test only the verifier; 5,036 (50.4%) have a non-empty answer.
Run time: 736 s engine, 765 s oracle, 2 vCPU.

## Ablations (one binary, one flag)

| optimisation | off → on | what it proves |
|---|---|---|
| memmem required-literal prefilter | 1,091 → 607 ms (`CONFIG_SMP`) | 1.1–1.8× on literal-heavy queries; result sets identical |
| parallel verification, 4 threads | 477 → 274 ms | 1.7–1.8×; no gain at 8 threads on 4 cores (memory-bound DFA) |
| skip-block cursor | plan 0.2–0.9 ms either way | makes the skip blocks *load-bearing*; planning was never the bottleneck |
| 4-thread extraction | 82.8 → 50.9 s build | extraction is embarrassingly parallel; merge is not |
| shared work queue + one shared radix scratch | 64.5 → 62.5 s, 892 → 918 MB peak | balanced threads, bound (threads + 1) × 256 MB; index files byte-identical |
| DFA: byte classes, 64 MB cache budget, NFA fallback | `a[ab]{16}c` 3,270 → 836 ms on 960 KB | 2^17 live states no longer thrash a 20,000-state cache |
| DFA, same change | `a[ab]{13}c` 318 → 88 ms; `a[ab]{10}c` 51 → 24 ms | best of 3, old and new binaries back to back on one host |

## Bugs found while building

1. NFA fragment exits stored as `int*` into a growing `std::vector` — dangling on reallocation. Fixed with `(state, field)` index pairs.
2. `is_regular_file()` follows symlinks; `grep -r` and ripgrep do not. One-file over-return (the safe direction). Now skipped.
3. Offset table was `{uint64, uint32}` padded to 16 bytes — 268 MB, 64% of the index. Postings fit in `uint32`; packed to 8 bytes, index 421 → 288 MB.
4. AND/OR trees were not flattened, so concatenation built an O(n²) nested tree.
5. The planner's cost estimate returned "whole corpus" for any nested node, so the selectivity cap silently dropped real conjuncts — `EXPORT_SYMBOL_GPL` returned 13,683 candidates instead of 3,402. A widening bug: never unsound, just slow.
6. The prefilter first used `std::string::find` — libstdc++'s naive O(n·m) — which was *slower* than the DFA it was meant to skip. `memmem` fixed it.
7. A broken `printf` segfaulted mid-output and truncated stdout, making the engine *look* unsound (315 vs 361 files). It never was.
8. **Found by the oracle:** `(gre|(a)+.[mpwz])` missed a file where RE2 matched `'aßm'`. ß is two UTF-8 bytes; RE2 matches `.` against one *code point*, the DFA matched one *byte*. Any class admitting a byte ≥ 0x80 now compiles to the UTF-8 sequence automaton, as RE2 does. Verified against RE2 on 8 probes including negatives (`a..m` → 0/0).

9. **Unsound, found in review:** alternation unioned the branches' prefix and suffix sets, reading an empty set ("unknown") as "no strings". `(abc|x.*)def` then demanded the trigram `bcd` and pruned a file containing `xyzdef` before verification. Fixed by keeping the merged set unknown if any branch is unknown; prefix and suffix sets now keep 2 bytes, which is all a join trigram needs. The v1 oracle could not reach this shape.
10. `\b \B \D \W \S` were read as letters, and so was every escape inside a class (`[\d]` matched `d`). `\D \W \S` and class escapes are now implemented with RE2's ASCII meaning; everything unsupported is a pattern error, exit status 2.
11. `^` and `$` were parsed into nothing, so `^#include` matched an indented `#include`. They are now line anchors in the NFA and the lazy DFA, which tracks start-of-line and resolves a pending `$` on the next `\n`.
12. A DFA cache flush wrote the new transition into a stale state index and kept a stale start state. With the cache shrunk to force flushes, `a[ab]{6}c` found 1 of 20 matching files. States now belong to a cache generation. The same work replaced 1 KB transition rows with byte classes and a 64 MB budget, and falls back to NFA simulation after repeated flushes: `a[ab]{16}c` (2^17 states) went from 3,270 to 836 ms on 960 KB.
13. Nested repeats multiplied the NFA: `(((ab){512}){512}){512}` ran 137 s and died with `bad_alloc`. The NFA is now capped at one million states, which makes it a pattern error.
14. Writes were unchecked, so a full disk produced a truncated index that silently under-returned. Every write is now checked, and a `format` marker written last makes a partial index refuse to open.

15. **Found by UBSan:** posting records start at arbitrary byte offsets, so the skip array inside a record is unaligned, and the cursor read it through a `SkipEntry*`. That is undefined behaviour: harmless on x86, a fault on stricter CPUs. Entries are now copied out with `memcpy`. The smoke test now has a query that seeks a 300-posting list through its skip blocks; the pre-fix code fails it under `make sanitize`.

Harness bugs are logged in `metrics.txt` too (a GET-signed PUT, a two-pipe
`Popen` deadlock, an OOM on a 3.9 GB VM); none touched the engine, every one
cost a rerun. Three more were measurement bugs, fixed in the October rerun:
the oracle read files in text mode (turning CRLF into LF) and only compared
candidate counts; the benchmark timed ripgrep once against trigrep's best of
5, let the two tools search different file sets, and said 4 verification
threads when the script passed none (the engine used its default of 2).

## Not done

- **The offset table is 97.5% empty.** A sorted `(trigram, offset, count)`
  array of 418K × 12 B ≈ 5 MB with binary search would cut the index from
  288 MB to ~155 MB for ~19 comparisons on an L2-resident array. Almost
  certainly a net win; deliberately left as the obvious next change.
- **Positional (line-level) postings** would remove the p95 tail by
  verifying lines instead of whole files.
- **LSM-style segments** for incremental re-indexing.
- Neither benchmark workload has class-only patterns, so neither exercises
  the degrade-to-`ANY` fallback; a fallback rate is therefore not claimed.
  The oracle does exercise it (37.9% of v2 patterns).
- Two planner constants — `kSelectivityCap = 0.25` (drop a conjunct covering
  more than a quarter of the corpus) and `kCursorRatio = 8` (seek through
  skip blocks when a list is 8× longer than the accumulator) — are plausible
  defaults, not tuned values. The train/held-out split exists so they *could*
  be tuned on train alone; that sweep was not run.
