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
| Baseline | ripgrep 14.1.0, 8 threads, `--no-ignore`, warm page cache |
| Oracle | google-re2 (Python bindings), file-set equality |

## Index build

| | Linux 6.6 (WSL, 2 vCPU) | Linux 6.6 (c5.xlarge) | Linux + LLVM + PostgreSQL + CPython (c5.xlarge) |
|---|---|---|---|
| files | 81,664 | 81,664 | **274,321** |
| indexed bytes | 1.21 GB | 1.21 GB | **3.57 GB** |
| distinct trigrams | 417,918 | 417,918 | 937,981 |
| postings | 132,232,738 | 132,232,738 | **300,782,058** |
| bytes / posting | 1.07 | 1.07 | 1.08 |
| index size | **288 MB** | 288 MB | **488 MB** |
| build time | **50.9 s** | 13.2 s | 46.2 s |
| peak RSS | 1,050 MB | 1,685 MB | **1,825 MB** |

**The scaling claim:** with the identical binary, indexed source grew ×2.95 and
postings ×2.27 while peak RSS grew **×1.08**. Memory is bounded by
`threads × 256 MB run buffer + 134 MB offset table` plus per-file bookkeeping —
it is a function of thread count, not corpus size. That is what the external
merge sort is for.

Index layout: 141 MB delta+varint postings, 12.7 MB inline skip blocks
(8 bytes per 128 postings, 9.0%), 134 MB direct-addressed offset table
(2²⁴ × 8 bytes; only 2.5% of slots are non-empty — see *Not done* below).

## Query performance

100 held-out queries mined from the kernel's own `scripts/`, `Documentation/`
and Coccinelle patches (train/held-out split 200/100; thresholds tuned on
train only). Index resident, 4 verification threads, best of 5, warm cache.

| | trigrep | ripgrep 14.1 (8 threads, unindexed scan) |
|---|---|---|
| p50 latency | **10.2 ms** | 1,013 ms |
| p95 latency | 975 ms | — |
| CPU per query (median) | **0.017 s** | 1.96 s |
| candidate files (median) | **46 of 81,664** | — |
| candidate files (p90) | 1,127 | — |
| fallback to full scan | 0 / 100 | — |

Ratios at the median: **115× less CPU, 100× less wall time.** Both endpoints
are printed so the ratio can be re-derived; CPU is measured inside the engine's
repeat loop so process startup (fork, 288 MB mmap, parsing 81,664 paths) is
excluded — that is the steady-state cost a resident service pays.

The p95 tail is real and explained: candidates are **size-biased ~8.5× upward**
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

**Differential oracle vs google-re2** — grammar-generated regexes over a
2,000-file shard, with both the engine and the oracle restricted to the same
files; every case also asserts the candidate count ≥ the oracle's match count.

| tier | patterns | mismatches | unsound | note |
|---|---|---|---|---|
| 0 | 2,000 | 0 | 0 | |
| A | 10,000 | **1 → 0** | 0 | found bug #8, fixed, re-run clean (see below) |
| A (re-run) | 10,000 | **0** | **0** | post-fix; 770 s engine + 175 s oracle |

## Ablations (one binary, one flag)

| optimisation | off → on | what it proves |
|---|---|---|
| memmem required-literal prefilter | 1,091 → 607 ms (`CONFIG_SMP`) | 1.1–1.8× on literal-heavy queries; result sets identical |
| parallel verification, 4 threads | 477 → 274 ms | 1.7–1.8×; no gain at 8 threads on 4 cores (memory-bound DFA) |
| skip-block cursor | plan 0.2–0.9 ms either way | makes the skip blocks *load-bearing*; planning was never the bottleneck |
| 4-thread extraction | 82.8 → 50.9 s build | extraction is embarrassingly parallel; merge is not |

## Bugs found while building

1. NFA fragment exits stored as `int*` into a growing `std::vector` — dangling on reallocation. Fixed with `(state, field)` index pairs.
2. `is_regular_file()` follows symlinks; `grep -r` and ripgrep do not. One-file over-return (the safe direction). Now skipped.
3. Offset table was `{uint64, uint32}` padded to 16 bytes — 268 MB, 64% of the index. Postings fit in `uint32`; packed to 8 bytes, index 421 → 288 MB.
4. AND/OR trees were not flattened, so concatenation built an O(n²) nested tree.
5. The planner's cost estimate returned "whole corpus" for any nested node, so the selectivity cap silently dropped real conjuncts — `EXPORT_SYMBOL_GPL` returned 13,683 candidates instead of 3,402. A widening bug: never unsound, just slow.
6. The prefilter first used `std::string::find` — libstdc++'s naive O(n·m) — which was *slower* than the DFA it was meant to skip. `memmem` fixed it.
7. A broken `printf` segfaulted mid-output and truncated stdout, making the engine *look* unsound (315 vs 361 files). It never was.
8. **Found by the oracle:** `(gre|(a)+.[mpwz])` missed a file where RE2 matched `'aßm'`. ß is two UTF-8 bytes; RE2 matches `.` against one *code point*, the DFA matched one *byte*. Any class admitting a byte ≥ 0x80 now compiles to the UTF-8 sequence automaton, as RE2 does. Verified against RE2 on 8 probes including negatives (`a..m` → 0/0).

Harness bugs are logged in `metrics.txt` too (a GET-signed PUT, a two-pipe
`Popen` deadlock, an OOM on a 3.9 GB VM); none touched the engine, every one
cost a rerun.

## Not done

- **The offset table is 97.5% empty.** A sorted `(trigram, offset, count)`
  array of 418K × 12 B ≈ 5 MB with binary search would cut the index from
  288 MB to ~155 MB for ~19 comparisons on an L2-resident array. Almost
  certainly a net win; deliberately left as the obvious next change.
- **Positional (line-level) postings** would remove the p95 tail by
  verifying lines instead of whole files.
- **LSM-style segments** for incremental re-indexing.
- The workload has no class-only patterns, so it never exercises the
  degrade-to-`ANY` fallback; a fallback rate is therefore not claimed.
