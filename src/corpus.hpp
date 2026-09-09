#pragma once
#include "common.hpp"
#include <vector>
#include <string>

struct FileEntry { std::string path; uint64_t size; };

// Skips .git, files > 8 MB, and files with a NUL in the first 8 KB (binary).
// Returns path-sorted entries so DocId assignment is deterministic.
std::vector<FileEntry> walk_corpus(const std::string& root);
std::string slurp(const std::string& path);
