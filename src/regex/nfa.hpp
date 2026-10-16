#pragma once
#include "ast.hpp"
#include <vector>
#include <bitset>

// split == true      -> epsilon state, follows `next` and `alt`
// assert_bol / _eol   -> epsilon state that follows `next` only at the start /
//                        end of a line (multi-line ^ and $, as code search uses)
// otherwise           -> consuming state, follows `next` on any byte in `cls`
struct NState {
    std::bitset<256> cls;
    int  next = -1;
    int  alt  = -1;
    bool split = false;
    bool accept = false;
    bool assert_bol = false;
    bool assert_eol = false;
};
struct Nfa { std::vector<NState> st; int start = 0; };

// Hard ceiling on NFA size. {m,n} is expanded by copying, so nested repeats
// multiply: (((ab){512}){512}){512} would need 268M states. build_nfa throws
// std::length_error past this, and the caller reports a pattern error.
static constexpr size_t kMaxNfaStates = 1u << 20;

Nfa build_nfa(const Node* root);
