#include "plan.hpp"
#include "intersect.hpp"
#include <algorithm>
#include <iterator>
#include <functional>
#include <queue>
#include <utility>

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
        // k-way merge on a min-heap of (docid, list) heads: one pass over all
        // k lists, O(N log k), duplicates collapsed as they surface. The first
        // version folded the lists in with repeated pairwise unions, which
        // re-copies the accumulator k times.
        std::vector<std::vector<DocId>> lists(q.kids.size());
        for (size_t i = 0; i < q.kids.size(); ++i) eval_query(q.kids[i], ix, lists[i]);
        using Head = std::pair<DocId, size_t>;
        std::priority_queue<Head, std::vector<Head>, std::greater<Head>> heap;
        std::vector<size_t> pos(lists.size(), 0);
        size_t total = 0;
        for (size_t i = 0; i < lists.size(); ++i) {
            total += lists[i].size();
            if (!lists[i].empty()) heap.push({lists[i][0], i});
        }
        out.clear();
        out.reserve(std::min(total, ix.num_docs()));
        while (!heap.empty()) {
            auto [d, i] = heap.top();
            heap.pop();
            if (out.empty() || out.back() != d) out.push_back(d);
            if (++pos[i] < lists[i].size()) heap.push({lists[i][pos[i]], i});
        }
        return; }
    }
}
