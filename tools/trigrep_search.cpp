#include <cstring>
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

int main(int argc, char** argv) {
    if (argc < 3) fatal("usage: trigrep_search <index-dir> <regex> "
                        "[--stats] [--no-prefilter] [--debug] [--repeat=N] [--threads=N]");
    bool stats = false, use_pre = true, debug = false;
    int repeat = 1;
    unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stats") stats = true;
        else if (a == "--no-prefilter") use_pre = false;            // ablation
        else if (a == "--debug") debug = true;
        else if (a.rfind("--repeat=", 0) == 0)  repeat  = std::stoi(a.substr(9));
        else if (a.rfind("--threads=", 0) == 0) threads = unsigned(std::stoi(a.substr(10)));
    }
    if (threads < 1) threads = 1;

    IndexReader ix;
    if (!ix.open(argv[1])) fatal("cannot open index");

    Arena arena;
    Node* ast = nullptr;
    try { ast = parse_regex(argv[2], arena); }
    catch (const std::exception& e) { fatal(std::string("parse: ") + e.what()); }

    TriQuery q = synthesize(ast);
    Nfa nfa = build_nfa(ast);
    std::vector<std::string> req = use_pre ? required_literals(ast) : std::vector<std::string>{};
    if (debug) {
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
    size_t hits = 0, bytes = 0, prefiltered = 0;
    std::vector<DocId> cand;
    std::vector<char> matched;

    for (int rep = 0; rep < repeat; ++rep) {
        double t0 = now_ms();
        cand.clear();
        eval_query(q, ix, cand);
        double t_plan = now_ms();

        // Parallel verification. Each worker owns its own LazyDfa (the
        // transition cache is mutable), takes candidates by stride, and
        // records into a shared result vector by index -- no locks, and the
        // output order is deterministic.
        matched.assign(cand.size(), 0);
        std::atomic<size_t> a_bytes{0}, a_pre{0}, a_hits{0};
        unsigned nt = std::min<size_t>(threads, std::max<size_t>(1, cand.size()));
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
                a_bytes += lb; a_pre += lp; a_hits += lh;
            });
        }
        for (auto& t : pool) t.join();
        double t_end = now_ms();

        hits = a_hits; bytes = a_bytes; prefiltered = a_pre;
        if (t_end - t0 < best_total) {
            best_total = t_end - t0; best_plan = t_plan - t0; best_verify = t_end - t_plan;
        }
    }
    double cpu_per_query_ms = (cpu_now() - cpu0) * 1000.0 / double(repeat);

    if (!stats)
        for (size_t i = 0; i < cand.size(); ++i)
            if (matched[i]) std::puts(ix.path(cand[i]).c_str());

    std::fprintf(stderr,
        "corpus_files %zu candidates %zu prefiltered %zu hits %zu bytes_scanned %zu "
        "nfa_states %zu threads %u plan_ms %.2f verify_ms %.2f total_ms %.2f cpu_ms %.2f query %s\n",
        ix.num_docs(), cand.size(), prefiltered, hits, bytes,
        nfa.st.size(), unsigned(std::min<size_t>(threads, std::max<size_t>(1, cand.size()))),
        best_plan, best_verify, best_total, cpu_per_query_ms, query_shape(q).c_str());
    return 0;
}
