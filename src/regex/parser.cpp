#include "parser.hpp"
#include <stdexcept>
#include <cctype>

namespace {
struct P {
    const std::string& s; size_t i; Arena& a;
    bool eof() const { return i >= s.size(); }
    char peek() const { return s[i]; }
    bool eat(char c) { if (!eof() && s[i] == c) { ++i; return true; } return false; }

    Node* lit_node(char c) { Node* n = a.make(Op::Literal); n->lit.assign(1, c); return n; }

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
                while (!eof() && std::isdigit((unsigned char)peek())) { lo = lo*10 + (s[i++]-'0'); got = true; }
                if (eat(',')) {
                    if (!eof() && std::isdigit((unsigned char)peek())) {
                        hi = 0;
                        while (!eof() && std::isdigit((unsigned char)peek())) hi = hi*10 + (s[i++]-'0');
                    }
                } else hi = lo;
                if (!got || !eat('}') || lo > 512 || hi > 512) { i = save; break; }
                Node* n = a.make(Op::Repeat);
                n->kids.push_back(at); n->rmin = lo; n->rmax = hi; at = n;
            } else break;
        }
        return at;
    }
    Node* atom() {
        if (eat('(')) {
            if (i + 1 < s.size() && s[i] == '?' && s[i+1] == ':') i += 2;   // non-capturing
            Node* n = alt();
            if (!eat(')')) throw std::runtime_error("unclosed (");
            return n;
        }
        if (eat('[')) return cls();
        if (eat('.')) { Node* n = a.make(Op::Class); n->cls.set(); n->cls.reset('\n'); return n; }
        if (eat('^') || eat('$')) return a.make(Op::Empty);   // sound to ignore for indexing
        if (eat(char(92))) {
            if (eof()) throw std::runtime_error("trailing backslash");
            char c = s[i++];
            Node* n = a.make(Op::Class);
            switch (c) {
                case 'd': for (int k='0';k<='9';++k) n->cls.set(size_t(k)); return n;
                case 'w': for (int k=0;k<256;++k) if (std::isalnum(k)||k=='_') n->cls.set(size_t(k)); return n;
                case 's': for (int k : {' ','\t','\n','\r','\f','\v'}) n->cls.set(size_t(k)); return n;
                case 'n': return lit_node('\n');
                case 't': return lit_node('\t');
                default:  return lit_node(c);
            }
        }
        if (eof()) throw std::runtime_error("unexpected end of pattern");
        return lit_node(s[i++]);
    }
    Node* cls() {
        Node* n = a.make(Op::Class);
        bool neg = eat('^');
        bool first = true;
        while (!eof() && (peek() != ']' || first)) {
            first = false;
            unsigned char lo = (unsigned char)s[i++];
            if (lo == char(92) && !eof()) lo = (unsigned char)s[i++];
            if (!eof() && peek() == '-' && i + 1 < s.size() && s[i+1] != ']') {
                ++i;
                unsigned char hi = (unsigned char)s[i++];
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
    if (!p.eof()) throw std::runtime_error("trailing junk in pattern");
    return n;
}
