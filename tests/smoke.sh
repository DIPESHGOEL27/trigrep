#!/bin/bash
# End-to-end smoke test on a synthetic corpus. Exact assertions, no corpus
# download: builds an index, then runs queries that each pin down one rule --
# the literal path, alternation and straddling trigrams, alternation with an
# unknown branch (a soundness bug the oracle missed for a release), UTF-8
# code points for `.`, line anchors, Perl classes, rejected escapes, the
# NFA size cap, degrade-to-ANY, batch mode and exit codes. Run via `make check`.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/c"
printf 'int kmalloc_thing = 1;\n'                  > "$T/c/a.c"
printf 'static void foo_bar_baz(void) {}\n'        > "$T/c/b.c"
printf '// nothing here, just Wa\xc3\x9fmann\n'    > "$T/c/c.c"   # ß = C3 9F
printf 'int xyzdef = 2;\n'                         > "$T/c/d.c"
printf '  #include <x.h>\n'                        > "$T/c/e.c"
printf '#include <y.h>\nint v7 = 3; /* tail */\n'  > "$T/c/f.c"

./build/trigrep_index "$T/c" "$T/idx" > "$T/build.log"
grep -q '^files  *6$' "$T/build.log" || { echo "FAIL: expected 6 files"; cat "$T/build.log"; exit 1; }

search() { ./build/trigrep_search "$T/idx" "$@" --threads=1 2>/dev/null; }
expect() {  # expect <regex> <space-separated basenames or "">
    local got
    got=$( { search "$1" || true; } | xargs -rn1 basename | sort | tr '\n' ' ' | sed 's/ $//')
    if [ "$got" != "$2" ]; then echo "FAIL: $1 -> got [$got] want [$2]"; exit 1; fi
    printf '  ok  %-24s -> [%s]\n' "$1" "$got"
}
expect_rc() {  # expect_rc <regex> <exit code>
    local rc=0
    search "$1" > /dev/null || rc=$?
    if [ "$rc" != "$2" ]; then echo "FAIL: $1 -> exit $rc, want $2"; exit 1; fi
    printf '  ok  %-24s -> exit %s\n' "$1" "$rc"
}

expect 'kmalloc_thing'          'a.c'
expect 'foo_(bar|qux)_baz'      'b.c'          # alternation + straddling trigrams
expect '(abc|x.*)def'           'd.c'          # a branch with unknown prefix keeps the
expect 'foo(_qux|.*)baz'        'b.c'          #   whole alternation's prefix unknown
expect 'Wa.mann'                'c.c'          # . consumes one UTF-8 code point
expect 'Wa..mann'               ''             # two dots would need two code points
expect 'a[^x]mann'              'c.c'          # negated class over a high byte
expect '(a)+.[mpwz]'            'c.c'          # the pattern the oracle caught first
expect '^#include'              'f.c'          # ^ is a line anchor, not "anywhere"
expect '^[ \t]+#include'        'e.c'
expect 'tail \*/$'              'f.c'          # $ before the newline
expect 'v\d = \d'               'f.c'          # \d is a digit, not the letter d
expect '[\d]'                   'a.c d.c f.c'  # ... inside a class too
expect 'int\W'                  'a.c d.c f.c'
expect 'zzz_absent'             ''
expect 'x*'                     'a.c b.c c.c d.c e.c f.c'   # degrades to ANY, then verified

expect_rc 'kmalloc_thing'            0
expect_rc 'zzz_absent'               1
expect_rc '\bint'                    2         # unsupported escape: an error, not the letter b
expect_rc '(((ab){512}){512}){512}'  2         # past the NFA state cap: an error, not an OOM

# batch mode: one process, three patterns, delimiters intact
printf 'kmalloc_thing\r\nfoo_bar_baz\nzzz_absent\n' > "$T/p.txt"
n=$(./build/trigrep_search "$T/idx" "--patterns-file=$T/p.txt" --threads=1 2>/dev/null | grep -c '^@@END')
[ "$n" = 3 ] || { echo "FAIL: batch mode emitted $n blocks, want 3"; exit 1; }
hit=$(./build/trigrep_search "$T/idx" "--patterns-file=$T/p.txt" --threads=1 2>/dev/null | grep -c 'a\.c$' || true)
[ "$hit" = 1 ] || { echo "FAIL: CRLF pattern line did not match"; exit 1; }
echo "  ok  batch mode               -> 3 blocks, CRLF tolerated"
echo "smoke: all checks passed"
