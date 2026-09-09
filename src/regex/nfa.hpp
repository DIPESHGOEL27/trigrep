#pragma once
#include "ast.hpp"
#include <vector>
#include <bitset>

// split == true  -> epsilon state, follows `next` and `alt`
// split == false -> consuming state, follows `next` on any byte in `cls`
struct NState {
    std::bitset<256> cls;
    int  next = -1;
    int  alt  = -1;
    bool split = false;
    bool accept = false;
};
struct Nfa { std::vector<NState> st; int start = 0; };

Nfa build_nfa(const Node* root);
