"""Copy a reproducible random sample of a corpus into a shard directory.

Keeps only files the engine would index (regular, non-empty, <= 8 MB, no NUL
byte) that are also valid UTF-8, so the oracle can decode them strictly and
both sides of the comparison read exactly the same bytes.

usage: make_shard.py <corpus-root> <out-dir> [n=2000] [seed=7]
"""
import os
import random
import shutil
import sys

MAX_BYTES = 8 << 20


def eligible(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    size = os.path.getsize(path)
    if size == 0 or size > MAX_BYTES:
        return False
    with open(path, "rb") as f:
        data = f.read()
    if b"\0" in data:
        return False
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return True


def main(root, out, n, seed):
    files = []
    for d, dirs, names in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x != ".git")
        files.extend(os.path.join(d, x) for x in sorted(names))
    random.Random(seed).shuffle(files)
    picked = []
    for f in files:
        if eligible(f):
            picked.append(f)
            if len(picked) == n:
                break
    if os.path.exists(out):
        shutil.rmtree(out)
    for f in picked:
        dst = os.path.join(out, os.path.relpath(f, root))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(f, dst)
    print(f"shard {out}: {len(picked)} files, {sum(os.path.getsize(f) for f in picked)} bytes")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2],
         int(sys.argv[3]) if len(sys.argv) > 3 else 2000,
         int(sys.argv[4]) if len(sys.argv) > 4 else 7)
