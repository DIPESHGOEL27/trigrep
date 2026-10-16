#pragma once
#include "nfa.hpp"
#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>

// Subset construction materialised ON DEMAND. Full determinisation can be
// exponential in the pattern, so states are built only as the input asks for
// them and the cache is flushed rather than allowed to grow without bound.
//
// Byte classes: bytes that no NFA state tells apart share one transition
// column, as in RE2. a[ab]{13}c has 5 classes, so a state's row is 20 bytes
// instead of 1 KB, and a 16K-state DFA is a 320 KB table instead of a 16 MB
// one that missed cache and TLB on nearly every byte.
//
// Two bugs in the first version, both fixed here:
//  * A flush invalidated every state index, but the caller kept writing the
//    new transition into the old index and kept a stale start state, so
//    matches were silently lost. Indices now belong to a cache generation and
//    nothing from an older generation is written or reused.
//  * A live DFA larger than the cache flushed over and over. Like RE2, after a
//    few flushes in one search we stop caching and simulate the NFA directly
//    for the rest of that text: still linear, no thrash.
//
// ^ and $ are line anchors. A DFA state records whether its position starts a
// line; a pending $ is resolved when the next byte is '\n' or the text ends.
class LazyDfa {
public:
    explicit LazyDfa(const Nfa& n);
    bool search(const std::string& text);      // unanchored: any position matches?
    size_t states() const { return sets_.size(); }
    size_t flushes() const { return total_flushes_; }
    size_t byte_classes() const { return ncls_; }
private:
    using Set = std::vector<int>;
    void new_pass();
    bool visit(int s);
    void closure(int s, Set& out, bool& accept, bool bol, bool eol_ok);
    void advance(const Set& in, bool bol_in, unsigned char c, Set& out, bool& accept);
    bool accepts_at_end(const Set& in, bool bol);
    int  intern(const Set& set, bool accept, bool bol);
    void flush();
    bool nfa_tail(const std::string& text, size_t from, Set cur, bool bol);

    const Nfa& nfa_;
    std::array<uint16_t, 256> cmap_{};         // byte -> class
    size_t ncls_ = 1;
    // State storage, struct-of-arrays so the hot loop touches only tr_/accept_.
    std::vector<Set>     sets_;                // sorted: consuming states + pending $ asserts
    std::vector<char>    accept_, bol_;
    std::vector<int32_t> tr_;                  // sets_.size() x ncls_, -1 = not computed
    std::unordered_map<std::string, int> lookup_;
    Set  start_bol_, start_mid_;               // start closure at / not at line start
    bool start_bol_acc_ = false, start_mid_acc_ = false;
    std::vector<uint32_t> mark_;               // visit stamps, no per-step allocation
    uint32_t stamp_ = 0;
    Set  scratch_, out_;
    uint64_t gen_ = 0;                         // bumped on every flush
    size_t total_flushes_ = 0;
    size_t cache_bytes_ = 0;
    // Memory budget, not a state count: with byte classes a state costs a
    // few hundred bytes, so 64 MB holds a 2^17-state DFA that a 20,000-state
    // cap used to flush on almost every byte.
    static constexpr size_t kMaxCacheBytes = 64u << 20;
    static constexpr int    kFlushesBeforeNfa = 3;
};
