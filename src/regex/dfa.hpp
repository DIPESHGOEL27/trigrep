#pragma once
#include "nfa.hpp"
#include <array>
#include <vector>
#include <string>
#include <unordered_map>

// Subset construction materialised ON DEMAND. Full determinisation can be
// exponential in the pattern, so states are built only as the input asks for
// them and the cache is flushed rather than allowed to grow without bound.
// Cost is O(1) amortised per input byte regardless of pattern, so an
// adversarial (a+)+b cannot hang the service -- the same choice RE2 makes.
class LazyDfa {
public:
    explicit LazyDfa(const Nfa& n);
    bool search(const std::string& text);      // unanchored: any position matches?
    size_t states() const { return d_.size(); }
private:
    struct DState {
        std::vector<int> set;                  // sorted consuming-state ids
        bool accept = false;
        std::array<int, 256> tr;               // -2 = not yet computed
    };
    int intern(std::vector<int> set, bool accept);
    int step(int from, unsigned char c);
    void closure(int s, std::vector<int>& out, std::vector<char>& mark, bool& accept) const;

    const Nfa& nfa_;
    std::vector<DState> d_;
    std::unordered_map<std::string, int> lookup_;
    std::vector<int> start_set_;
    bool start_accept_ = false;
    int  start_ = 0;
    static constexpr size_t kMaxStates = 20000;
};
