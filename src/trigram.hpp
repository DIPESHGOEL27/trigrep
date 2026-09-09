#pragma once
#include "common.hpp"
#include <vector>
#include <string>

// The 2^24 presence bitset is allocated once and cleared through a dirty
// list, so per-file cost is O(distinct trigrams), never O(2^24).
class TrigramSet {
public:
    TrigramSet() : seen_(kTrigramSpace / 64, 0) {}
    void collect(const std::string& text, std::vector<Trigram>& out);
private:
    std::vector<uint64_t> seen_;
    std::vector<Trigram>  dirty_;
};

inline Trigram tri_at(const std::string& s, size_t i) {
    return (Trigram(uint8_t(s[i]))     << 16)
         | (Trigram(uint8_t(s[i + 1])) <<  8)
         |  Trigram(uint8_t(s[i + 2]));
}
