#pragma once
#include "common.hpp"
#include "postings.hpp"
#include <vector>
#include <string>

// 8 bytes, not 16: a uint64 offset padded to 16 made this table 268 MB,
// 64% of the whole index. Postings fit comfortably in uint32.
struct OffRec { uint32_t byte_off; uint32_t count; };   // 2^24 of these
static_assert(sizeof(OffRec) == 8, "OffRec must stay 8 bytes");

// On-disk record for one posting list, at postings.bin + OffRec::byte_off:
//   [uint32 nskips][SkipEntry x nskips][delta+varint payload]
// Skip entries live INLINE before their payload so a cursor can locate
// them from the offset table alone, with no second lookup table.

class IndexWriter {
public:
    static void build(const std::string& corpus_root, const std::string& out_dir);
};

class IndexReader {
public:
    bool open(const std::string& dir);
    uint32_t list_count(Trigram t) const { return off_[t].count; }
    void read_list(Trigram t, std::vector<DocId>& out) const;
    const std::string& path(DocId d) const { return docs_[d]; }
    size_t num_docs() const { return docs_.size(); }
    size_t index_bytes() const { return post_len_ + sizeof(OffRec) * size_t(kTrigramSpace); }
    const uint8_t* record(Trigram t) const { return post_ + off_[t].byte_off; }
private:
    const OffRec*  off_  = nullptr;
    const uint8_t* post_ = nullptr; size_t post_len_ = 0;
    std::vector<std::string> docs_;
};

// Forward-only cursor over one posting list that USES the skip blocks:
// seek(target) binary-searches the skip array by first-docid, jumps straight
// to that block's byte offset, and decodes forward from there. A short list
// intersected against a long one touches O(|short| * 128) postings instead
// of decoding the whole long list.
class PostingCursor {
public:
    PostingCursor(const IndexReader& ix, Trigram t);
    bool  valid() const { return idx_ < count_; }
    DocId cur()   const { return cur_; }
    void  seek(DocId target);          // advance to first docid >= target
private:
    void jump_to_block(uint32_t b);
    const uint8_t*   payload_ = nullptr;
    const SkipEntry* skips_   = nullptr;
    uint32_t nskips_ = 0, count_ = 0, idx_ = 0, block_ = 0;
    size_t   off_ = 0;
    DocId    cur_ = 0;
};
