#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>
#include <sys/resource.h>

using DocId   = uint32_t;
using Trigram = uint32_t;                       // low 24 bits significant
static constexpr uint32_t kTrigramSpace = 1u << 24;

[[noreturn]] inline void fatal(const std::string& m) {
    std::fprintf(stderr, "fatal: %s\n", m.c_str());
    std::exit(1);
}
inline double rss_peak_mb() {                    // ru_maxrss is KB on Linux
    rusage ru{}; getrusage(RUSAGE_SELF, &ru); return ru.ru_maxrss / 1024.0;
}
inline double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
