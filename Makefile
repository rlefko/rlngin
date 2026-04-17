CXX := g++

# Target architecture. Pick ``native`` for a local-machine build (picks up every
# extension the host CPU supports, including BMI2/PEXT when present) or select
# a specific tier for reproducible distributable binaries.
#
#   native         -- tune for the build host (default)
#   x86-64-v3      -- modern x86-64 baseline (Haswell/Zen2+): AVX2, BMI2, POPCNT
#   x86-64         -- portable x86-64 with POPCNT and SSSE3 bolted on
#   armv8          -- generic ARMv8-A (NEON is baseline)
#   apple-silicon  -- tuned for Apple M-series cores
ARCH ?= native

ifeq ($(ARCH),native)
ARCH_FLAGS := -march=native -mtune=native
else ifeq ($(ARCH),x86-64-v3)
ARCH_FLAGS := -march=x86-64-v3 -mtune=generic
else ifeq ($(ARCH),x86-64)
ARCH_FLAGS := -mpopcnt -mssse3
else ifeq ($(ARCH),armv8)
ARCH_FLAGS := -march=armv8-a
else ifeq ($(ARCH),apple-silicon)
ARCH_FLAGS := -mcpu=apple-m1
else
$(error Unknown ARCH '$(ARCH)'. Valid values: native, x86-64-v3, x86-64, armv8, apple-silicon)
endif

# Force the scalar magic-bitboard path even on a BMI2 host. Useful on Zen1/Zen2
# where PEXT is microcoded and runs slower than the magic multiply-shift lookup.
ifeq ($(NO_PEXT),1)
ARCH_FLAGS += -DNO_PEXT
endif

# LTO across TUs (default on). Override with ``LTO=off`` if a toolchain objects.
LTO ?= on
ifeq ($(LTO),on)
LTO_FLAGS := -flto
else
LTO_FLAGS :=
endif

# DEBUG swaps release flags for -O0 -g and keeps asserts live. NDEBUG and the
# exception/RTTI suppressions only belong in the release flavor.
DEBUG ?= off
ifeq ($(DEBUG),on)
CXXFLAGS := -std=c++17 -Wall -Wextra -O0 -g -pthread $(ARCH_FLAGS)
LDFLAGS :=
else
CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -DNDEBUG -fno-exceptions -fno-rtti \
            -pthread $(ARCH_FLAGS) $(LTO_FLAGS)
LDFLAGS := $(LTO_FLAGS)
endif

SRCDIR := src
BUILDDIR := build
TARGET := $(BUILDDIR)/rlngin

SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))

TESTDIR := tests
CATCH2DIR := $(TESTDIR)/catch2
TEST_TARGET := $(BUILDDIR)/test_rlngin
TEST_SRCS := $(wildcard $(TESTDIR)/test_*.cpp)
TEST_OBJS := $(patsubst $(TESTDIR)/%.cpp,$(BUILDDIR)/%.o,$(TEST_SRCS))
CATCH2_OBJ := $(BUILDDIR)/catch_amalgamated.o
LIB_OBJS := $(filter-out $(BUILDDIR)/main.o,$(OBJS))

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
FASTCHESS_ASSET := fastchess-mac-arm64
else
FASTCHESS_ASSET := fastchess-linux-x86-64
endif
FASTCHESS_URL := https://github.com/Disservin/fastchess/releases/latest/download/$(FASTCHESS_ASSET).tar

CATCH2_VERSION := 3.8.0
CATCH2_URL := https://raw.githubusercontent.com/catchorg/Catch2/v$(CATCH2_VERSION)/extras

OPENINGS_URL := https://github.com/official-stockfish/books/raw/master/UHO_Lichess_4852_v1.epd.zip
OPENINGS_DIR := openings
OPENINGS_FILE := $(OPENINGS_DIR)/UHO_Lichess_4852_v1.epd

.PHONY: build clean run test format format-check fetch-catch2 fetch-fastchess fetch-openings selfplay

build: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)

run: build
	./$(TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJS) $(CATCH2_OBJ) $(LIB_OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

$(BUILDDIR)/test_%.o: $(TESTDIR)/test_%.cpp $(CATCH2DIR)/catch_amalgamated.hpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I$(CATCH2DIR) -I$(SRCDIR) -c -o $@ $<

$(CATCH2_OBJ): $(CATCH2DIR)/catch_amalgamated.cpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I$(CATCH2DIR) -c -o $@ $<

$(CATCH2DIR)/catch_amalgamated.hpp $(CATCH2DIR)/catch_amalgamated.cpp:
	@mkdir -p $(CATCH2DIR)
	@echo "Downloading Catch2 v$(CATCH2_VERSION)..."
	@curl -sL "$(CATCH2_URL)/catch_amalgamated.hpp" -o $(CATCH2DIR)/catch_amalgamated.hpp
	@curl -sL "$(CATCH2_URL)/catch_amalgamated.cpp" -o $(CATCH2DIR)/catch_amalgamated.cpp

fetch-catch2: $(CATCH2DIR)/catch_amalgamated.hpp $(CATCH2DIR)/catch_amalgamated.cpp

format:
	@find src tests -name '*.cpp' -o -name '*.h' | grep -v catch2 | xargs clang-format -i

format-check:
	@find src tests -name '*.cpp' -o -name '*.h' | grep -v catch2 | xargs clang-format --dry-run --Werror

fetch-fastchess:
	@echo "Downloading fastchess..."
	@curl -sL "$(FASTCHESS_URL)" -o /tmp/fastchess.tar
	@tar xf /tmp/fastchess.tar -C /tmp
	@cp /tmp/$(FASTCHESS_ASSET)/fastchess ./fastchess
	@chmod +x ./fastchess
	@rm -rf /tmp/fastchess.tar /tmp/$(FASTCHESS_ASSET)
	@echo "fastchess downloaded to ./fastchess"

fetch-openings: $(OPENINGS_FILE)

$(OPENINGS_FILE):
	@mkdir -p $(OPENINGS_DIR)
	@echo "Downloading opening book..."
	@curl -sL "$(OPENINGS_URL)" -o $(OPENINGS_DIR)/openings.zip
	@unzip -o -q $(OPENINGS_DIR)/openings.zip -d $(OPENINGS_DIR)
	@rm -f $(OPENINGS_DIR)/openings.zip
	@echo "Opening book saved to $(OPENINGS_FILE)"

selfplay: build
	./scripts/selfplay.sh
