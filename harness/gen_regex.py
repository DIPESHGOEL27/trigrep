"""Grammar-driven random regex generator, weighted over the declared subset."""
import random, string

ATOMS = list(string.ascii_lowercase[:10]) + ['_', 'e', 't', 'r', 'in', 'de', 'st', 'con', 'int', 'str']

def gen(depth, rng):
    r = rng.random()
    if depth > 3 or r < 0.38:
        return ''.join(rng.choice(ATOMS) for _ in range(rng.randint(1, 4)))
    if r < 0.52: return f"({gen(depth+1, rng)}|{gen(depth+1, rng)})"
    if r < 0.63: return f"({gen(depth+1, rng)})*"
    if r < 0.74: return f"({gen(depth+1, rng)})+"
    if r < 0.83: return f"({gen(depth+1, rng)})?"
    if r < 0.91:
        cs = sorted(set(rng.choice(string.ascii_lowercase) for _ in range(rng.randint(2, 4))))
        return f"[{''.join(cs)}]"
    if r < 0.96: return gen(depth+1, rng) + "." + gen(depth+1, rng)
    lo = rng.randint(0, 2); hi = lo + rng.randint(0, 2)
    return f"({gen(depth+1, rng)}){{{lo},{hi}}}"

if __name__ == "__main__":
    import sys
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
    for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 100):
        print(gen(0, rng))
