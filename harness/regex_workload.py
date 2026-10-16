"""Derive a REGEX workload from the identifier workload, mechanically.

The held-out benchmark set is 100 plain identifiers, so it measures literal
search. This turns each identifier into one regex by a fixed rule chosen by
its position, so the regex tier is not hand-picked and is reproducible:

  0  call or declaration site      ident[ \\t]*\\(
  1  whole statement on one line   ^[ \\t]*.*ident.*;$
  2  alternation of tails          head_(tail|other_tail)
  3  optional suffixes, then [=;,)] ident(_[a-z0-9]+)*[ \\t]*[=;,)]
  4  identifier prefix class       [A-Za-z0-9_]*ident[A-Za-z0-9_]*[ \\t]*\\(

Only ASCII classes are used, never \\s or \\w: ripgrep's \\s and \\w are
Unicode-aware while RE2's are ASCII, and the bench diffs the two file sets.

usage: regex_workload.py <identifiers.txt>   (writes to stdout)
"""
import sys


def transform(i, ident, nxt):
    k = i % 5
    if k == 0:
        return rf"{ident}[ \t]*\("
    if k == 1:
        return rf"^[ \t]*.*{ident}.*;$"
    if k == 2:
        if "_" in ident:
            head, tail = ident.rsplit("_", 1)
            other = nxt.rsplit("_", 1)[-1]
            if other == tail:
                other = tail + "s"
            return f"{head}_({tail}|{other})"
        return f"({ident}|{ident}_[a-z]+)"
    if k == 3:
        return rf"{ident}(_[a-z0-9]+)*[ \t]*[=;,)]"
    return rf"[A-Za-z0-9_]*{ident}[A-Za-z0-9_]*[ \t]*\("


def main(path):
    idents = [l.strip() for l in open(path) if l.strip()]
    for i, ident in enumerate(idents):
        print(transform(i, ident, idents[(i + 1) % len(idents)]))


if __name__ == "__main__":
    main(sys.argv[1])
