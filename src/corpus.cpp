#include "corpus.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
namespace fs = std::filesystem;
static constexpr uint64_t kMaxFileBytes = 8ull << 20;

static bool looks_binary(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return true;
    char buf[8192];
    in.read(buf, sizeof buf);
    std::streamsize n = in.gcount();
    for (std::streamsize i = 0; i < n; ++i) if (buf[i] == '\0') return true;
    return false;
}

std::vector<FileEntry> walk_corpus(const std::string& root) {
    std::vector<FileEntry> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_directory(ec)) {
            if (it->path().filename() == ".git") it.disable_recursion_pending();
            continue;
        }
        // Skip symlinks: is_regular_file() follows them, but grep -r and
        // ripgrep do not, and ripgrep is the benchmark baseline. Following
        // them made us over-return (safe, but not comparable).
        if (it->is_symlink(ec)) continue;
        if (!it->is_regular_file(ec)) continue;
        uint64_t sz = it->file_size(ec);
        if (ec) { ec.clear(); continue; }
        if (sz == 0 || sz > kMaxFileBytes) continue;
        std::string p = it->path().string();
        if (looks_binary(p)) continue;
        out.push_back({p, sz});
    }
    std::sort(out.begin(), out.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    return out;
}

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
