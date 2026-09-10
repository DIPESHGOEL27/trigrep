# trigrep

Indexed regex search over a source tree. Ask an arbitrary regex over the
1.2 GB Linux kernel tree and get the matching files from an index in about
**10 ms**, instead of scanning the corpus on every query.

Written from scratch in C++17: the index format, the external sort, the regex
engine, and the query planner. Measured, not modelled — see
[RESULTS.md](RESULTS.md).

```
$ ./build/trigrep_search index 'kmalloc\(.*GFP_ATOMIC' --stats
corpus_files 81664 candidates 3542 prefiltered 2769 hits 361 ... plan_ms 0.46 verify_ms 562 total_ms 563
```

## The idea

You cannot index a regex: a regex matches an infinite set of strings. The
resolution (Russ Cox's, from Google Code Search) is not to index the regex.
Index every 3-byte substring of every file, then derive from the regex a
boolean *trigram query* that every matching file must satisfy.

The query is allowed to **over-return** and never allowed to **under-return**.
That asymmetry is the whole design. Each construct of the regex contributes a
weaker condition than it actually imposes, so by induction over the parse tree
the candidate set is a superset of the true match set. Candidates are then
verified exactly by a Thompson NFA with a lazily materialised DFA.

| construct | contributes | why it is sound |
|---|---|---|
| literal ≥ 3 bytes | AND of its trigrams | every match contains them |
| literal < 3 bytes | ANY | carries no trigram |
| concatenation | AND of both sides, plus trigrams straddling the join | the join bytes are also required |
| alternation | OR of the branches | some branch must match |
| `*` `?` `{0,n}` | ANY | zero repetitions must be allowed |
| `+` `{m≥1,n}` | the child's query | at least one repetition |
| class over the size cap | ANY | **degrade, never truncate** — a truncated set is unsound |

The planner may drop a conjunct whose posting list is too large. Dropping
only *widens* the candidate set, so the optimiser may be wrong about cost and
can never be wrong about correctness. The same holds for the `memmem`
prefilter, which skips only files missing a literal every match must contain.

## What is in the box

```
corpus ──walk──> distinct trigrams per file    2^24-bit presence set + dirty list
       ──sort──> (trigram, docid) pairs        radix-sorted 256 MB runs, k-way merged on a loser tree
       ──encode> postings                      delta+varint, inline skip blocks, 2^24 offset table

regex  ──parse─> AST                           recursive descent
       ──DFS───> trigram query                 post-order four-tuple: exact / prefix / suffix / query
       ──plan──> candidates                    cost-ordered AND, galloping intersection, skip-block seeks
       ──verify> matches                       memmem prefilter, Thompson NFA, lazy DFA, N threads
```

Peak memory during indexing is bounded by `threads × 256 MB + 134 MB`, not by
corpus size: the same binary indexes 1.2 GB and 3.6 GB of source within 8% of
the same peak RSS.

## Build and run

Linux (or WSL2). Needs `g++` with C++17 and `make`; nothing else.

```bash
make
./build/count_trigrams  <corpus>            # measure the corpus before indexing it
./build/trigrep_index   <corpus> <index>    # build
./build/trigrep_search  <index> '<regex>'   # search; add --stats for metrics only
```

Useful flags for `trigrep_search`: `--threads=N`, `--repeat=N` (amortise
process startup; a service keeps the index resident), `--no-prefilter`
(ablation), `--debug` (print the synthesised query and required literals),
`--patterns-file=F` (one process, many queries — used by the oracle).

## Regex support

Literals, `.`, character classes, `\d \w \s`, `?` `*` `+` `{m,n}`,
alternation, grouping, anchors. `.` and negated classes match one UTF-8 code
point, as RE2 and ripgrep do. **No backreferences, no lookaround** — neither
is linear-time nor indexable by this technique; that is a design decision,
not a gap.

## Verification

- Six kernel queries against `grep -rlE` on all 81,664 files: file sets identical.
- A grammar-driven differential oracle (`harness/oracle.py`) generates random
  regexes and compares file sets against google-re2 on a 2,000-file shard,
  asserting on every case that the candidate count is at least the oracle's
  match count. It found one real bug (byte-vs-code-point semantics of `.`),
  which is fixed; the counts are in RESULTS.md.
- Every optimisation is behind a flag so it can be turned off and shown to
  leave the result set unchanged.

## Harness

`harness/bench.py` measures latency and CPU per query against an unindexed
ripgrep scan on a held-out workload. `harness/gen_regex.py` + `oracle.py` is
the differential fuzzer. `harness/aws_bootstrap.sh` is the user-data script
for the cross-corpus scaling run. `metrics.txt` is the raw, append-only log of
every measurement, including the ones that went wrong and why.

## Provenance

The trigram-index idea is Russ Cox's ("Regular Expression Matching with a
Trigram Index", 2012) and is cited, not claimed. Everything here is written
from scratch: corpus walker, trigram extractor, radix sort, loser-tree merge,
varint codec, skip blocks, mmap reader, galloping intersection, regex parser,
Thompson NFA, lazy DFA, query synthesis, planner. No third-party C++ library
is linked into the engine. ripgrep is a benchmark baseline and google-re2 is a
test oracle; neither is in the serving path.

## License

MIT.
