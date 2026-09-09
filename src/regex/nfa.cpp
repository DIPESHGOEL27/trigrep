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
        case Op::Class: { NState s{}; s.cls = nd->cls; int id = add(s); return {id, {{id, false}}}; }
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
