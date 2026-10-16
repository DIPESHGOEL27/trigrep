#include "extsort.hpp"

// LSD radix sort, 8 passes of 8 bits. Non-comparison, O(n) per pass. `tmp` is
// the ping-pong buffer; after the 8th (even) swap both vectors own their
// original storage again, so a shared scratch keeps its capacity.
static void radix_sort_u64(std::vector<Pair>& v, std::vector<Pair>& tmp) {
    tmp.resize(v.size());
    for (int shift = 0; shift < 64; shift += 8) {
        size_t cnt[256] = {0};
        for (Pair p : v) ++cnt[(p >> shift) & 0xFF];
        size_t sum = 0;
        for (int i = 0; i < 256; ++i) { size_t c = cnt[i]; cnt[i] = sum; sum += c; }
        for (Pair p : v) tmp[cnt[(p >> shift) & 0xFF]++] = p;
        v.swap(tmp);
    }
}

void RunWriter::spill() {
    if (scratch_) {
        // One 256 MB scratch for all threads: sorts take turns, writes do
        // not. Peak memory is (threads + 1) x 256 MB instead of threads x
        // 512 MB, which matters because threads pulling from one work queue
        // fill their buffers at the same rate and spill at the same moment.
        std::lock_guard<std::mutex> lock(scratch_->m);
        radix_sort_u64(buf_, scratch_->buf);
    } else {
        std::vector<Pair> tmp;
        radix_sort_u64(buf_, tmp);
    }
    char name[512];
    std::snprintf(name, sizeof name, "%s/run_t%u_%03zu.bin", dir_.c_str(), tag_, runs_.size());
    std::FILE* f = std::fopen(name, "wb");
    if (!f) fatal(std::string("cannot open run file ") + name);
    // A short write (disk full) must stop the build: a truncated run would
    // silently drop postings, and a missing posting is an UNSOUND index.
    size_t wrote = std::fwrite(buf_.data(), sizeof(Pair), buf_.size(), f);
    if (wrote != buf_.size() || std::fclose(f) != 0)
        fatal(std::string("short write to run file (disk full?) ") + name);
    runs_.emplace_back(name);
    buf_.clear();
}

static constexpr size_t kSrcBuf = 1u << 16;
static constexpr Pair   kInf    = ~Pair(0);

// ---- loser tree -----------------------------------------------------------
// k leaves (one per run), k-1 internal nodes each holding the LOSER of the
// match played there; the overall winner sits at tree_[0]. Replacing the
// winner and replaying its root path costs exactly ceil(log2 k) comparisons
// per output element, versus ~2 log2 k for a binary heap (pop + push).

Pair RunMerger::head(size_t i) {
    Src& s = src_[i];
    if (s.pos >= s.len && !refill(i)) return kInf;
    return s.buf[s.pos];
}

RunMerger::RunMerger(const std::vector<std::string>& runs) {
    k_ = runs.size();
    src_.resize(k_);
    for (size_t i = 0; i < k_; ++i) {
        src_[i].f = std::fopen(runs[i].c_str(), "rb");
        if (!src_[i].f) fatal("cannot reopen run");
        src_[i].buf.resize(kSrcBuf);
        refill(i);
    }
    // Build: every internal node starts "empty" (-1); play each leaf up.
    tree_.assign(k_ ? k_ : 1, -1);
    for (size_t i = 0; i < k_; ++i) replay(int(i));
}

RunMerger::~RunMerger() { for (auto& s : src_) if (s.f) std::fclose(s.f); }

bool RunMerger::refill(size_t i) {
    Src& s = src_[i];
    if (s.eof) return false;
    s.len = std::fread(s.buf.data(), sizeof(Pair), kSrcBuf, s.f);
    s.pos = 0;
    if (std::ferror(s.f)) fatal("read error on run file");
    if (s.len == 0) { s.eof = true; return false; }
    return true;
}

// Play leaf `leaf` up the tree. At each internal node the smaller key
// continues upward and the larger is parked as that node's loser.
void RunMerger::replay(int leaf) {
    int cur = leaf;
    Pair cur_key = head(size_t(cur));
    for (size_t node = (size_t(leaf) + k_) / 2; node >= 1; node /= 2) {
        int& slot = tree_[node];
        if (slot < 0) { slot = cur; return; }         // first arrival: park and stop
        Pair slot_key = head(size_t(slot));
        if (slot_key < cur_key) { std::swap(slot, cur); std::swap(slot_key, cur_key); }
        if (node == 1) break;
    }
    tree_[0] = cur;                                     // winner
}

bool RunMerger::next(Pair& out) {
    if (k_ == 0) return false;
    int w = tree_[0];
    if (w < 0) return false;
    Pair v = head(size_t(w));
    if (v == kInf) return false;
    out = v;
    ++src_[size_t(w)].pos;                              // consume the winner
    replay(w);                                          // and replay its path
    return true;
}
