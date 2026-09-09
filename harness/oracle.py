"""Differential harness against a reference regex engine, in batch mode.

Compares FILE-SET equality, not match positions: leftmost-longest vs
leftmost-first is a different question, and file-set agreement is the contract
a code-search service actually owes.

Also asserts candidates >= |oracle| on EVERY case. That is the soundness
invariant, and it is the failure mode that is silent rather than loud -- an
index that drops results still looks fast.

Batch mode: all patterns go to ONE trigrep_search process via --patterns-file
(the per-pattern exec of an earlier version cost ~45 ms of startup each, which
made 250K patterns a 40-hour job). The oracle side preloads the shard into
memory once and runs the reference engine in a process pool.

Tier "shard": generated regexes vs a brute-force scan of a 2,000-file shard.
Tier "full":  a workload file vs the whole corpus (slow; use small counts).
"""
import subprocess, sys, os, random, re, pathlib, time
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_regex import gen

try:
    import re2 as RE
    ENGINE = "google-re2"
except ImportError:
    import re as RE
    ENGINE = "python-re (fallback)"

BIN = "./build/trigrep_search"
_TEXTS = None

def _load(paths):
    global _TEXTS
    _TEXTS = []
    for f in paths:
        try:
            _TEXTS.append((f, pathlib.Path(f).read_text(errors="ignore")))
        except Exception:
            pass

def oracle_one(pat):
    try:
        rx = RE.compile(pat)
    except Exception:
        return None
    return frozenset(f for f, t in _TEXTS if rx.search(t))

def engine_batch(index, pats, threads):
    """Run every pattern through one process; return {i: (files, candidates)}."""
    pf = "/tmp/oracle_patterns.txt"
    with open(pf, "w") as f:
        for p in pats:
            f.write(p + "\n")
    p = subprocess.run([BIN, index, f"--patterns-file={pf}", f"--threads={threads}"],
                       capture_output=True, text=True)
    out = {}
    cur, files = None, set()
    for line in p.stdout.splitlines():
        if line.startswith("@@PATTERN "):
            cur = int(line.split()[1]); files = set()
        elif line.startswith("@@END "):
            out[cur] = [files, 0]; cur = None
        elif cur is not None and line:
            files.add(line)
    for line in p.stderr.splitlines():
        m = re.match(r"@@PATTERN (\d+) .*?candidates (\d+)", line)
        if m and int(m.group(1)) in out:
            out[int(m.group(1))][1] = int(m.group(2))
        m2 = re.match(r"@@PATTERN (\d+) parse_error", line)
        if m2:
            out[int(m2.group(1))] = None
    return out

def main(index, tier, count, seed, procs, threads):
    paths = [l.strip() for l in open(os.path.join(index, "docs.txt"))]
    if tier == "shard":
        random.Random(7).shuffle(paths)
        paths = paths[:2000]
    pathset = set(paths)
    rng = random.Random(seed)
    if tier == "full":
        pats = [l.strip() for l in open(count) if l.strip()]       # count is a file here
    else:
        pats = [gen(0, rng) for _ in range(count)]

    t0 = time.time()
    eng = engine_batch(index, pats, threads)
    t1 = time.time()
    print(f"engine: {len(pats)} patterns in {t1-t0:.1f}s ({len(pats)/max(t1-t0,1e-9):.0f}/s)", flush=True)

    counts = {"ok": 0, "mism": 0, "unsound": 0, "skip": 0}
    with Pool(procs, initializer=_load, initargs=(paths,)) as pool:
        for i, exp in enumerate(pool.imap(oracle_one, pats, chunksize=64)):
            got = eng.get(i)
            if exp is None or got is None:
                counts["skip"] += 1; continue
            files, cand = got
            files &= pathset
            if cand < len(exp):
                counts["unsound"] += 1
                print(f"UNSOUND cand={cand} oracle={len(exp)} pat={pats[i]!r}", flush=True)
            elif files != set(exp):
                counts["mism"] += 1
                print(f"MISMATCH pat={pats[i]!r} missing={sorted(set(exp)-files)[:2]} extra={sorted(files-set(exp))[:2]}", flush=True)
            else:
                counts["ok"] += 1
            if (i + 1) % 10000 == 0:
                print(f"  ...{i+1}/{len(pats)} ok={counts['ok']} mism={counts['mism']} "
                      f"unsound={counts['unsound']} skip={counts['skip']} ({time.time()-t1:.0f}s)", flush=True)
    checked = counts["ok"] + counts["mism"] + counts["unsound"]
    print(f"oracle={ENGINE} tier={tier} files={len(paths)} checked={checked} "
          f"skipped={counts['skip']} mismatches={counts['mism']} unsound={counts['unsound']} "
          f"oracle_s={time.time()-t1:.0f}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2],
         sys.argv[3] if sys.argv[2] == "full" else int(sys.argv[3]),
         int(sys.argv[4]) if len(sys.argv) > 4 else 1,
         int(sys.argv[5]) if len(sys.argv) > 5 else 2,
         int(sys.argv[6]) if len(sys.argv) > 6 else 1)
