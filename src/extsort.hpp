#pragma once
#include "common.hpp"
#include <vector>
#include <string>
#include <cstdio>
#include <mutex>

using Pair = uint64_t;                                  // (trigram << 32) | docid
inline Pair    mk_pair(Trigram t, DocId d) { return (Pair(t) << 32) | d; }
inline Trigram pair_tri(Pair p) { return Trigram(p >> 32); }
inline DocId   pair_doc(Pair p) { return DocId(p & 0xFFFFFFFFu); }

static constexpr size_t kRunPairs = 32ull << 20;        // 32M pairs = 256 MB per run

// Radix-sort scratch shared by every RunWriter of one build.
struct SpillScratch {
    std::mutex m;
    std::vector<Pair> buf;
};

class RunWriter {
public:
    // `tag` disambiguates run filenames when several writers (one per
    // extraction thread) share a directory. `scratch`, if given, is the
    // shared sort buffer; otherwise each spill allocates its own.
    explicit RunWriter(std::string dir, unsigned tag = 0, SpillScratch* scratch = nullptr)
        : dir_(std::move(dir)), tag_(tag), scratch_(scratch) { buf_.reserve(kRunPairs); }
    void push(Pair p) { buf_.push_back(p); if (buf_.size() == kRunPairs) spill(); }
    void finish() { if (!buf_.empty()) spill(); }
    const std::vector<std::string>& runs() const { return runs_; }
private:
    void spill();
    std::string dir_;
    unsigned tag_ = 0;
    SpillScratch* scratch_ = nullptr;
    std::vector<Pair> buf_;
    std::vector<std::string> runs_;
};

class RunMerger {
public:
    explicit RunMerger(const std::vector<std::string>& runs);
    ~RunMerger();
    bool next(Pair& out);
private:
    struct Src { std::FILE* f = nullptr; std::vector<Pair> buf; size_t pos = 0, len = 0; bool eof = false; };
    bool refill(size_t i);
    Pair head(size_t i);
    void replay(int leaf);
    std::vector<Src> src_;
    std::vector<int> tree_;     // tree_[0] = winner; tree_[1..k-1] = parked losers
    size_t k_ = 0;
};
