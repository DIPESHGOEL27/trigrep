"""Two-tier differential harness against a reference regex engine.

Compares FILE-SET equality, not match positions: leftmost-longest vs
leftmost-first is a different question, and file-set agreement is the contract
a code-search service actually owes.

Also asserts candidates >= |oracle| on EVERY case. That is the soundness
invariant, and it is the failure mode that is silent rather than loud -- an
index that drops results still looks fast.

Tier "shard": N generated regexes vs a brute-force scan of a 2,000-file shard,
preloaded into memory once (re-reading 25 MB per pattern from disk was the
difference between 30 minutes and 21 hours). Parallel over patterns.
Tier "full": workload queries vs the whole corpus, single process.
"""
import subprocess, sys, os, random, re, pathlib
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
_TEXTS = None      # per-process cache: list of (path, text)

def _load(paths):
    global _TEXTS
    _TEXTS = []
    for f in paths:
        try:
            _TEXTS.append((f, pathlib.Path(f).read_text(errors="ignore")))
        except Exception:
            pass

def engine(pat, index):
    p = subprocess.run([BIN, index, pat, "--threads=1"], capture_output=True, text=True)
    if p.returncode != 0:
        return None, 0
    files = set(l for l in p.stdout.splitlines() if l)
    m = re.search(r"candidates (\d+)", p.stderr)
    return files, int(m.group(1)) if m else 0

def check_one(args):
    pat, index, pathset = args
    try:
        rx = RE.compile(pat)
    except Exception:
        return ("skip", pat, None)
    exp = set(f for f, t in _TEXTS if rx.search(t))
    got, cand = engine(pat, index)
    if got is None:
        return ("skip", pat, None)
    got &= pathset
    unsound = cand < len(exp)
    mism = got != exp
    detail = None
    if mism or unsound:
        detail = f"cand={cand} oracle={len(exp)} missing={sorted(exp-got)[:2]} extra={sorted(got-exp)[:2]}"
    return ("unsound" if unsound else ("mism" if mism else "ok"), pat, detail)

def main(index, tier, count, seed, procs):
    paths = [l.strip() for l in open(os.path.join(index, "docs.txt"))]
    if tier == "shard":
        random.Random(7).shuffle(paths)
        paths = paths[:2000]
    pathset = set(paths)
    rng = random.Random(seed)
    pats = [gen(0, rng) for _ in range(count)]
    counts = {"ok": 0, "mism": 0, "unsound": 0, "skip": 0}
    with Pool(procs, initializer=_load, initargs=(paths,)) as pool:
        for i, (st, pat, detail) in enumerate(
                pool.imap_unordered(check_one, [(p, index, pathset) for p in pats], chunksize=32), 1):
            counts[st] += 1
            if st in ("mism", "unsound"):
                print(f"{st.upper()} pat={pat!r} {detail}", flush=True)
            if i % 5000 == 0:
                print(f"  ...{i}/{count} ok={counts['ok']} mism={counts['mism']} "
                      f"unsound={counts['unsound']} skip={counts['skip']}", flush=True)
    checked = counts["ok"] + counts["mism"] + counts["unsound"]
    print(f"oracle={ENGINE} tier={tier} files={len(paths)} checked={checked} "
          f"skipped={counts['skip']} mismatches={counts['mism']} unsound={counts['unsound']}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]),
         int(sys.argv[4]) if len(sys.argv) > 4 else 1,
         int(sys.argv[5]) if len(sys.argv) > 5 else 2)
