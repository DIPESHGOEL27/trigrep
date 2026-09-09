#include "dfa.hpp"
#include <algorithm>

void LazyDfa::closure(int s, std::vector<int>& out, std::vector<char>& mark, bool& accept) const {
    if (s < 0 || mark[size_t(s)]) return;
    mark[size_t(s)] = 1;
    const NState& st = nfa_.st[size_t(s)];
    if (st.accept) accept = true;
    if (st.split) { closure(st.next, out, mark, accept); closure(st.alt, out, mark, accept); }
    else out.push_back(s);                     // consuming state
}

int LazyDfa::intern(std::vector<int> set, bool accept) {
    std::sort(set.begin(), set.end());
    set.erase(std::unique(set.begin(), set.end()), set.end());
    std::string key(reinterpret_cast<const char*>(set.data()), set.size() * sizeof(int));
    key.push_back(accept ? '1' : '0');
    auto it = lookup_.find(key);
    if (it != lookup_.end()) return it->second;
    if (d_.size() >= kMaxStates) {             // bounded: flush rather than OOM
        d_.clear(); lookup_.clear();
        // start_ is re-interned below on the next call path
    }
    DState ds;
    ds.set = std::move(set);
    ds.accept = accept;
    ds.tr.fill(-2);
    d_.push_back(std::move(ds));
    int id = int(d_.size()) - 1;
    lookup_.emplace(std::move(key), id);
    return id;
}

LazyDfa::LazyDfa(const Nfa& n) : nfa_(n) {
    std::vector<char> mark(nfa_.st.size(), 0);
    closure(nfa_.start, start_set_, mark, start_accept_);
    std::sort(start_set_.begin(), start_set_.end());
    start_ = intern(start_set_, start_accept_);
}

int LazyDfa::step(int from, unsigned char c) {
    std::vector<int> out;
    bool accept = false;
    std::vector<char> mark(nfa_.st.size(), 0);
    for (int s : d_[size_t(from)].set) {
        const NState& st = nfa_.st[size_t(s)];
        if (st.cls.test(c)) closure(st.next, out, mark, accept);
    }
    // Unanchored: union the start closure at every position. This is the
    // implicit ".*?" prefix, and keeps the transition a pure function of
    // (state, byte) so it stays cacheable.
    for (int s : start_set_) if (!mark[size_t(s)]) { mark[size_t(s)] = 1; out.push_back(s); }
    if (start_accept_) accept = true;
    return intern(std::move(out), accept);
}

bool LazyDfa::search(const std::string& text) {
    if (d_.empty()) { start_ = intern(start_set_, start_accept_); }
    int cur = start_;
    if (d_[size_t(cur)].accept) return true;
    for (unsigned char c : text) {
        int nxt = d_[size_t(cur)].tr[c];
        if (nxt == -2) {
            nxt = step(cur, c);
            if (size_t(cur) < d_.size()) d_[size_t(cur)].tr[c] = nxt;
            else { cur = start_ = intern(start_set_, start_accept_); nxt = step(cur, c); }
        }
        cur = nxt;
        if (d_[size_t(cur)].accept) return true;
    }
    return false;
}
