#include "parser.hpp"
#include <stdexcept>
#include <cctype>
#include <cstring>

// Recursive descent over an RE2-compatible subset. Anything outside the
// subset is REJECTED with an error rather than silently reinterpreted: an
// earlier version read \b as the letter b and [\d] as the letter d, which
// returned wrong answers with no warning. A pattern error is loud; a wrong
// answer is not.
namespace {

// RE2's Perl classes are ASCII-only: \d [0-9], \w [0-9A-Za-z_], \s [\t\n\f\r ].
// The upper-case forms are complements, so they admit every byte >= 0x80 and
// the NFA compiles them to the UTF-8 code-point automaton.
void add_perl_class(std::bitset<256>& b, char k) {
    std::bitset<256> x;
    switch (std::tolower((unsigned char)k)) {
    case 'd': for (int c = '0'; c <= '9'; ++c) x.set(size_t(c)); break;
    case 'w': for (int c = 0; c < 128; ++c) if (std::isalnum(c) || c == '_') x.set(size_t(c)); break;
    case 's': for (int c : {'\t', '\n', '\f', '\r', ' '}) x.set(size_t(c)); break;
    }
    if (std::isupper((unsigned char)k)) x.flip();
    b |= x;
}
bool is_perl_class(char k) { return k && std::strchr("dwsDWS", k) != nullptr; }

// Single-byte escapes. Punctuation escapes to itself; the listed letters are
// control characters. Any other letter or digit (\b \B \A \z \x \p \1 ...) is
// a feature this engine does not implement, so it is an error.
bool simple_escape(char c, unsigned char& out) {
    switch (c) {
    case 'n': out = '\n'; return true;
    case 't': out = '\t'; return true;
    case 'r': out = '\r'; return true;
    case 'f': out = '\f'; return true;
    case 'v': out = '\v'; return true;
    }
    if (!std::isalnum((unsigned char)c) && (unsigned char)c < 0x80) { out = (unsigned char)c; return true; }
    return false;
}
[[noreturn]] void unsupported_escape(char c) {
    throw std::runtime_error(std::string("unsupported escape \\") + c);
}

// Length of the UTF-8 sequence led by byte b (1 for ASCII or a stray byte).
size_t utf8_len(unsigned char b) {
    if (b >= 0xF0 && b <= 0xF4) return 4;
    if (b >= 0xE0) return b <= 0xEF ? 3 : 1;
    if (b >= 0xC2) return 2;
    return 1;
}

struct P {
    const std::string& s; size_t i; Arena& a;
    bool eof() const { return i >= s.size(); }
    char peek() const { return s[i]; }
    bool eat(char c) { if (!eof() && s[i] == c) { ++i; return true; } return false; }

    Node* lit_node(const std::string& t) { Node* n = a.make(Op::Literal); n->lit = t; return n; }

    Node* alt() {
        Node* first = concat();
        if (eof() || peek() != '|') return first;
        Node* n = a.make(Op::Alt);
        n->kids.push_back(first);
        while (eat('|')) n->kids.push_back(concat());
        return n;
    }
    Node* concat() {
        Node* n = a.make(Op::Concat);
        while (!eof() && peek() != '|' && peek() != ')') n->kids.push_back(repeat());
        if (n->kids.empty()) return a.make(Op::Empty);
        if (n->kids.size() == 1) return n->kids[0];
        return n;
    }
    Node* repeat() {
        Node* at = atom();
        for (;;) {
            if (eat('*'))      { Node* n = a.make(Op::Star);  n->kids.push_back(at); at = n; }
            else if (eat('+')) { Node* n = a.make(Op::Plus);  n->kids.push_back(at); at = n; }
            else if (eat('?')) { Node* n = a.make(Op::Quest); n->kids.push_back(at); at = n; }
            else if (!eof() && peek() == '{') {
                size_t save = i; ++i;
                int lo = 0, hi = -1; bool got = false;
                while (!eof() && std::isdigit((unsigned char)peek())) { lo = lo*10 + (s[i++]-'0'); got = true; if (lo > 100000) break; }
                if (eat(',')) {
                    if (!eof() && std::isdigit((unsigned char)peek())) {
                        hi = 0;
                        while (!eof() && std::isdigit((unsigned char)peek())) { hi = hi*10 + (s[i++]-'0'); if (hi > 100000) break; }
                    }
                } else hi = lo;
                if (!got || !eat('}')) { i = save; break; }    // not a repeat: '{' is a literal
                if (lo > 1000 || hi > 1000) throw std::runtime_error("repeat count above 1000");
                if (hi >= 0 && hi < lo) throw std::runtime_error("bad repeat range");
                Node* n = a.make(Op::Repeat);
                n->kids.push_back(at); n->rmin = lo; n->rmax = hi; at = n;
            } else break;
        }
        return at;
    }
    Node* atom() {
        if (eof()) throw std::runtime_error("unexpected end of pattern");
        if (eat('(')) {
            if (!eof() && peek() == '?') {
                if (i + 1 < s.size() && s[i+1] == ':') i += 2;      // non-capturing group
                else throw std::runtime_error("unsupported group flag (?");
            }
            Node* n = alt();
            if (!eat(')')) throw std::runtime_error("unclosed (");
            return n;
        }
        char c = peek();
        if (c == '*' || c == '+' || c == '?') throw std::runtime_error("missing argument to repetition operator");
        if (eat('[')) return cls();
        if (eat('.')) { Node* n = a.make(Op::Class); n->cls.set(); n->cls.reset('\n'); return n; }
        if (eat('^')) return a.make(Op::Bol);
        if (eat('$')) return a.make(Op::Eol);
        if (eat('\\')) {
            if (eof()) throw std::runtime_error("trailing backslash");
            char e = s[i++];
            if (is_perl_class(e)) { Node* n = a.make(Op::Class); add_perl_class(n->cls, e); return n; }
            unsigned char b;
            if (!simple_escape(e, b)) unsupported_escape(e);
            return lit_node(std::string(1, char(b)));
        }
        // A multi-byte UTF-8 character is ONE atom, so `é+` repeats the whole
        // code point, as RE2 does, not just its last byte.
        size_t n = std::min(utf8_len((unsigned char)c), s.size() - i);
        Node* lit = lit_node(s.substr(i, n));
        i += n;
        return lit;
    }
    // One class member: a byte, or (when `perl` is set) a \d-style class.
    unsigned char class_byte(bool allow_perl, char& perl) {
        perl = 0;
        unsigned char c = (unsigned char)s[i++];
        if (c >= 0x80) throw std::runtime_error("non-ASCII character in class is unsupported");
        if (c != '\\') return c;
        if (eof()) throw std::runtime_error("trailing backslash");
        char e = s[i++];
        if (is_perl_class(e)) {
            if (!allow_perl) throw std::runtime_error("bad character class range");
            perl = e; return 0;
        }
        unsigned char b;
        if (!simple_escape(e, b)) unsupported_escape(e);
        return b;
    }
    Node* cls() {
        Node* n = a.make(Op::Class);
        bool neg = eat('^');
        bool first = true;
        while (!eof() && (peek() != ']' || first)) {
            first = false;
            char perl;
            unsigned char lo = class_byte(true, perl);
            if (perl) { add_perl_class(n->cls, perl); continue; }
            if (!eof() && peek() == '-' && i + 1 < s.size() && s[i+1] != ']') {
                ++i;
                unsigned char hi = class_byte(false, perl);
                if (hi < lo) throw std::runtime_error("bad character class range");
                for (int c = lo; c <= int(hi); ++c) n->cls.set(size_t(c));
            } else n->cls.set(lo);
        }
        if (!eat(']')) throw std::runtime_error("unclosed [");
        if (neg) n->cls.flip();
        return n;
    }
};
} // namespace

Node* parse_regex(const std::string& pattern, Arena& a) {
    P p{pattern, 0, a};
    Node* n = p.alt();
    if (!p.eof()) throw std::runtime_error("unmatched )");
    return n;
}
