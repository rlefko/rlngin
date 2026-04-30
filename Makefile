CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread

GIT_SHA := $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_DATE := $(shell git log -1 --format=%cd --date=format:%Y%m%d 2>/dev/null)
ifeq ($(strip $(GIT_SHA)),)
ENGINE_VERSION := dev
else
ENGINE_VERSION := dev-$(GIT_DATE)-$(GIT_SHA)
endif
CXXFLAGS += '-DENGINE_VERSION="$(ENGINE_VERSION)"'

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

FASTCHESS_REPO := https://github.com/Disservin/fastchess.git

CATCH2_VERSION := 3.8.0
CATCH2_URL := https://raw.githubusercontent.com/catchorg/Catch2/v$(CATCH2_VERSION)/extras

OPENINGS_URL := https://github.com/official-stockfish/books/raw/master/UHO_Lichess_4852_v1.epd.zip
OPENINGS_DIR := openings
OPENINGS_FILE := $(OPENINGS_DIR)/UHO_Lichess_4852_v1.epd

TUNE_TARGET := $(BUILDDIR)/tune
TUNE_SRC := tools/tune.cpp
TUNE_OBJ := $(BUILDDIR)/tune.o

.PHONY: build clean run test format format-check fetch-catch2 fetch-fastchess fetch-openings selfplay tune spsa texel-selfplay texel-extract texel-tune texel texel-bg texel-stop texel-status texel-dump texel-apply

# Texel pipeline tunables. Override on the command line, e.g.:
#   make texel-bg ROUNDS=10000 LIMIT=1+0.08 TUNE_PASSES=50
ROUNDS ?= 32000
LIMIT ?= nodes=100000
CONCURRENCY ?= 12
TUNE_THREADS ?= 14
TUNE_PASSES ?= 30

build: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

tune: $(TUNE_TARGET)

$(TUNE_TARGET): $(TUNE_OBJ) $(LIB_OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(TUNE_OBJ): $(TUNE_SRC)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)

run: build
	./$(TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJS) $(CATCH2_OBJ) $(LIB_OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

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
	@echo "Building fastchess from source..."
	@rm -rf /tmp/fastchess-src
	@git clone --depth 1 "$(FASTCHESS_REPO)" /tmp/fastchess-src
	@$(MAKE) -C /tmp/fastchess-src -j
	@cp /tmp/fastchess-src/fastchess ./fastchess
	@chmod +x ./fastchess
	@rm -rf /tmp/fastchess-src
	@echo "fastchess built to ./fastchess"

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

spsa: build
	@mkdir -p tuning/spsa
	python3 tools/spsa/spsa.py \
	    --iterations 300 \
	    --concurrency 6 \
	    --tc 10+0.1 \
	    --output-dir tuning/spsa \
	    --engine ./build/rlngin \
	    --fastchess ./fastchess \
	    --openings $(OPENINGS_FILE) \
	    --seed 1

texel-selfplay: build
	./scripts/texel_selfplay.sh

texel-extract:
	./scripts/texel_extract.sh

texel-tune: $(TUNE_TARGET)
	./scripts/texel_tune.sh

# Full Texel pipeline: self-play -> extract -> tune. Defaults match the
# overnight schedule (32k pairs, nodes=100000, concurrency 12, 14 tune
# threads, 30 passes, refit-K every 4, refresh-leaves every 8). Stages
# skip when their output exists; pass FORCE=1 to redo from scratch.
# Override any setting on the command line, e.g.:
#   make texel-bg ROUNDS=10000 LIMIT=1+0.08 TUNE_PASSES=50
texel: $(TARGET) $(TUNE_TARGET)
	./scripts/texel_pipeline.sh $(ROUNDS) $(LIMIT) $(CONCURRENCY) $(TUNE_THREADS) $(TUNE_PASSES)

# Same as `texel` but launched detached. Survives a closed shell. Writes
# tuning/texel/pipeline.pid for status / stop targets.
texel-bg: $(TARGET) $(TUNE_TARGET)
	@./scripts/texel_bg.sh $(ROUNDS) $(LIMIT) $(CONCURRENCY) $(TUNE_THREADS) $(TUNE_PASSES)

# Stop the running pipeline and every descendant (self-play, extract,
# tuner). Sweeps orphan workers pinned to tuning/texel/.
texel-stop:
	@./scripts/texel_stop.sh

# Show pipeline state, active workers, and per-stage progress.
texel-status:
	@./scripts/texel_status.sh

# Print the current tuned-eval snapshot from tuning/checkpoint.txt as a
# kDefaultEvalParams literal. Useful for previewing before applying.
texel-dump: $(TUNE_TARGET)
	@$(TUNE_TARGET) --dump tuning/checkpoint.txt

# Apply the current tuned snapshot to src/eval_params.cpp in place. Does
# not commit; review with `git diff` and commit when ready.
texel-apply: $(TUNE_TARGET)
	@./scripts/texel_apply_checkpoint.sh

