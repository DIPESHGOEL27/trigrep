#pragma once
#include "ast.hpp"
#include "common.hpp"
#include <vector>
#include <set>
#include <string>

struct TriQuery {
    enum K { ANY, NONE, AND, OR, TRI } k = ANY;
    Trigram t = 0;
    std::vector<TriQuery> kids;
    static TriQuery any()  { return {}; }
    static TriQuery none() { TriQuery q; q.k = NONE; return q; }
    static TriQuery tri(Trigram x) { TriQuery q; q.k = TRI; q.t = x; return q; }
};

// Bottom-up four-tuple computed by post-order DFS: exact set (or "not exact"),
// prefix set, suffix set, and the trigram query. Every rule is a WEAKENING, so
// by induction the composed query is implied by every string the subexpression
// matches -- which makes the candidate set a provable SUPERSET of the true
// match set. Sets are capped; a node over the cap degrades to ANY, because
// silently truncating a set would be UNSOUND.
struct Info {
    bool exact = false;
    std::set<std::string> ex, pre, suf;
    TriQuery q = TriQuery::any();
};

TriQuery synthesize(const Node* root);

// Literals that EVERY matching string must contain (AND semantics). Used as a
// memmem prefilter before the DFA: memmem runs at GB/s, the DFA at ~145 MB/s,
// and most candidates are false positives. Same trick ripgrep uses. Sound by
// construction: alternation contributes nothing unless all branches agree, and
// anything that can match zero times contributes nothing.
std::vector<std::string> required_literals(const Node* root);
std::string query_shape(const TriQuery& q);   // for debugging / the ablation table
