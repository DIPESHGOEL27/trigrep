#include <cstring>
#include <fstream>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <sys/resource.h>
#include "index.hpp"
#include "corpus.hpp"
#include "regex/parser.hpp"
#include "regex/nfa.hpp"
#include "regex/dfa.hpp"
#include "regex/synth.hpp"
#include "regex/plan.hpp"

static double cpu_now() {
    rusage r{}; getrusage(RUSAGE_SELF, &r);
    return (r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6) +
           (r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6);
}

struct Options {
    bool stats = false, use_pre = true, debug = false, emit_candidates = false;
    int repeat = 1;
    unsigned threads = 1;
};

enum { kMatched = 0, kNoMatch = 1, kError = 2 };      // grep's exit-code convention

// One query against an open index. Prints matched paths (unless --stats) and
// one stats line on stderr. Factored out so --patterns-file can run thousands
// of queries in ONE process: exec'ing per pattern cost ~45 ms of startup each.
static int run_query(const IndexReader& ix, const std::string& pattern, const Options& o) {
    Arena arena;
    Node* ast = nullptr;
    Nfa nfa;
    try {
        ast = parse_regex(pattern, arena);
        nfa = build_nfa(ast);                 // can throw: nested repeats past the state cap
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parse_error pattern=%s err=%s\n", pattern.c_str(), e.what());
        return kError;
    }
    TriQuery q = synthesize(ast);
    std::vector<std::string> req = o.use_pre ? required_literals(ast) : std::vector<std::string>{};
    if (o.debug) {
        std::fprintf(stderr, "required_literals:");
        for (const auto& L : req) std::fprintf(stderr, " [%s]", L.c_str());
        std::fprintf(stderr, "\nquery: %s\n", query_shape(q).c_str());
    }

    // Repeat loop amortises process startup: a real service keeps the index
    // resident. CPU is measured around the loop and divided by `repeat`, so
    // fork/exec, the mmap and docs.txt parsing are excluded -- the steady-
    // state cost a resident service pays, comparable to ripgrep's CPU-seconds.
    double cpu0 = cpu_now();
    double best_total = 1e18, best_plan = 0, best_verify = 0;
    size_t hits = 0, bytes = 0, prefiltered = 0, flushes = 0;
    std::vector<DocId> cand;
    std::vector<char> matched;
    unsigned nt = 1;

    for (int rep = 0; rep < o.repeat; ++rep) {
        double t0 = now_ms();
        cand.clear();
        eval_query(q, ix, cand);
        double t_plan = now_ms();

        // Parallel verification. Each worker owns its own LazyDfa (the
        // transition cache is mutable), takes candidates by stride, and
        // records into a shared result vector by index -- no locks, and the
        // output order is deterministic.
        matched.assign(cand.size(), 0);
        std::atomic<size_t> a_bytes{0}, a_pre{0}, a_hits{0}, a_flush{0};
        nt = unsigned(std::min<size_t>(o.threads, std::max<size_t>(1, cand.size())));
        std::vector<std::thread> pool;
        for (unsigned tid = 0; tid < nt; ++tid) {
            pool.emplace_back([&, tid]() {
                LazyDfa dfa(nfa);
                size_t lb = 0, lp = 0, lh = 0;
                for (size_t i = tid; i < cand.size(); i += nt) {
                    std::string text = slurp(ix.path(cand[i]));
                    lb += text.size();
                    bool skip = false;
                    for (const auto& L : req)
                        if (!memmem(text.data(), text.size(), L.data(), L.size())) { skip = true; break; }
                    if (skip) { ++lp; continue; }        // sound: L is required
                    if (dfa.search(text)) { matched[i] = 1; ++lh; }
                }
                a_bytes += lb; a_pre += lp; a_hits += lh; a_flush += dfa.flushes();
            });
        }
        for (auto& t : pool) t.join();
        double t_end = now_ms();

        hits = a_hits; bytes = a_bytes; prefiltered = a_pre; flushes = a_flush;
        if (t_end - t0 < best_total) {
            best_total = t_end - t0; best_plan = t_plan - t0; best_verify = t_end - t_plan;
        }
    }
    double cpu_per_query_ms = (cpu_now() - cpu0) * 1000.0 / double(o.repeat);

    if (!o.stats)
        for (size_t i = 0; i < cand.size(); ++i)
            if (matched[i]) std::puts(ix.path(cand[i]).c_str());
    // Exact superset check for the oracle: it needs the candidate SET, not
    // just its size, to prove every true match survived planning.
    if (o.emit_candidates)
        for (DocId d : cand) std::printf("@@C %s\n", ix.path(d).c_str());

    std::fprintf(stderr,
        "corpus_files %zu candidates %zu prefiltered %zu hits %zu bytes_scanned %zu "
        "nfa_states %zu dfa_flushes %zu threads %u plan_ms %.2f verify_ms %.2f total_ms %.2f "
        "cpu_ms %.2f query %s\n",
        ix.num_docs(), cand.size(), prefiltered, hits, bytes, nfa.st.size(), flushes, nt,
        best_plan, best_verify, best_total, cpu_per_query_ms, query_shape(q).c_str());
    return hits ? kMatched : kNoMatch;
}

static int parse_positive(const std::string& flag, const std::string& v) {
    try {
        size_t used = 0;
        int n = std::stoi(v, &used);
        if (used == v.size() && n >= 1) return n;
    } catch (const std::exception&) {}
    std::fprintf(stderr, "fatal: %s expects a positive integer, got '%s'\n", flag.c_str(), v.c_str());
    std::exit(kError);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: trigrep_search <index-dir> <regex|--patterns-file=F> [--stats] "
                             "[--no-prefilter] [--debug] [--repeat=N] [--threads=N] [--emit-candidates]\n");
        return kError;
    }
    Options o;
    o.threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::string patterns_file;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stats") o.stats = true;
        else if (a == "--no-prefilter") o.use_pre = false;            // ablation
        else if (a == "--debug") o.debug = true;
        else if (a == "--emit-candidates") o.emit_candidates = true;
        else if (a.rfind("--repeat=", 0) == 0)  o.repeat  = parse_positive("--repeat", a.substr(9));
        else if (a.rfind("--threads=", 0) == 0) o.threads = unsigned(parse_positive("--threads", a.substr(10)));
        else { std::fprintf(stderr, "fatal: unknown option %s\n", a.c_str()); return kError; }
    }
    std::string arg2 = argv[2];
    if (arg2.rfind("--patterns-file=", 0) == 0) patterns_file = arg2.substr(16);

    IndexReader ix;
    if (!ix.open(argv[1])) fatal("cannot open index");

    if (patterns_file.empty()) return run_query(ix, arg2, o);

    // Batch mode: one pattern per line. For each, stdout gets a delimiter
    // line, then the matched paths, and stderr gets the stats line. The
    // index is opened once for the whole file.
    std::ifstream in(patterns_file);
    if (!in) fatal("cannot open patterns file");
    std::string line; size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();    // CRLF files
        if (line.empty()) continue;
        std::printf("@@PATTERN %zu %s\n", n, line.c_str());
        std::fprintf(stderr, "@@PATTERN %zu ", n);
        run_query(ix, line, o);
        std::printf("@@END %zu\n", n);
        std::fflush(stdout);
        ++n;
    }
    return kMatched;
}
