#pragma once
#include "common.hpp"
#include "index.hpp"
#include <vector>

// Galloping (exponential) search intersection of two decoded lists.
void intersect_gallop(const std::vector<DocId>& a, const std::vector<DocId>& b,
                      std::vector<DocId>& out);

// Intersect a short decoded list against a long list that is NOT decoded:
// each element of `small` seeks the cursor via skip blocks, so the long list
// costs O(|small| * (log nskips + 128)) instead of O(|long|) to decode.
void intersect_cursor(const std::vector<DocId>& small, PostingCursor& c,
                      std::vector<DocId>& out);
