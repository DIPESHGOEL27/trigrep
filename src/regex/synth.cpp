#include "synth.hpp"
#include "trigram.hpp"
#include <algorithm>

static constexpr size_t kSetCap = 64;      // degrade-to-ANY threshold

// Flatten AND(AND(a,b),c) into AND(a,b,c) and dedupe trigrams. Without this
// the concat rule builds an O(n^2) nested tree, and the planner's cost
// estimate cannot see through a nested node -- it treats it as full-corpus
// and drops it at the selectivity cap, silently widening the candidate set.
static TriQuery and_of(std::vector<TriQuery> v) {
    std::vector<TriQuery> flat;
    std::vector<Trigram> tris;
    for (auto& x : v) {
        if (x.k == TriQuery::ANY) continue;
        if (x.k == TriQuery::NONE) return TriQuery::none();
        if (x.k == TriQuery::TRI) { tris.push_back(x.t); continue; }
        if (x.k == TriQuery::AND) {
            for (auto& c : x.kids) {
                if (c.k == TriQuery::TRI) tris.push_back(c.t);
                else flat.push_back(std::move(c));
            }
            continue;
        }
        flat.push_back(std::move(x));
    }
    std::sort(tris.begin(), tris.end());
    tris.erase(std::unique(tris.begin(), tris.end()), tris.end());
    for (Trigram t : tris) flat.push_back(TriQuery::tri(t));
    if (flat.empty()) return TriQuery::any();
    if (flat.size() == 1) return flat[0];
    TriQuery q; q.k = TriQuery::AND; q.kids = std::move(flat); return q;
}
static TriQuery or_of(std::vector<TriQuery> v) {
    for (const auto& x : v) if (x.k == TriQuery::ANY) return TriQuery::any();  // OR with ANY is ANY
    std::vector<TriQuery> flat;
    for (auto& x : v) {
        if (x.k == TriQuery::NONE) continue;
        if (x.k == TriQuery::OR) { for (auto& c : x.kids) flat.push_back(std::move(c)); continue; }
        flat.push_back(std::move(x));
    }
    if (flat.empty()) return TriQuery::none();
    if (flat.size() == 1) return flat[0];
    TriQuery q; q.k = TriQuery::OR; q.kids = std::move(flat); return q;
}

// AND of every trigram in s. Strings shorter than 3 bytes contribute ANY.
static TriQuery tris_of(const std::string& s) {
    if (s.size() < 3) return TriQuery::any();
    std::vector<TriQuery> v;
    for (size_t i = 0; i + 3 <= s.size(); ++i) v.push_back(TriQuery::tri(tri_at(s, i)));
    return and_of(std::move(v));
}
static TriQuery or_tris_of_set(const std::set<std::string>& S) {
    if (S.empty()) return TriQuery::any();
    std::vector<TriQuery> v;
    for (const auto& s : S) v.push_back(tris_of(s));
    return or_of(std::move(v));
}

// ---- the four-tuple -------------------------------------------------------
// Invariants (every one is what makes the result a superset):
//   ex   when `exact`: the complete, finite set of strings the node matches.
//   pre  strings of which EVERY match starts with one (kept to <= 2 bytes);
//   suf  strings of which EVERY match ends with one (kept to <= 2 bytes).
//   An EMPTY pre/suf set means UNKNOWN, never "no strings". The original
//   alternation rule unioned sets and so read "unknown" as "nothing", which
//   made (abc|x.*)def demand the trigram "bcd" and silently drop true matches.
// Two bytes are all a join needs: a trigram straddling a concatenation takes
// at most 2 bytes from either side; trigrams inside a side are already in
// that side's query.
static constexpr size_t kExactMaxLen = 32;   // longer exact strings are demoted

static std::string first2(const std::string& s) { return s.substr(0, std::min<size_t>(2, s.size())); }
static std::string last2(const std::string& s)  { return s.substr(s.size() - std::min<size_t>(2, s.size())); }

// out = { f(a + b) : a in A, b in B }. False if either side is unknown or the
// result would exceed the cap; the caller then treats the result as unknown.
template <class F>
static bool product(const std::set<std::string>& A, const std::set<std::string>& B,
                    F f, std::set<std::string>& out) {
    out.clear();
    if (A.empty() || B.empty()) return false;
    for (const auto& a : A) for (const auto& b : B) {
        out.insert(f(a + b));
        if (out.size() > kSetCap) { out.clear(); return false; }
    }
    return true;
}
static std::string ident(const std::string& s) { return s; }

static Info info_any() { Info i; i.exact = false; i.q = TriQuery::any(); return i; }

static Info info_exact(std::set<std::string> ex, TriQuery q) {
    Info i; i.q = std::move(q);
    size_t longest = 0;
    for (const auto& s : ex) longest = std::max(longest, s.size());
    for (const auto& s : ex) { i.pre.insert(first2(s)); i.suf.insert(last2(s)); }
    if (ex.size() <= kSetCap && longest <= kExactMaxLen) { i.exact = true; i.ex = std::move(ex); }
    if (i.pre.size() > kSetCap) i.pre.clear();
    if (i.suf.size() > kSetCap) i.suf.clear();
    return i;
}

static Info concat2(const Info& a, const Info& b) {
    std::vector<TriQuery> qs{a.q, b.q};
    std::set<std::string> joins;               // last2(suffix) + first2(prefix)
    if (product(a.suf, b.pre, ident, joins)) qs.push_back(or_tris_of_set(joins));
    TriQuery q = and_of(std::move(qs));

    std::set<std::string> ex;
    if (a.exact && b.exact && product(a.ex, b.ex, ident, ex)) return info_exact(std::move(ex), std::move(q));

    Info r; r.exact = false; r.q = std::move(q);
    if (!a.exact) r.pre = a.pre;
    else if (!product(a.ex, b.pre, first2, r.pre)) for (const auto& x : a.ex) r.pre.insert(first2(x));
    if (!b.exact) r.suf = b.suf;
    else if (!product(a.suf, b.ex, last2, r.suf)) for (const auto& y : b.ex) r.suf.insert(last2(y));
    if (r.pre.size() > kSetCap) r.pre.clear();
    if (r.suf.size() > kSetCap) r.suf.clear();
    return r;
}

static Info walk(const Node* n) {
    switch (n->op) {
    case Op::Empty:
    case Op::Bol:                                // anchors consume nothing
    case Op::Eol:
        return info_exact({""}, TriQuery::any());
    case Op::Literal:
        return info_exact({n->lit}, tris_of(n->lit));
    case Op::Class: {
        if (n->cls.count() > kSetCap) return info_any();     // '.' and big classes -> ANY
        std::set<std::string> ex;
        for (int c = 0; c < 256; ++c) if (n->cls.test(size_t(c))) ex.insert(std::string(1, char(c)));
        return info_exact(std::move(ex), TriQuery::any()); } // single bytes carry no trigram
    case Op::Concat: {
        Info acc = walk(n->kids[0]);
        for (size_t k = 1; k < n->kids.size(); ++k) acc = concat2(acc, walk(n->kids[k]));
        return acc; }
    case Op::Alt: {
        Info r; r.exact = true;
        bool pre_known = true, suf_known = true;
        std::vector<TriQuery> qs;
        for (Node* k : n->kids) {
            Info b = walk(k);
            qs.push_back(b.q);
            r.exact = r.exact && b.exact;
            if (b.exact) r.ex.insert(b.ex.begin(), b.ex.end());
            pre_known = pre_known && !b.pre.empty();
            suf_known = suf_known && !b.suf.empty();
            r.pre.insert(b.pre.begin(), b.pre.end());
            r.suf.insert(b.suf.begin(), b.suf.end());
        }
        r.q = or_of(std::move(qs));
        if (!r.exact || r.ex.size() > kSetCap) { r.exact = false; r.ex.clear(); }
        if (!pre_known || r.pre.size() > kSetCap) r.pre.clear();     // one unknown branch
        if (!suf_known || r.suf.size() > kSetCap) r.suf.clear();     // makes the whole set unknown
        return r; }
    case Op::Star:
    case Op::Quest:
        return info_any();                       // zero repetitions must be allowed
    case Op::Plus: {
        Info b = walk(n->kids[0]);               // x+ implies at least one x
        Info r; r.exact = false; r.pre = b.pre; r.suf = b.suf; r.q = b.q; return r; }
    case Op::Repeat: {
        if (n->rmin == 0) return info_any();     // {0,k} allows zero -> ANY
        Info b = walk(n->kids[0]);
        Info r; r.exact = false; r.pre = b.pre; r.suf = b.suf; r.q = b.q; return r; }
    }
    return info_any();
}

TriQuery synthesize(const Node* root) { return walk(root).q; }

std::string query_shape(const TriQuery& q) {
    switch (q.k) {
    case TriQuery::ANY:  return "ANY";
    case TriQuery::NONE: return "NONE";
    case TriQuery::TRI:  {
        char b[8]; std::snprintf(b, sizeof b, "'%c%c%c'",
            char((q.t >> 16) & 0xFF), char((q.t >> 8) & 0xFF), char(q.t & 0xFF));
        return b; }
    case TriQuery::AND:
    case TriQuery::OR: {
        std::string s = (q.k == TriQuery::AND ? "AND(" : "OR(");
        for (size_t i = 0; i < q.kids.size(); ++i) {
            if (i) s += ",";
            if (i >= 4) { s += "..."; break; }
            s += query_shape(q.kids[i]);
        }
        return s + ")"; }
    }
    return "?";
}

// --- required literals (memmem prefilter) --------------------------------
static void req_walk(const Node* n, std::vector<std::string>& out) {
    switch (n->op) {
    case Op::Literal: out.push_back(n->lit); return;
    case Op::Concat: {
        // Adjacent literal children fuse: "ab" "cd" -> "abcd", which is a
        // longer and therefore more selective required substring.
        std::string run;
        for (Node* k : n->kids) {
            if (k->op == Op::Literal) { run += k->lit; continue; }
            if (!run.empty()) { out.push_back(run); run.clear(); }
            req_walk(k, out);
        }
        if (!run.empty()) out.push_back(run);
        return; }
    case Op::Plus: req_walk(n->kids[0], out); return;
    case Op::Repeat: if (n->rmin >= 1) req_walk(n->kids[0], out); return;
    case Op::Alt:    return;   // branches may differ -- contribute nothing
    case Op::Star: case Op::Quest: case Op::Class: case Op::Empty:
    case Op::Bol: case Op::Eol: return;
    }
}

std::vector<std::string> required_literals(const Node* root) {
    std::vector<std::string> all, keep;
    req_walk(root, all);
    for (auto& s : all) if (s.size() >= 3) keep.push_back(s);
    std::sort(keep.begin(), keep.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    if (keep.size() > 4) keep.resize(4);      // 4 memmem passes is plenty
    return keep;
}
