#pragma once
#include <cstdint>
#include <string>

inline void put_uvarint(std::string& out, uint64_t v) {
    while (v >= 0x80) { out.push_back(char(uint8_t(v) | 0x80)); v >>= 7; }
    out.push_back(char(uint8_t(v)));
}
inline uint64_t get_uvarint(const uint8_t* p, size_t& off) {
    uint64_t v = 0; int shift = 0;
    for (;;) {
        uint8_t b = p[off++];
        v |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}
