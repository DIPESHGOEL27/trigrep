#include "trigram.hpp"

void TrigramSet::collect(const std::string& text, std::vector<Trigram>& out) {
    dirty_.clear();
    if (text.size() < 3) return;
    for (size_t i = 0; i + 3 <= text.size(); ++i) {
        Trigram t = tri_at(text, i);
        uint64_t& w = seen_[t >> 6];
        uint64_t  b = 1ull << (t & 63);
        if (!(w & b)) { w |= b; dirty_.push_back(t); out.push_back(t); }
    }
    for (Trigram t : dirty_) seen_[t >> 6] &= ~(1ull << (t & 63));
}
