# trigrep — trigram-indexed regex code search

Answers an arbitrary regex over a source tree from an index, instead of
scanning the corpus on every query.

## The idea

You cannot index a regex — a regex matches an infinite set of strings. The
resolution (Russ Cox's, which this implements) is not to index the regex.
Index every 3-byte substring of every file, then derive from the regex a
boolean trigram query that **every matching file must satisfy**.

The query is allowed to **over-return** and is never allowed to
**under-return**. That asymmetry is the whole design. It is an induction over
the regex parse tree in which every rule is a *weakening*, so the candidate
set is provably a superset of the true match set. Survivors are then verified
exactly by a Thompson NFA with a lazily materialised DFA.

## Pipeline

```
corpus ──walk──> distinct trigrams per file (2^24 bitset + dirty list)
       ──sort──> (trigram, docid) pairs: radix-sorted 256 MB runs, spilled,
                 k-way merged  ── flat RSS, independent of corpus size
       ──encode> delta+varint postings + skip blocks + 2^24 offset table

regex  ──parse─> AST ──post-order DFS──> four-tuple (exact/prefix/suffix/query)
       ──plan──> cost-ordered AND (cheapest conjunct first, galloping intersect)
       ──verify> memmem required-literal prefilter, then lazy DFA
```

## Build and run

```bash
make                                    # g++ -std=c++17 -O2 -march=native
./build/count_trigrams  <corpus>        # the counting pass: measure before modelling
./build/trigrep_index   <corpus> <dir>  # build the index
./build/trigrep_search  <dir> '<regex>' # search
```

Flags: `--stats` (suppress paths, print metrics), `--repeat=N` (amortise
process startup; a real service keeps the index resident), `--no-prefilter`
(ablation), `--debug` (show required literals).

## Supported regex subset

Literals, `.`, character classes, `?` `*` `+` `{m,n}`, alternation, grouping,
anchors, `\d` `\w` `\s`. **No backreferences and no lookaround** — neither is
linear-time nor indexable by this technique. That is a declared design
decision, stated here on day one, not a gap discovered later.

## Soundness

Every synthesis rule only weakens:

| construct | contributes | why |
|---|---|---|
| literal >= 3 bytes | AND of its trigrams | every match contains them |
| literal < 3 bytes | `ANY` | carries no trigram |
| concatenation | AND of children **plus** trigrams straddling the join (left suffixes x right prefixes) | the join bytes are also required |
| alternation | OR of branches | some branch must match |
| `*`, `?`, `{0,n}` | `ANY` | zero repetitions must be allowed |
| `+`, `{m>=1,n}` | the child's query | at least one repetition |
| class over the cap | `ANY` | **degrade, never truncate** — truncating a set is unsound |

The planner may drop a conjunct whose posting list is too large: dropping only
*widens* the candidate set, so an optimiser may be wrong about cost and can
never be wrong about soundness. The same holds for the memmem prefilter, which
only skips files missing a literal that every match must contain.

## Provenance

The trigram-index idea is Russ Cox's (Google Code Search); it is cited, not
claimed. Everything in this repository is written from scratch in C++17: the
corpus walker, trigram extractor, radix sort, k-way merge, varint codec, skip
blocks, mmap reader, galloping intersection, regex parser, Thompson NFA, lazy
DFA, query synthesis and planner. No third-party C++ library is linked into
the engine. `ripgrep` appears only as a benchmark baseline and a reference
regex engine only as a differential oracle in `harness/` — neither is in the
serving path.

Measured numbers live in `metrics.txt`; every one was printed by a binary in
this repository. Bugs found during development are logged there too.
