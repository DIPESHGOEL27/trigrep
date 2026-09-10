#!/bin/bash
# End-to-end smoke test on a synthetic 3-file corpus. Exact assertions, no
# corpus download: builds an index, runs queries that exercise the literal
# path, alternation with straddling trigrams, the UTF-8 code-point rule for
# `.`, the degrade-to-ANY path, batch mode, and a negative. Run via `make check`.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/c"
printf 'int kmalloc_thing = 1;\n'                 > "$T/c/a.c"
printf 'static void foo_bar_baz(void) {}\n'       > "$T/c/b.c"
printf '// nothing here, just Wa\xc3\x9fmann\n'   > "$T/c/c.c"   # ß = C3 9F

./build/trigrep_index "$T/c" "$T/idx" > "$T/build.log"
grep -q '^files  *3$' "$T/build.log" || { echo "FAIL: expected 3 files"; cat "$T/build.log"; exit 1; }

expect() {  # expect <regex> <space-separated basenames or "">
    local got; got=$(./build/trigrep_search "$T/idx" "$1" --threads=1 2>/dev/null | xargs -rn1 basename | sort | tr '\n' ' ' | sed 's/ $//')
    if [ "$got" != "$2" ]; then echo "FAIL: $1 -> got [$got] want [$2]"; exit 1; fi
    printf '  ok  %-22s -> [%s]\n' "$1" "$got"
}
expect 'kmalloc_thing'       'a.c'
expect 'foo_(bar|qux)_baz'   'b.c'        # alternation + straddling trigrams
expect 'Wa.mann'             'c.c'        # . consumes one UTF-8 code point
expect 'Wa..mann'            ''           # two dots would need two code points
expect 'a[^x]mann'           'c.c'        # negated class over a high byte
expect '(a)+.[mpwz]'         'c.c'        # the exact pattern the oracle caught
expect 'zzz_absent'          ''
expect 'x*'                  'a.c b.c c.c' # degrades to ANY: every file is a candidate, verified

# batch mode: one process, three patterns, delimiters intact
printf 'kmalloc_thing\nfoo_bar_baz\nzzz_absent\n' > "$T/p.txt"
n=$(./build/trigrep_search "$T/idx" "--patterns-file=$T/p.txt" --threads=1 2>/dev/null | grep -c '^@@END')
[ "$n" = 3 ] || { echo "FAIL: batch mode emitted $n blocks, want 3"; exit 1; }
echo "  ok  batch mode            -> 3 blocks"
echo "smoke: all checks passed"
