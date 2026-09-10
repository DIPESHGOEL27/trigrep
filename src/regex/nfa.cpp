#include "nfa.hpp"

namespace {
// Dangling exits are recorded as (state index, which field) pairs, NOT as
// int* into the state vector: the vector reallocates as states are added and
// raw pointers would dangle.
struct Out { int state; bool is_alt; };
struct Frag { int entry; std::vector<Out> outs; };

struct B {
    Nfa n;
    int add(const NState& s) { n.st.push_back(s); return int(n.st.size()) - 1; }
    int add_split() { NState s{}; s.split = true; return add(s); }
    void patch(const std::vector<Out>& outs, int target) {
        for (const Out& o : outs) (o.is_alt ? n.st[size_t(o.state)].alt
                                            : n.st[size_t(o.state)].next) = target;
    }
    static Frag join(Frag a, Frag b, B& self) {
        self.patch(a.outs, b.entry);
        return Frag{a.entry, b.outs};
    }

    Frag go(const Node* nd) {
        switch (nd->op) {
        case Op::Empty: { int s = add_split(); return {s, {{s, false}}}; }
        case Op::Literal: {
            int entry = -1, prev = -1;
            for (unsigned char c : nd->lit) {
                NState s{}; s.cls.set(c);
                int id = add(s);
                if (entry < 0) entry = id; else n.st[size_t(prev)].next = id;
                prev = id;
            }
            if (entry < 0) { int s = add_split(); return {s, {{s, false}}}; }
            return {entry, {{prev, false}}};
        }
        case Op::Class: {
            // Byte-vs-code-point semantics. RE2 (and ripgrep) match `.` and
            // negated classes against ONE UTF-8 code point; a naive byte
            // automaton matches ONE byte, so "a.m" fails on "aßm" (ß = C3 9F).
            // The differential oracle caught exactly this (BUGLOG #8). Fix as
            // RE2 does: a class that admits any byte >= 0x80 is compiled into
            // the UTF-8 sequence automaton -- ASCII | 2-byte | 3-byte | 4-byte
            // -- so it consumes a whole well-formed code point.
            bool high = false;
            for (int c = 0x80; c < 256; ++c) if (nd->cls.test(size_t(c))) { high = true; break; }
            if (!high) { NState s{}; s.cls = nd->cls; int id = add(s); return {id, {{id, false}}}; }

            auto range = [&](int lo, int hi) { NState s{}; for (int c = lo; c <= hi; ++c) s.cls.set(size_t(c)); return add(s); };
            auto cont  = [&]() { return range(0x80, 0xBF); };
            std::vector<Out> outs;
            // Branch 1: ASCII bytes the class actually admits.
            NState a{}; for (int c = 0; c < 0x80; ++c) if (nd->cls.test(size_t(c))) a.cls.set(size_t(c));
            int b1 = add(a); outs.push_back({b1, false});
            // Branch 2: 2-byte sequences  C2..DF 80..BF
            int b2 = range(0xC2, 0xDF); int c2 = cont(); n.st[size_t(b2)].next = c2; outs.push_back({c2, false});
            // Branch 3: 3-byte sequences  E0..EF 80..BF 80..BF
            int b3 = range(0xE0, 0xEF); int c3a = cont(); int c3b = cont();
            n.st[size_t(b3)].next = c3a; n.st[size_t(c3a)].next = c3b; outs.push_back({c3b, false});
            // Branch 4: 4-byte sequences  F0..F4 80..BF 80..BF 80..BF
            int b4 = range(0xF0, 0xF4); int c4a = cont(); int c4b = cont(); int c4c = cont();
            n.st[size_t(b4)].next = c4a; n.st[size_t(c4a)].next = c4b; n.st[size_t(c4b)].next = c4c; outs.push_back({c4c, false});
            // Fan-in: a chain of epsilon splits selecting one of the four branches.
            int s3 = add_split(); n.st[size_t(s3)].next = b3; n.st[size_t(s3)].alt = b4;
            int s2 = add_split(); n.st[size_t(s2)].next = b2; n.st[size_t(s2)].alt = s3;
            int s1 = add_split(); n.st[size_t(s1)].next = b1; n.st[size_t(s1)].alt = s2;
            return {s1, std::move(outs)};
        }
        case Op::Concat: {
            Frag acc = go(nd->kids[0]);
            for (size_t k = 1; k < nd->kids.size(); ++k) acc = join(acc, go(nd->kids[k]), *this);
            return acc;
        }
        case Op::Alt: {
            Frag acc = go(nd->kids[0]);
            for (size_t k = 1; k < nd->kids.size(); ++k) {
                Frag b = go(nd->kids[k]);
                int s = add_split();
                n.st[size_t(s)].next = acc.entry;
                n.st[size_t(s)].alt  = b.entry;
                std::vector<Out> outs = acc.outs;
                outs.insert(outs.end(), b.outs.begin(), b.outs.end());
                acc = Frag{s, std::move(outs)};
            }
            return acc;
        }
        case Op::Star: {
            Frag b = go(nd->kids[0]);
            int s = add_split();
            n.st[size_t(s)].next = b.entry;
            patch(b.outs, s);                       // loop back
            return {s, {{s, true}}};                // exit via alt
        }
        case Op::Plus: {
            Frag b = go(nd->kids[0]);
            int s = add_split();
            n.st[size_t(s)].next = b.entry;
            patch(b.outs, s);
            return {b.entry, {{s, true}}};          // must consume once
        }
        case Op::Quest: {
            Frag b = go(nd->kids[0]);
            int s = add_split();
            n.st[size_t(s)].next = b.entry;
            std::vector<Out> outs = b.outs;
            outs.push_back({s, true});              // skip branch
            return {s, std::move(outs)};
        }
        case Op::Repeat: {
            const Node* child = nd->kids[0];
            int lo = nd->rmin, hi = nd->rmax;
            Frag acc{-1, {}};
            bool have = false;
            for (int k = 0; k < lo; ++k) {
                Frag f = go(child);
                if (!have) { acc = f; have = true; } else acc = join(acc, f, *this);
            }
            if (hi < 0) {                            // {m,} == m copies then a star
                Frag b = go(child);
                int s = add_split();
                n.st[size_t(s)].next = b.entry;
                patch(b.outs, s);
                Frag star{s, {{s, true}}};
                acc = have ? join(acc, star, *this) : star;
                have = true;
            } else {
                for (int k = lo; k < hi; ++k) {       // optional tail copies
                    Frag b = go(child);
                    int s = add_split();
                    n.st[size_t(s)].next = b.entry;
                    std::vector<Out> outs = b.outs;
                    outs.push_back({s, true});
                    Frag q{s, std::move(outs)};
                    acc = have ? join(acc, q, *this) : q;
                    have = true;
                }
            }
            if (!have) { int s = add_split(); return {s, {{s, false}}}; }
            return acc;
        }
        }
        int s = add_split(); return {s, {{s, false}}};
    }
};
} // namespace

Nfa build_nfa(const Node* root) {
    B b;
    Frag f = b.go(root);
    NState acc{}; acc.accept = true; acc.split = true;
    int a = b.add(acc);
    b.patch(f.outs, a);
    b.n.start = f.entry;
    return b.n;
}
