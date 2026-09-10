CXX      := g++
CXXFLAGS := -std=c++17 -O2 -march=native -Wall -Wextra -Isrc -MMD -MP -pthread
LIB      := $(wildcard src/*.cpp src/regex/*.cpp)
LIBOBJ   := $(LIB:%.cpp=build/%.o)
TOOLS    := count_trigrams trigrep_index trigrep_search
BINS     := $(TOOLS:%=build/%)

all: $(BINS)

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%: build/tools/%.o $(LIBOBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

check: all
	@bash tests/smoke.sh

clean:
	rm -rf build

.PRECIOUS: build/%.o build/tools/%.o
.PHONY: all check clean
-include $(shell find build -name '*.d' 2>/dev/null)
