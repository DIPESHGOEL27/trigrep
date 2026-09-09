"""Latency + CPU benchmark vs an unindexed ripgrep scan.

Reports CPU-seconds MEASURED (never derived) alongside wall time. CPU per
query is what decides whether a shared service is affordable, and it is the
thread-fair unit: 8-thread ripgrep burning N CPU-s is already normalised.
Both endpoints are printed so the ratio can be re-derived independently.
"""
import subprocess, resource, time, statistics, sys, re

def run(cmd):
    b = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True)
    wall = (time.perf_counter() - t0) * 1000.0
    a = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (a.ru_utime - b.ru_utime) + (a.ru_stime - b.ru_stime)
    return wall, cpu, p.stderr.decode(errors="ignore")

def pct(v, q):
    v = sorted(v)
    if not v: return 0.0
    k = min(int(q / 100.0 * len(v)), len(v) - 1)
    return v[k]

def main(qfile, corpus, index):
    mw, mc, rw, rc, cands, fallbacks = [], [], [], [], [], 0
    pats = [l.rstrip("\n") for l in open(qfile) if l.strip()]
    for pat in pats:
        w, c, err = run(["./build/trigrep_search", index, pat, "--stats", "--repeat=5"])
        mt = re.search(r"total_ms ([0-9.]+)", err)
        if mt: w = float(mt.group(1))      # engine latency, index resident
        mcpu = re.search(r"cpu_ms ([0-9.]+)", err)
        if mcpu: c = float(mcpu.group(1)) / 1000.0   # per-query CPU, startup excluded
        m = re.search(r"candidates (\d+).*?query (\S+)", err)
        if m:
            cands.append(int(m.group(1)))
            if m.group(2).startswith("ANY"): fallbacks += 1
        mw.append(w); mc.append(c)
        w, c, _ = run(["rg", "-l", "--no-ignore", "-e", pat, corpus])
        rw.append(w); rc.append(c)
    n = len(pats)
    print(f"queries            {n}")
    print(f"corpus_files       81664")
    print(f"candidates_median  {int(statistics.median(cands))}")
    print(f"candidates_p90     {int(pct(cands,90))}")
    print(f"fallback_to_scan   {fallbacks}/{n} ({100.0*fallbacks/max(n,1):.1f}%)")
    print(f"trigrep_p50_ms     {pct(mw,50):.1f}   (index resident, best of 5)")
    print(f"trigrep_p95_ms     {pct(mw,95):.1f}")
    print(f"trigrep_cpu_s_med  {statistics.median(mc):.4f}  (per query, startup excluded)")
    print(f"ripgrep_p50_ms     {pct(rw,50):.1f}")
    print(f"ripgrep_cpu_s_med  {statistics.median(rc):.4f}")
    print(f"cpu_ratio_median   {statistics.median(rc)/max(statistics.median(mc),1e-9):.0f}x")
    print(f"wall_ratio_median  {statistics.median(rw)/max(statistics.median(mw),1e-9):.1f}x")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
