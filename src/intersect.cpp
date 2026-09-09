#include "intersect.hpp"
#include <algorithm>

// Exponential ("galloping") search: for each element of the SHORT list, jump
// through the long list in doubling strides then binary-search the bracket.
// O(m log(n/m)) instead of O(n+m) when list sizes are wildly unequal, which
// is the common case for trigram postings.
void intersect_gallop(const std::vector<DocId>& a, const std::vector<DocId>& b,
                      std::vector<DocId>& out) {
    const std::vector<DocId>& s = a.size() <= b.size() ? a : b;
    const std::vector<DocId>& l = a.size() <= b.size() ? b : a;
    out.clear();
    if (s.empty() || l.empty()) return;
    size_t lo = 0;
    for (DocId x : s) {
        if (lo >= l.size()) break;
        size_t step = 1;
        while (lo + step < l.size() && l[lo + step] < x) { lo += step; step <<= 1; }
        size_t hi = std::min(lo + step, l.size() - 1);
        size_t p  = size_t(std::lower_bound(l.begin() + long(lo), l.begin() + long(hi) + 1, x)
                           - l.begin());
        if (p < l.size() && l[p] == x) out.push_back(x);
        lo = p;
    }
}

void intersect_cursor(const std::vector<DocId>& small, PostingCursor& c,
                      std::vector<DocId>& out) {
    out.clear();
    for (DocId x : small) {
        c.seek(x);
        if (!c.valid()) break;
        if (c.cur() == x) out.push_back(x);
    }
}
