"""Differential harness against a reference regex engine, in batch mode.

Compares FILE-SET equality, not match positions: leftmost-longest vs
leftmost-first is a different question, and file-set agreement is the contract
a code-search service actually owes.

Also asserts candidates >= |oracle| on EVERY case. That is the soundness
invariant, and it is the failure mode that is silent rather than loud -- an
index that drops results still looks fast. (Count-based: an under-return
still surfaces as a MISMATCH with a `missing=` list, so nothing is lost
silently, but the UNSOUND label alone does not prove the superset held.)

Memory: the first version slurped the engine's whole stdout (up to 10K
patterns x 2,000 paths ~ 1.6 GB) and kept each result as a set of path
strings; on a 3.9 GB WSL VM the OOM killer took it. Results are now streamed
line-by-line and stored as one Python int bitmask over the shard's files
(~250 bytes per pattern), and the oracle side returns the same encoding.

Batch mode: all patterns go to ONE trigrep_search process via --patterns-file
(the per-pattern exec of an earlier version cost ~45 ms of startup each).
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
_TEXTS = None      # per-worker: list of (bit, text)

def _load(paths):
    global _TEXTS
    _TEXTS = []
    for i, f in enumerate(paths):
        try:
            _TEXTS.append((1 << i, pathlib.Path(f).read_text(errors="ignore")))
        except Exception:
            pass

def oracle_one(pat):
    """Bitmask of shard files matching pat, or None if the reference engine rejects it."""
    try:
        rx = RE.compile(pat)
    except Exception:
        return None
    m = 0
    for bit, t in _TEXTS:
        if rx.search(t):
            m |= bit
    return m

def engine_batch(index, pats, threads, bit_of):
    """Stream the engine over all patterns; return {i: (bitmask, candidates)} or None."""
    pf = os.path.expanduser("~/tg/oracle/patterns.txt")
    os.makedirs(os.path.dirname(pf), exist_ok=True)
    with open(pf, "w") as f:
        for p in pats:
            f.write(p + "\n")
    # stderr goes to a FILE, not a pipe. The engine emits one stats line per
    # pattern on stderr; draining stdout to EOF first while stderr sat in a
    # 64 KB pipe buffer deadlocked both processes (found after a 2-hour hang
    # at 0% CPU with no engine child).
    ef = os.path.expanduser("~/tg/oracle/engine.err")
    with open(ef, "w") as errf:
        p = subprocess.Popen([BIN, index, f"--patterns-file={pf}", f"--threads={threads}"],
                             stdout=subprocess.PIPE, stderr=errf, text=True, bufsize=1 << 16)
        out, cur, mask = {}, None, 0
        for line in p.stdout:                   # streaming: nothing accumulates but bitmasks
            if line.startswith("@@PATTERN "):
                cur, mask = int(line.split()[1]), 0
            elif line.startswith("@@END "):
                out[cur] = [mask, 0]; cur = None
            elif cur is not None:
                b = bit_of.get(line.rstrip("\n"))
                if b: mask |= b
        p.wait()
    err = open(ef).read()
    for line in err.splitlines():
        m = re.match(r"@@PATTERN (\d+) .*?candidates (\d+)", line)
        if m and int(m.group(1)) in out:
            out[int(m.group(1))][1] = int(m.group(2))
        elif re.match(r"@@PATTERN (\d+) parse_error", line):
            out[int(re.match(r"@@PATTERN (\d+)", line).group(1))] = None
    return out

def names(mask, paths, k=2):
    r = []
    i = 0
    while mask and len(r) < k:
        if mask & 1: r.append(os.path.basename(paths[i]))
        mask >>= 1; i += 1
    return r

def main(index, tier, count, seed, procs, threads):
    paths = [l.strip() for l in open(os.path.join(index, "docs.txt"))]
    if tier == "shard":
        random.Random(7).shuffle(paths)
        paths = paths[:2000]
    bit_of = {p: 1 << i for i, p in enumerate(paths)}
    rng = random.Random(seed)
    pats = [l.strip() for l in open(count) if l.strip()] if tier == "full" \
        else [gen(0, rng) for _ in range(count)]

    t0 = time.time()
    eng = engine_batch(index, pats, threads, bit_of)
    t1 = time.time()
    print(f"engine: {len(pats)} patterns in {t1-t0:.1f}s ({len(pats)/max(t1-t0,1e-9):.0f}/s)", flush=True)

    counts = {"ok": 0, "mism": 0, "unsound": 0, "skip": 0}
    with Pool(procs, initializer=_load, initargs=(paths,)) as pool:
        for i, exp in enumerate(pool.imap(oracle_one, pats, chunksize=64)):
            got = eng.get(i)
            if exp is None or got is None:
                counts["skip"] += 1; continue
            mask, cand = got
            nexp = bin(exp).count("1")
            if cand < nexp:
                counts["unsound"] += 1
                print(f"UNSOUND cand={cand} oracle={nexp} pat={pats[i]!r}", flush=True)
            elif mask != exp:
                counts["mism"] += 1
                print(f"MISMATCH pat={pats[i]!r} missing={names(exp & ~mask, paths)} extra={names(mask & ~exp, paths)}", flush=True)
            else:
                counts["ok"] += 1
            if (i + 1) % 1000 == 0:
                print(f"  ...{i+1}/{len(pats)} ok={counts['ok']} mism={counts['mism']} "
                      f"unsound={counts['unsound']} skip={counts['skip']} ({time.time()-t1:.0f}s)", flush=True)
    checked = counts["ok"] + counts["mism"] + counts["unsound"]
    print(f"oracle={ENGINE} tier={tier} files={len(paths)} checked={checked} "
          f"skipped={counts['skip']} mismatches={counts['mism']} unsound={counts['unsound']} "
          f"engine_s={t1-t0:.0f} oracle_s={time.time()-t1:.0f}", flush=True)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2],
         sys.argv[3] if sys.argv[2] == "full" else int(sys.argv[3]),
         int(sys.argv[4]) if len(sys.argv) > 4 else 1,
         int(sys.argv[5]) if len(sys.argv) > 5 else 2,
         int(sys.argv[6]) if len(sys.argv) > 6 else 1)
