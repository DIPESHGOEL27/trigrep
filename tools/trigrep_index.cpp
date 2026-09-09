#include "index.hpp"
int main(int argc, char** argv) {
    if (argc < 3) fatal("usage: trigrep_index <corpus-root> <index-dir>");
    IndexWriter::build(argv[1], argv[2]);
    return 0;
}
