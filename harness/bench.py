"""Latency + CPU benchmark vs an unindexed ripgrep scan, with a correctness diff.

Both tools are measured the same way: N runs per query, wall time is the best
of N and CPU is the mean of N. (The first version took trigrep's best of 5
against a single ripgrep run.) trigrep's numbers come from inside the engine's
--repeat loop, so process startup and the index mmap are excluded: that is the
steady-state cost of a resident service. ripgrep has no index to load, and its
numbers are whole-process.

Every query's file set is also diffed against ripgrep's, so the benchmark
doubles as a full-corpus correctness check. ripgrep runs with --no-ignore
--hidden --max-filesize 8M so it searches exactly the files the indexer walks
(the indexer skips files over 8 MB, such as 24 MB AMD register headers).

usage: bench.py <queries.txt> <corpus> <index> [--reps=5] [--threads=2]
"""
import os
import re
import resource
import statistics
import subprocess
import sys
import time


def run(cmd):
    b = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True)
    wall = (time.perf_counter() - t0) * 1000.0
    a = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (a.ru_utime - b.ru_utime) + (a.ru_stime - b.ru_stime)
    return wall, cpu, p.stdout.decode(errors="surrogateescape"), p.stderr.decode(errors="ignore")


def pct(v, q):
    v = sorted(v)
    return v[min(int(q / 100.0 * len(v)), len(v) - 1)] if v else 0.0


def files(out):
    return {os.path.normpath(l) for l in out.splitlines() if l}


def main(qfile, corpus, index, reps, threads):
    pats = [l.rstrip("\n") for l in open(qfile) if l.strip()]
    mw, mc, rw, rc, cands, fallbacks, diffs = [], [], [], [], [], 0, 0
    corpus_files = 0
    for pat in pats:
        _, _, out, err = run(["./build/trigrep_search", index, pat, f"--repeat={reps}",
                              f"--threads={threads}"])
        m = re.search(r"corpus_files (\d+) candidates (\d+).*total_ms ([0-9.]+) cpu_ms ([0-9.]+) query (\S+)", err)
        if not m:
            sys.exit(f"engine failed on {pat!r}: {err.strip()}")
        corpus_files = int(m.group(1))
        cands.append(int(m.group(2)))
        mw.append(float(m.group(3)))
        mc.append(float(m.group(4)) / 1000.0)
        fallbacks += m.group(5) == "ANY"
        mine = files(out)

        walls, cpus, theirs = [], [], set()
        for _ in range(reps):
            w, c, o, _ = run(["rg", "-l", "--no-ignore", "--hidden", "--max-filesize", "8M", "--no-messages", "-e", pat, corpus])
            walls.append(w)
            cpus.append(c)
            theirs = files(o)
        rw.append(min(walls))
        rc.append(statistics.mean(cpus))
        if mine != theirs:
            diffs += 1
            print(f"DIFF {pat!r}: only_trigrep={sorted(mine - theirs)[:2]} only_rg={sorted(theirs - mine)[:2]}",
                  flush=True)

    n = len(pats)
    print(f"queries            {n}")
    print(f"corpus_files       {corpus_files}")
    print(f"candidates_median  {int(statistics.median(cands))}")
    print(f"candidates_p90     {int(pct(cands, 90))}")
    print(f"fallback_to_scan   {fallbacks}/{n}")
    print(f"trigrep_p50_ms     {pct(mw, 50):.1f}   (best of {reps}, index resident, {threads} threads)")
    print(f"trigrep_p95_ms     {pct(mw, 95):.1f}")
    print(f"trigrep_cpu_s_med  {statistics.median(mc):.4f}  (mean of {reps}, startup excluded)")
    print(f"ripgrep_p50_ms     {pct(rw, 50):.1f}   (best of {reps})")
    print(f"ripgrep_p95_ms     {pct(rw, 95):.1f}")
    print(f"ripgrep_cpu_s_med  {statistics.median(rc):.4f}  (mean of {reps})")
    print(f"cpu_ratio_median   {statistics.median(rc) / max(statistics.median(mc), 1e-9):.0f}x")
    print(f"wall_ratio_median  {pct(rw, 50) / max(pct(mw, 50), 1e-9):.0f}x")
    print(f"file_set_diffs_vs_ripgrep {diffs}/{n}")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    if len(args) != 3:
        sys.exit(__doc__)
    main(args[0], args[1], args[2], int(opts.get("reps", 5)), int(opts.get("threads", 2)))
