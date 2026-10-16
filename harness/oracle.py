"""Differential harness against google-re2, in batch mode.

Compares FILE-SET equality, not match positions: leftmost-longest vs
leftmost-first is a different question, and file-set agreement is the contract
a code-search service actually owes.

Soundness is checked EXACTLY: the engine is run with --emit-candidates, and a
case is UNSOUND if any file the oracle matches is missing from the candidate
set, i.e. the index pruned a true match before verification ever ran. (The
first version only compared candidate COUNTS, which cannot prove a superset.)
If the binary under test predates --emit-candidates, the count check is used
and the summary says so.

Semantics are pinned so both sides see the same question:
  * patterns compile with (?m): ^ and $ are line anchors, as in grep and code
    search, and as the engine implements them;
  * files are read as raw bytes and decoded as strict UTF-8. Text mode would
    turn \\r\\n into \\n and hide CR bytes the engine sees, and a lossy decode
    would drop bytes. make_shard.py keeps only valid UTF-8 files for this.

Memory: results are streamed line by line and stored as one Python int bitmask
over the shard's files, so 10,000 patterns cost megabytes, not gigabytes.
"""
import os
import re
import subprocess
import sys
import time
import random
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_regex import gen  # noqa: E402

try:
    import re2 as RE
    ENGINE = "google-re2"
except ImportError:
    import re as RE
    ENGINE = "python-re (fallback)"

BIN = os.environ.get("TRIGREP_BIN", "./build/trigrep_search")
_TEXTS = None      # per worker: list of (bit, text)


def _load(paths):
    global _TEXTS
    _TEXTS = []
    for i, f in enumerate(paths):
        with open(f, "rb") as fh:
            _TEXTS.append((1 << i, fh.read().decode("utf-8")))   # strict: shard is valid UTF-8


def oracle_one(pat):
    """Bitmask of shard files matching pat, or None if the reference engine rejects it."""
    try:
        rx = RE.compile("(?m)" + pat)
    except Exception:
        return None
    m = 0
    for bit, t in _TEXTS:
        if rx.search(t):
            m |= bit
    return m


def engine_batch(index, pats, threads, bit_of, workdir):
    """Stream the engine over all patterns.

    Returns ({i: [match_mask, cand_count, cand_mask, query_is_any]} with None
    for a parse error, emitted_candidates).
    """
    pf = os.path.join(workdir, "patterns.txt")
    with open(pf, "w") as f:
        for p in pats:
            f.write(p + "\n")
    # stderr goes to a FILE, not a pipe: draining stdout while stderr sat in a
    # full 64 KB pipe buffer once deadlocked both processes for two hours.
    ef = os.path.join(workdir, "engine.err")
    out, emitted = {}, False
    with open(ef, "w") as errf:
        p = subprocess.Popen([BIN, index, f"--patterns-file={pf}", f"--threads={threads}",
                              "--emit-candidates"],
                             stdout=subprocess.PIPE, stderr=errf, text=True, bufsize=1 << 16)
        cur, mask, cmask = None, 0, 0
        for line in p.stdout:
            line = line.rstrip("\n")
            if line.startswith("@@PATTERN "):
                cur, mask, cmask = int(line.split()[1]), 0, 0
            elif line.startswith("@@END "):
                out[cur] = [mask, 0, cmask, False]
                cur = None
            elif cur is None:
                continue
            elif line.startswith("@@C "):
                emitted = True
                b = bit_of.get(line[4:])
                if b:
                    cmask |= b
            else:
                b = bit_of.get(line)
                if b:
                    mask |= b
        p.wait()
    with open(ef) as fh:
        for line in fh:
            m = re.match(r"@@PATTERN (\d+) (parse_error)?", line)
            if not m or int(m.group(1)) not in out:
                continue
            i = int(m.group(1))
            if m.group(2):
                out[i] = None
                continue
            c = re.search(r"candidates (\d+).*query (\S+)", line)
            if c:
                out[i][1] = int(c.group(1))
                out[i][3] = c.group(2) == "ANY"
    return out, emitted


def names(mask, paths, k=2):
    r, i = [], 0
    while mask and len(r) < k:
        if mask & 1:
            r.append(os.path.basename(paths[i]))
        mask >>= 1
        i += 1
    return r


def main(index, count, seed, procs, threads):
    paths = [l.rstrip("\n") for l in open(os.path.join(index, "docs.txt"))]
    bit_of = {p: 1 << i for i, p in enumerate(paths)}
    rng = random.Random(seed)
    pats = [gen(0, rng) for _ in range(count)]
    workdir = os.path.expanduser("~/tg/oracle")
    os.makedirs(workdir, exist_ok=True)

    t0 = time.time()
    eng, emitted = engine_batch(index, pats, threads, bit_of, workdir)
    t1 = time.time()
    print(f"engine: {BIN}  {len(pats)} patterns in {t1-t0:.1f}s", flush=True)

    c = {"ok": 0, "mism": 0, "unsound": 0, "skip_ref": 0, "skip_engine": 0,
         "any": 0, "nonempty": 0}
    with Pool(procs, initializer=_load, initargs=(paths,)) as pool:
        for i, exp in enumerate(pool.imap(oracle_one, pats, chunksize=64)):
            got = eng.get(i, "missing")
            if exp is None:
                c["skip_ref"] += 1
                continue
            if got is None or got == "missing":
                c["skip_engine"] += 1
                print(f"ENGINE-REJECTED pat={pats[i]!r}", flush=True)
                continue
            mask, cand, cmask, is_any = got
            c["any"] += is_any
            c["nonempty"] += exp != 0
            lost = (exp & ~cmask) if emitted else (exp if cand < bin(exp).count("1") else 0)
            if lost:
                c["unsound"] += 1
                print(f"UNSOUND pat={pats[i]!r} pruned={names(lost, paths)}", flush=True)
            elif mask != exp:
                c["mism"] += 1
                print(f"MISMATCH pat={pats[i]!r} missing={names(exp & ~mask, paths)} "
                      f"extra={names(mask & ~exp, paths)}", flush=True)
            else:
                c["ok"] += 1
            if (i + 1) % 2000 == 0:
                print(f"  ...{i+1}/{len(pats)} ok={c['ok']} mism={c['mism']} unsound={c['unsound']} "
                      f"({time.time()-t1:.0f}s)", flush=True)
    checked = c["ok"] + c["mism"] + c["unsound"]
    print(f"oracle={ENGINE} files={len(paths)} checked={checked} mismatches={c['mism']} "
          f"unsound={c['unsound']} superset_check={'exact' if emitted else 'count-only'} "
          f"skipped_by_re2={c['skip_ref']} rejected_by_engine={c['skip_engine']} "
          f"degraded_to_ANY={c['any']} nonempty_answer={c['nonempty']} "
          f"engine_s={t1-t0:.0f} oracle_s={time.time()-t1:.0f}", flush=True)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: oracle.py <shard-index> <count> [seed] [procs] [threads]")
    main(sys.argv[1], int(sys.argv[2]),
         int(sys.argv[3]) if len(sys.argv) > 3 else 1,
         int(sys.argv[4]) if len(sys.argv) > 4 else 2,
         int(sys.argv[5]) if len(sys.argv) > 5 else 1)
