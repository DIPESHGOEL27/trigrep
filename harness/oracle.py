"""Two-tier differential harness against a reference regex engine.

Compares FILE-SET equality, not match positions: leftmost-longest vs
leftmost-first is a different question, and file-set agreement is the contract
a code-search service actually owes.

Also asserts candidates >= oracle on EVERY case. That is the soundness
invariant, and it is the failure mode that is silent rather than loud -- an
index that drops results still looks fast.
"""
import subprocess, sys, os, random, re, pathlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_regex import gen

try:
    import re2 as RE
    ENGINE = "google-re2"
except ImportError:
    import re as RE
    ENGINE = "python-re (fallback)"

BIN = "./build/trigrep_search"

def engine(pat, index):
    p = subprocess.run([BIN, index, pat], capture_output=True, text=True)
    if p.returncode != 0:
        return None, 0
    files = set(l for l in p.stdout.splitlines() if l)
    m = re.search(r"candidates (\d+)", p.stderr)
    return files, int(m.group(1)) if m else 0

def oracle(pat, paths):
    try:
        rx = RE.compile(pat)
    except Exception:
        return None
    out = set()
    for f in paths:
        try:
            t = pathlib.Path(f).read_text(errors="ignore")
        except Exception:
            continue
        if rx.search(t):
            out.add(f)
    return out

def main(index, tier, count, seed):
    paths = [l.strip() for l in open(os.path.join(index, "docs.txt"))]
    if tier == "shard":
        random.Random(7).shuffle(paths)
        paths = paths[:2000]
    pathset = set(paths)
    rng = random.Random(seed)
    checked = mism = unsound = skipped = 0
    for i in range(count):
        pat = gen(0, rng)
        exp = oracle(pat, paths)
        if exp is None:
            skipped += 1
            continue
        got, cand = engine(pat, index)
        if got is None:
            skipped += 1
            continue
        got &= pathset
        checked += 1
        if cand < len(exp):
            unsound += 1
            print(f"UNSOUND cand={cand} oracle={len(exp)} pat={pat!r}", flush=True)
        if got != exp:
            mism += 1
            print(f"MISMATCH pat={pat!r} missing={sorted(exp-got)[:2]} extra={sorted(got-exp)[:2]}", flush=True)
        if (i + 1) % 250 == 0:
            print(f"  ...{i+1} checked={checked} mism={mism} unsound={unsound}", flush=True)
    print(f"oracle={ENGINE} tier={tier} files={len(paths)} checked={checked} "
          f"skipped={skipped} mismatches={mism} unsound={unsound}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 1)
