#pragma once
#include "synth.hpp"
#include "index.hpp"
#include <vector>

// Conjuncts whose posting list covers more than this fraction of the corpus
// are dropped: decoding them costs more than they prune. Dropping only ever
// WIDENS the candidate set, so the optimiser may be wrong about cost and can
// never be wrong about soundness.
static constexpr double kSelectivityCap = 0.25;

// When a conjunct's list is this many times longer than the accumulator,
// seek it through skip blocks rather than decoding it in full.
static constexpr double kCursorRatio = 8.0;

void eval_query(const TriQuery& q, const IndexReader& ix, std::vector<DocId>& out);
