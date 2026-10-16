# trigrep

Indexed regex search over a source tree. Ask an arbitrary regex over the
1.2 GB Linux kernel tree and get the matching files from an index in about
**10 ms** (median, index resident, warm cache), instead of scanning the corpus
on every query.

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
| alternation | OR of the branches | some branch must match; a prefix or suffix set is known only if it is known for every branch |
| `^` `$` | ANY (consume nothing) | an anchor adds no bytes to the match |
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
       ──plan──> candidates                    cost-ordered AND, galloping intersection, skip-block seeks,
                                               k-way OR merge on a min-heap
       ──verify> matches                       memmem prefilter, Thompson NFA, lazy DFA over byte classes, N threads
```

Peak memory during indexing is bounded by `(threads + 1) × 256 MB + 134 MB`:
a 256 MB run buffer per thread, one 256 MB radix-sort scratch that spills take
turns on, and the offset table. It depends on thread count, not corpus size:
the indexer indexed 1.2 GB and 3.6 GB of source within 8% of the same peak
RSS. It uses up to 8 threads.

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
`--patterns-file=F` (one process, many queries, used by the oracle),
`--emit-candidates` (print the candidate set, for the oracle's superset check).
Exit status follows grep: 0 if something matched, 1 if nothing did, 2 on a
pattern error.

`make check` runs an end-to-end smoke test on a synthetic corpus with exact
assertions, one per rule: literals, alternation with straddling trigrams,
alternation with an unknown branch, UTF-8 `.`, line anchors, Perl classes,
rejected escapes, the NFA size cap, degrade-to-`ANY`, batch mode and exit
codes. CI runs it on every push.

## Reproduce the numbers

```bash
curl -O https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz
sha256sum -c corpus_sha256.txt && tar xf linux-6.6.tar.xz
./build/count_trigrams linux-6.6                     # 81,664 files, 132.2M postings
./build/trigrep_index  linux-6.6 index               # ~288 MB, prints build time and peak RSS
python3 harness/bench.py harness/heldout.txt linux-6.6 index    # identifiers; needs ripgrep
python3 harness/regex_workload.py harness/heldout.txt > regex.txt
python3 harness/bench.py regex.txt linux-6.6 index              # the same 100, as regexes
pip install google-re2
python3 harness/make_shard.py linux-6.6 shard 2000 7 && ./build/trigrep_index shard shard_index
TRIGREP_BIN=./build/trigrep_search python3 harness/oracle.py shard_index 10000 1 2 2
```

Numbers in [RESULTS.md](RESULTS.md) were taken on WSL2 (2 vCPU) and an AWS
`c5.xlarge`; absolute times will differ on other hardware, ratios should not.

## Regex support

An RE2-compatible subset, with RE2's meaning for every construct it accepts:

- literals (a multi-byte UTF-8 character is one atom), `.`, `\n \t \r \f \v`
  and escaped punctuation;
- classes with ranges and negation, and `\d \w \s \D \W \S` both inside and
  outside classes (ASCII, as in RE2);
- `?` `*` `+` `{m,n}` with counts up to 1000, alternation, `(...)` and `(?:...)`;
- `^` and `$` as line anchors, which is what grep and code search mean by them.

`.`, negated classes and `\D \W \S` match one UTF-8 code point, as RE2 and
ripgrep do.

Everything else is **rejected with an error** rather than guessed at:
`\b \B \A \z`, flags such as `(?i)`, non-ASCII characters inside a class,
and patterns whose NFA would exceed one million states (nested repeats like
`((x{512}){512}){512}`). **No backreferences, no lookaround**: neither is
linear-time nor indexable by this technique; that is a design decision, not a
gap.

## Verification

- **Full corpus vs ripgrep.** Every benchmark query (100 identifiers, 100
  regexes) is diffed against ripgrep over all 81,664 files: 200 of 200 file
  sets identical. Six earlier kernel queries matched `grep -rlE` exactly.
- **Differential oracle vs google-re2** (`harness/oracle.py`): 10,000
  grammar-generated regexes over a 2,000-file shard, with an exact superset
  check on every case (every true match must be in the candidate set). The
  pre-fix binary failed 809 of them, 241 unsound, across three bug classes;
  the fixed binary fails none. The first generator could not produce those
  shapes, which is how they shipped; RESULTS.md has the breakdown.
- **Sanitizers.** `make sanitize` runs the smoke test under ASan and UBSan,
  in CI on every push. 1,000 oracle patterns under both: no reports. UBSan
  found a misaligned read in the skip-block cursor that no other test hit.
- **Ablations.** Every optimisation is behind a flag so it can be turned off
  and shown to leave the result set unchanged.

## Harness

`harness/bench.py` measures latency and CPU per query against an unindexed
ripgrep scan, best of 5 for both tools, and diffs every query's file set
against ripgrep's. `harness/regex_workload.py` turns the identifier workload
into a regex workload by five fixed rules. `harness/gen_regex.py`,
`make_shard.py` and `oracle.py` are the differential fuzzer. `harness/aws_bootstrap.sh` is the user-data script
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
