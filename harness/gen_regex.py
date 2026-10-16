"""Grammar-driven random regex generator over the declared RE2 subset.

The first generator only ever joined pieces with '.', so it never placed a
group directly next to a literal -- the one shape that exercises trigrams
straddling a group boundary. It shipped 10,000 cases that could not reach the
alternation soundness bug. This grammar concatenates terms directly, and
covers anchors, Perl classes, ranges, negated classes, digits and upper case.
"""
import random
import string

WORDS = ['in', 'de', 'st', 'con', 'int', 'str', 'ret', 'urn', 'if', 'for', 'el', 'se',
         'lock', 'irq', 'dev', 'buf', 'len', 'init', 'free', 'err', 'data', 'size']
CHARS = list(string.ascii_lowercase[:12]) + ['_', 'e', 't', 'r', 's', 'n', 'o']
RARE = ['A', 'E', 'R', 'S', 'T', '0', '1', '2', '(', ')', ';', '*', ' ', '=']
ESCAPED = {'(': r'\(', ')': r'\)', '*': r'\*'}


def literal(rng):
    out = []
    for _ in range(rng.randint(1, 3)):
        r = rng.random()
        if r < 0.45:
            out.append(rng.choice(WORDS))
        elif r < 0.9:
            out.append(rng.choice(CHARS))
        else:
            c = rng.choice(RARE)
            out.append(ESCAPED.get(c, c))
    return ''.join(out)


def char_class(rng):
    r = rng.random()
    if r < 0.30:
        return rng.choice([r'\d', r'\w', r'\s', r'\D', r'\W', r'\S'])
    if r < 0.55:
        lo = rng.choice('abcdefgh')
        hi = chr(min(ord('z'), ord(lo) + rng.randint(1, 8)))
        neg = '^' if rng.random() < 0.3 else ''
        return f'[{neg}{lo}-{hi}]'
    if r < 0.70:
        return rng.choice(['[A-Z]', '[0-9]', '[A-Za-z_]', r'[\w.]', r'[ \t]', '[_a-z0-9]'])
    members = ''.join(sorted(set(rng.choice(string.ascii_lowercase + '_') for _ in range(rng.randint(2, 4)))))
    neg = '^' if rng.random() < 0.3 else ''
    return f'[{neg}{members}]'


def term(depth, rng):
    r = rng.random()
    if depth > 2 or r < 0.36:
        return literal(rng)
    if r < 0.50:
        return f'({gen(depth + 1, rng)}|{gen(depth + 1, rng)})'
    if r < 0.58:
        return f'({gen(depth + 1, rng)})*'
    if r < 0.66:
        return f'({gen(depth + 1, rng)})+'
    if r < 0.73:
        return f'({gen(depth + 1, rng)})?'
    if r < 0.84:
        return char_class(rng) + rng.choice(['', '', '+', '*', '?'])
    if r < 0.90:
        return rng.choice(['.', '.*', '.+', '.?'])
    lo = rng.randint(0, 2)
    hi = lo + rng.randint(0, 2)
    return f'({gen(depth + 1, rng)}){{{lo},{hi}}}'


def gen(depth, rng):
    """A concatenation of 1 to 3 terms, sometimes line-anchored."""
    body = ''.join(term(depth, rng) for _ in range(rng.randint(1, 3)))
    if depth == 0:
        r = rng.random()
        if r < 0.08:
            body = '^' + body
        elif r < 0.14:
            body = body + '$'
    return body


if __name__ == "__main__":
    import sys
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
    for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 100):
        print(gen(0, rng))
