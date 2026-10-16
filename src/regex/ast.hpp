#pragma once
#include <string>
#include <vector>
#include <memory>
#include <bitset>

// Bol / Eol are the multi-line anchors ^ and $: they consume nothing, so for
// trigram synthesis they behave exactly like Empty, but the verifier honours them.
enum class Op { Empty, Literal, Class, Concat, Alt, Star, Plus, Quest, Repeat, Bol, Eol };

struct Node {
    Op op = Op::Empty;
    std::string lit;                 // Op::Literal, possibly multi-byte
    std::bitset<256> cls;            // Op::Class
    std::vector<Node*> kids;
    int rmin = 0, rmax = 0;          // Op::Repeat; rmax < 0 means unbounded
};

// Arena: the AST has one allocation lifetime; nodes are never freed singly.
struct Arena {
    std::vector<std::unique_ptr<Node>> pool;
    Node* make(Op o) {
        pool.push_back(std::make_unique<Node>());
        pool.back()->op = o;
        return pool.back().get();
    }
};
