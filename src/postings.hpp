#pragma once
#include "common.hpp"
#include "varint.hpp"
#include <vector>
#include <string>

static constexpr uint32_t kSkipStride = 128;

// A skip entry MUST carry the byte offset as well as the first docid:
// posting lists are variable-length varints, so the first docid alone tells
// you which block to read but not where it starts.
struct SkipEntry { DocId first; uint32_t byte_off; };

void encode_list(const std::vector<DocId>& docs, std::string& payload,
                 std::vector<SkipEntry>& skips);
void decode_list(const uint8_t* p, uint32_t count, std::vector<DocId>& out);
