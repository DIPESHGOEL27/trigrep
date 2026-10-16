#include "dfa.hpp"
#include <algorithm>
#include <unordered_set>

void LazyDfa::new_pass() {
    if (++stamp_ == 0) { std::fill(mark_.begin(), mark_.end(), 0u); stamp_ = 1; }
}
bool LazyDfa::visit(int s) {
    if (mark_[size_t(s)] == stamp_) return false;
    mark_[size_t(s)] = stamp_;
    return true;
}

// Epsilon closure from s. `bol`: this position starts a line, so ^ holds.
// `eol_ok`: the next byte is '\n' (or the text ended), so $ holds; otherwise a
// $ assert is kept in the set as pending and resolved one byte later.
void LazyDfa::closure(int s, Set& out, bool& accept, bool bol, bool eol_ok) {
    if (s < 0 || !visit(s)) return;
    const NState& st = nfa_.st[size_t(s)];
    if (st.accept) accept = true;
    if (st.assert_bol) { if (bol) closure(st.next, out, accept, bol, eol_ok); return; }
    if (st.assert_eol) {
        if (eol_ok) closure(st.next, out, accept, bol, eol_ok);
        else out.push_back(s);
        return;
    }
    if (st.split) { closure(st.next, out, accept, bol, eol_ok); closure(st.alt, out, accept, bol, eol_ok); return; }
    out.push_back(s);
}

// One NFA step on byte c, including the unanchored restart.
void LazyDfa::advance(const Set& in, bool bol_in, unsigned char c, Set& out, bool& accept) {
    out.clear();
    accept = false;
    const Set* src = &in;
    if (c == '\n') {
        // The byte about to be consumed ends a line: pending $ asserts hold
        // here, at the position BEFORE the '\n'.
        bool pending = false;
        for (int s : in) if (nfa_.st[size_t(s)].assert_eol) { pending = true; break; }
        if (pending) {
            scratch_.clear();
            new_pass();
            for (int s : in) {
                const NState& st = nfa_.st[size_t(s)];
                if (st.assert_eol) closure(st.next, scratch_, accept, bol_in, true);
                else if (visit(s)) scratch_.push_back(s);
            }
            src = &scratch_;
        }
    }
    const bool bol_out = (c == '\n');
    new_pass();
    for (int s : *src) {
        const NState& st = nfa_.st[size_t(s)];
        if (!st.assert_eol && st.cls.test(c)) closure(st.next, out, accept, bol_out, false);
    }
    const Set& st0 = bol_out ? start_bol_ : start_mid_;
    out.insert(out.end(), st0.begin(), st0.end());
    if (bol_out ? start_bol_acc_ : start_mid_acc_) accept = true;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool LazyDfa::accepts_at_end(const Set& in, bool bol) {
    bool accept = false;
    scratch_.clear();
    new_pass();
    for (int s : in) {
        const NState& st = nfa_.st[size_t(s)];
        if (st.assert_eol) closure(st.next, scratch_, accept, bol, true);
    }
    return accept;
}

void LazyDfa::flush() {
    sets_.clear(); accept_.clear(); bol_.clear(); tr_.clear(); lookup_.clear();
    cache_bytes_ = 0;
    ++gen_; ++total_flushes_;
}

int LazyDfa::intern(const Set& set, bool accept, bool bol) {
    std::string key(reinterpret_cast<const char*>(set.data()), set.size() * sizeof(int));
    key.push_back(char((accept ? 1 : 0) | (bol ? 2 : 0)));
    auto it = lookup_.find(key);
    if (it != lookup_.end()) return it->second;
    // Approximate footprint: the set, its transition row, the key, and the
    // hash node and vector headers.
    size_t cost = 2 * key.size() + ncls_ * sizeof(int32_t) + 96;
    if (cache_bytes_ + cost > kMaxCacheBytes && !sets_.empty()) flush();   // bounded: flush rather than OOM
    cache_bytes_ += cost;
    int id = int(sets_.size());
    sets_.push_back(set);
    accept_.push_back(char(accept));
    bol_.push_back(char(bol));
    tr_.resize(tr_.size() + ncls_, -1);
    lookup_.emplace(std::move(key), id);
    return id;
}

LazyDfa::LazyDfa(const Nfa& n) : nfa_(n), mark_(n.st.size(), 0u) {
    // Byte classes by partition refinement: start with one class, then split
    // it by every distinct consuming bitset, and by '\n' (anchors need it).
    std::unordered_set<std::bitset<256>> seen;
    std::array<uint16_t, 256> cls{};
    auto refine = [&](const std::bitset<256>& b) {
        std::unordered_map<uint32_t, uint16_t> remap;
        for (int c = 0; c < 256; ++c) {
            uint32_t k = (uint32_t(cls[size_t(c)]) << 1) | uint32_t(b.test(size_t(c)));
            auto it = remap.emplace(k, uint16_t(remap.size())).first;
            cls[size_t(c)] = it->second;
        }
    };
    std::bitset<256> nl; nl.set('\n');
    refine(nl);
    for (const NState& st : nfa_.st)
        if (!st.split && !st.assert_bol && !st.assert_eol && !st.accept && seen.insert(st.cls).second)
            refine(st.cls);
    cmap_ = cls;
    ncls_ = size_t(*std::max_element(cls.begin(), cls.end())) + 1;

    new_pass(); closure(nfa_.start, start_bol_, start_bol_acc_, true,  false);
    new_pass(); closure(nfa_.start, start_mid_, start_mid_acc_, false, false);
    std::sort(start_bol_.begin(), start_bol_.end());
    std::sort(start_mid_.begin(), start_mid_.end());
}

bool LazyDfa::nfa_tail(const std::string& text, size_t from, Set cur, bool bol) {
    Set nxt;
    bool accept = false;
    for (size_t i = from; i < text.size(); ++i) {
        unsigned char c = (unsigned char)text[i];
        advance(cur, bol, c, nxt, accept);
        if (accept) return true;
        cur.swap(nxt);
        bol = (c == '\n');
    }
    return accepts_at_end(cur, bol);
}

bool LazyDfa::search(const std::string& text) {
    int cur = intern(start_bol_, start_bol_acc_, true);
    if (accept_[size_t(cur)]) return true;
    int flushes = 0;
    const size_t n = text.size();
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        size_t slot = size_t(cur) * ncls_ + cmap_[c];
        int nxt = tr_[slot];
        if (nxt < 0) {
            bool accept;
            advance(sets_[size_t(cur)], bol_[size_t(cur)] != 0, c, out_, accept);
            uint64_t g = gen_;
            nxt = intern(out_, accept, c == '\n');
            if (gen_ == g) {
                tr_[slot] = nxt;                // cur is still a live index
            } else if (++flushes >= kFlushesBeforeNfa) {
                if (accept_[size_t(nxt)]) return true;
                return nfa_tail(text, i + 1, sets_[size_t(nxt)], c == '\n');
            }
        }
        cur = nxt;
        if (accept_[size_t(cur)]) return true;
    }
    return accepts_at_end(sets_[size_t(cur)], bol_[size_t(cur)] != 0);
}
