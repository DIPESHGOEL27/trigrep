CXX      := g++
B        ?= build
CXXFLAGS ?= -std=c++17 -O2 -march=native -Wall -Wextra -Isrc -MMD -MP -pthread
SANFLAGS := -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
            -fno-sanitize-recover=all -Wall -Wextra -Isrc -MMD -MP -pthread
LIB      := $(wildcard src/*.cpp src/regex/*.cpp)
LIBOBJ   := $(LIB:%.cpp=$(B)/%.o)
TOOLS    := count_trigrams trigrep_index trigrep_search
BINS     := $(TOOLS:%=$(B)/%)

all: $(BINS)

$(B)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/%: $(B)/tools/%.o $(LIBOBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

check: all
	@BIN_DIR=$(B) bash tests/smoke.sh

# The same smoke test under AddressSanitizer + UndefinedBehaviorSanitizer, in
# a separate build directory so it never mixes objects with the -O2 build.
sanitize:
	@$(MAKE) --no-print-directory B=build-san CXXFLAGS="$(SANFLAGS)" check

clean:
	rm -rf build build-san

.PRECIOUS: $(B)/%.o $(B)/tools/%.o
.PHONY: all check sanitize clean
-include $(shell find $(B) -name '*.d' 2>/dev/null)
