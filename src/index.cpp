#include "index.hpp"
#include "corpus.hpp"
#include "trigram.hpp"
#include "extsort.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <thread>
#include <memory>
#include <atomic>

// Written last by the builder, checked first by the reader. A crashed or
// partial build therefore has no marker and is refused instead of mmapped.
static const char* kFormatTag = "trigrep-index 1";

static const void* map_file(const std::string& p, size_t& len) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) fatal("open " + p);
    struct stat st{};
    if (fstat(fd, &st) != 0) fatal("fstat " + p);
    len = size_t(st.st_size);
    if (len == 0) { ::close(fd); return nullptr; }
    void* m = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) fatal("mmap " + p);
    return m;
}

void IndexWriter::build(const std::string& root, const std::string& out) {
    double t0 = now_ms();
    ::mkdir(out.c_str(), 0755);
    std::string runs_dir = out + "/runs";
    ::mkdir(runs_dir.c_str(), 0755);

    auto files = walk_corpus(root);
    ::unlink((out + "/format").c_str());          // invalid until the build completes
    for (const auto& f : files)
        if (f.path.find('\n') != std::string::npos) fatal("path contains a newline: docs.txt is line-based");
    { std::ofstream d(out + "/docs.txt");
      for (const auto& f : files) d << f.path << '\n';
      d.close();
      if (!d) fatal("write failed: docs.txt"); }

    // Extraction is embarrassingly parallel per file. Each thread owns a
    // TrigramSet (the 2 MB bitset) and a RunWriter, and pulls the next file
    // index from one shared atomic counter, so a directory of huge files
    // cannot stall a single thread the way a fixed contiguous range did. The
    // merge stage takes the union of every thread's spilled runs: the k-way
    // merge does not care which thread produced a run.
    //
    // Memory: each thread holds a 256 MB run buffer, and radix sort needs a
    // second 256 MB scratch buffer while it spills, so the bound is
    // threads x 512 MB + the 134 MB offset table + per-file bookkeeping.
    unsigned nthreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::unique_ptr<RunWriter>> writers;
    for (unsigned t = 0; t < nthreads; ++t)
        writers.emplace_back(std::make_unique<RunWriter>(runs_dir, t));
    {
        std::vector<std::thread> pool;
        std::atomic<size_t> next_file{0};
        for (unsigned t = 0; t < nthreads; ++t) {
            pool.emplace_back([&, t]() {
                TrigramSet ts; std::vector<Trigram> buf;
                for (size_t i; (i = next_file.fetch_add(1)) < files.size(); ) {
                    std::string text = slurp(files[i].path);
                    buf.clear();
                    ts.collect(text, buf);
                    for (Trigram tr : buf) writers[t]->push(mk_pair(tr, DocId(i)));
                }
                writers[t]->finish();
            });
        }
        for (auto& th : pool) th.join();
    }
    std::vector<std::string> all_runs;
    for (auto& w : writers) for (const auto& r : w->runs()) all_runs.push_back(r);
    double t_extract = now_ms();

    std::vector<OffRec> off(kTrigramSpace, OffRec{0, 0});
    std::ofstream fp(out + "/postings.bin", std::ios::binary);
    uint64_t pos = 0, payload_bytes = 0, skip_bytes = 0, total_postings = 0, lists = 0;
    {
        RunMerger rm(all_runs);
        Pair p;
        std::vector<DocId> cur;
        Trigram curt = 0; bool have = false;
        std::string payload; std::vector<SkipEntry> skips;
        auto flush = [&]() {
            if (cur.empty()) return;
            encode_list(cur, payload, skips);
            if (pos > 0xFFFFFFFFull) fatal("postings exceed 4 GB; widen OffRec::byte_off");
            off[curt] = {uint32_t(pos), uint32_t(cur.size())};
            uint32_t ns = uint32_t(skips.size());
            fp.write(reinterpret_cast<const char*>(&ns), sizeof ns);
            fp.write(reinterpret_cast<const char*>(skips.data()), long(ns * sizeof(SkipEntry)));
            fp.write(payload.data(), long(payload.size()));
            size_t rec = sizeof ns + ns * sizeof(SkipEntry) + payload.size();
            pos += rec; payload_bytes += payload.size();
            skip_bytes += sizeof ns + ns * sizeof(SkipEntry);
            total_postings += cur.size(); ++lists;
            cur.clear();
        };
        while (rm.next(p)) {
            Trigram t = pair_tri(p);
            if (!have) { curt = t; have = true; }
            else if (t != curt) { flush(); curt = t; }
            DocId d = pair_doc(p);
            if (cur.empty() || cur.back() != d) cur.push_back(d);   // dedupe
        }
        flush();
    }
    fp.close();
    if (!fp) fatal("write failed: postings.bin (disk full?)");
    { std::ofstream fo(out + "/offsets.bin", std::ios::binary);
      fo.write(reinterpret_cast<const char*>(off.data()), long(off.size() * sizeof(OffRec)));
      fo.close();
      if (!fo) fatal("write failed: offsets.bin (disk full?)"); }
    { std::ofstream ft(out + "/format");
      ft << kFormatTag << '\n';
      ft.close();
      if (!ft) fatal("write failed: format"); }

    for (const auto& r : all_runs) ::unlink(r.c_str());
    ::rmdir(runs_dir.c_str());

    uint64_t off_bytes = uint64_t(sizeof(OffRec)) * kTrigramSpace;
    std::printf("files             %zu\n", files.size());
    std::printf("distinct_trigrams %llu\n", (unsigned long long)lists);
    std::printf("postings          %llu\n", (unsigned long long)total_postings);
    std::printf("payload_bytes     %llu\n", (unsigned long long)payload_bytes);
    std::printf("skip_bytes        %llu (inline)\n", (unsigned long long)skip_bytes);
    std::printf("offsets_bytes     %llu\n", (unsigned long long)off_bytes);
    std::printf("index_bytes       %llu (%.0f MB)\n",
                (unsigned long long)(pos + off_bytes), double(pos + off_bytes) / 1e6);
    std::printf("bytes_per_posting %.2f\n",
                total_postings ? double(payload_bytes) / double(total_postings) : 0.0);
    std::printf("skip_overhead_pct %.1f\n",
                payload_bytes ? 100.0 * double(skip_bytes) / double(payload_bytes) : 0.0);
    std::printf("runs              %zu (from %u threads)\n", all_runs.size(), nthreads);
    std::printf("extract_s         %.1f\n", (t_extract - t0) / 1000.0);
    std::printf("build_s           %.1f\n", (now_ms() - t0) / 1000.0);
    std::printf("peak_rss_mb       %.0f\n", rss_peak_mb());
}

bool IndexReader::open(const std::string& dir) {
    { std::ifstream ft(dir + "/format");
      std::string tag;
      if (!std::getline(ft, tag) || tag != kFormatTag)
          fatal("not a complete trigrep index (missing or wrong format marker): " + dir); }
    size_t olen = 0;
    off_  = static_cast<const OffRec*>(map_file(dir + "/offsets.bin", olen));
    if (olen != sizeof(OffRec) * size_t(kTrigramSpace)) fatal("offsets.bin has the wrong size: " + dir);
    post_ = static_cast<const uint8_t*>(map_file(dir + "/postings.bin", post_len_));
    std::ifstream d(dir + "/docs.txt");
    std::string line;
    while (std::getline(d, line)) docs_.push_back(line);
    return off_ && post_ && !docs_.empty();
}

void IndexReader::read_list(Trigram t, std::vector<DocId>& out) const {
    const OffRec& r = off_[t];
    if (!r.count) { out.clear(); return; }
    const uint8_t* rec = post_ + r.byte_off;
    uint32_t ns; std::memcpy(&ns, rec, sizeof ns);
    decode_list(rec + sizeof ns + ns * sizeof(SkipEntry), r.count, out);
}

// ---- PostingCursor ---------------------------------------------------------

PostingCursor::PostingCursor(const IndexReader& ix, Trigram t) {
    count_ = ix.list_count(t);
    if (!count_) return;
    const uint8_t* rec = ix.record(t);
    std::memcpy(&nskips_, rec, sizeof nskips_);
    skips_   = reinterpret_cast<const SkipEntry*>(rec + sizeof nskips_);
    payload_ = rec + sizeof nskips_ + nskips_ * sizeof(SkipEntry);
    jump_to_block(0);
}

void PostingCursor::jump_to_block(uint32_t b) {
    block_ = b;
    off_   = skips_[b].byte_off;
    idx_   = b * kSkipStride;
    get_uvarint(payload_, off_);            // consume the block's first delta...
    cur_   = skips_[b].first;               // ...its absolute value is in the skip entry
}

void PostingCursor::seek(DocId target) {
    if (!valid() || cur_ >= target) return;
    // Block holding the first docid >= target: last skip with first <= target.
    uint32_t lo = block_, hi = nskips_;      // never move backwards
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (skips_[mid].first <= target) lo = mid; else hi = mid;
    }
    if (lo != block_) jump_to_block(lo);
    if (cur_ >= target) return;
    // Linear decode inside the block: at most kSkipStride steps.
    while (idx_ + 1 < count_ && cur_ < target) {
        cur_ += DocId(get_uvarint(payload_, off_));
        ++idx_;
    }
    if (cur_ < target) idx_ = count_;       // exhausted
}
