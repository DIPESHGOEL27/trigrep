#pragma once
#include "ast.hpp"
// Declared subset: literals, ., classes, ? * + {m,n}, alternation, groups,
// anchors. No backreferences, no lookaround -- neither is linear-time nor
// indexable, and that is a design decision stated up front.
Node* parse_regex(const std::string& pattern, Arena& a);   // throws std::runtime_error
