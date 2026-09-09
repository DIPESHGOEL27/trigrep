#include "plan.hpp"
#include "intersect.hpp"
#include <algorithm>
#include <iterator>
#include <functional>

static void all_docs(const IndexReader& ix, std::vector<DocId>& out) {
    out.resize(ix.num_docs());
    for (size_t i = 0; i < out.size(); ++i) out[i] = DocId(i);
}

void eval_query(const TriQuery& q, const IndexReader& ix, std::vector<DocId>& out) {
    switch (q.k) {
    case TriQuery::ANY:  all_docs(ix, out); return;
    case TriQuery::NONE: out.clear();       return;
    case TriQuery::TRI:  ix.read_list(q.t, out); return;
    case TriQuery::AND: {
        // Estimate must see through nested nodes: AND is bounded by its
        // cheapest child, OR by the sum of its children.
        std::function<size_t(const TriQuery*)> est = [&](const TriQuery* k) -> size_t {
            switch (k->k) {
            case TriQuery::TRI:  return ix.list_count(k->t);
            case TriQuery::NONE: return 0;
            case TriQuery::ANY:  return ix.num_docs();
            case TriQuery::AND: {
                size_t m = ix.num_docs();
                for (const auto& c : k->kids) m = std::min(m, est(&c));
                return m; }
            case TriQuery::OR: {
                size_t t = 0;
                for (const auto& c : k->kids) t += est(&c);
                return std::min(t, ix.num_docs()); }
            }
            return ix.num_docs();
        };
        // Cost-ordered: cheapest conjunct first. Lengths are known up front
        // from the offset table, so ordering costs nothing.
        std::vector<const TriQuery*> ks;
        ks.reserve(q.kids.size());
        for (const auto& k : q.kids) ks.push_back(&k);
        std::sort(ks.begin(), ks.end(),
                  [&](const TriQuery* a, const TriQuery* b) { return est(a) < est(b); });
        bool have = false;
        std::vector<DocId> tmp, nxt;
        for (const TriQuery* k : ks) {
            if (have && double(est(k)) > kSelectivityCap * double(ix.num_docs()))
                continue;                       // widening only -- always sound
            if (!have) { eval_query(*k, ix, out); have = true; }
            else if (k->k == TriQuery::TRI &&
                     double(est(k)) > kCursorRatio * double(out.size())) {
                // Long list vs short accumulator: seek through skip blocks
                // instead of decoding the whole long list.
                PostingCursor c(ix, k->t);
                intersect_cursor(out, c, nxt); out.swap(nxt);
            } else {
                eval_query(*k, ix, tmp);
                intersect_gallop(out, tmp, nxt); out.swap(nxt);
            }
            if (out.empty()) return;
        }
        if (!have) all_docs(ix, out);
        return; }
    case TriQuery::OR: {
        std::vector<DocId> acc, tmp, merged;
        for (const auto& k : q.kids) {
            eval_query(k, ix, tmp);
            merged.clear();
            merged.reserve(acc.size() + tmp.size());
            std::set_union(acc.begin(), acc.end(), tmp.begin(), tmp.end(),
                           std::back_inserter(merged));
            acc.swap(merged);
        }
        out.swap(acc);
        return; }
    }
}
