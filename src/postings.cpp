#include "postings.hpp"

void encode_list(const std::vector<DocId>& docs, std::string& payload,
                 std::vector<SkipEntry>& skips) {
    payload.clear(); skips.clear();
    DocId prev = 0;
    for (size_t i = 0; i < docs.size(); ++i) {
        if (i % kSkipStride == 0) skips.push_back({docs[i], uint32_t(payload.size())});
        put_uvarint(payload, uint64_t(docs[i] - prev));   // first delta is absolute
        prev = docs[i];
    }
}

void decode_list(const uint8_t* p, uint32_t count, std::vector<DocId>& out) {
    out.clear(); out.reserve(count);
    size_t off = 0; DocId cur = 0;
    for (uint32_t i = 0; i < count; ++i) { cur += DocId(get_uvarint(p, off)); out.push_back(cur); }
}
