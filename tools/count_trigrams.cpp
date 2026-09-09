#include "corpus.hpp"
#include "trigram.hpp"
#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 2) fatal("usage: count_trigrams <corpus-root>");
    double t0 = now_ms();
    auto files = walk_corpus(argv[1]);
    TrigramSet ts;
    std::vector<Trigram> buf;
    std::vector<uint32_t> per_file;
    uint64_t postings = 0, bytes = 0;
    per_file.reserve(files.size());
    for (const auto& f : files) {
        std::string text = slurp(f.path);
        buf.clear();
        ts.collect(text, buf);
        postings += buf.size();
        bytes    += text.size();
        per_file.push_back(uint32_t(buf.size()));
    }
    std::sort(per_file.begin(), per_file.end());
    auto pct = [&](double p) {
        return per_file.empty() ? 0u : per_file[size_t(p * double(per_file.size() - 1))];
    };
    std::printf("files          %zu\n", files.size());
    std::printf("bytes          %llu (%.2f GB)\n", (unsigned long long)bytes, double(bytes) / 1e9);
    std::printf("postings       %llu (%.1fM)\n", (unsigned long long)postings, double(postings) / 1e6);
    std::printf("postings_byte  %.4f\n", double(postings) / double(bytes));
    std::printf("distinct_file  p50=%u p90=%u max=%u\n", pct(0.50), pct(0.90),
                per_file.empty() ? 0u : per_file.back());
    std::printf("wall_s         %.1f\n", (now_ms() - t0) / 1000.0);
    std::printf("peak_rss_mb    %.0f\n", rss_peak_mb());
    return 0;
}
