CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
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

.PHONY: build clean run test format format-check fetch-fastchess selfplay

build: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

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
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/test_%.o: $(TESTDIR)/test_%.cpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I$(CATCH2DIR) -I$(SRCDIR) -c -o $@ $<

$(CATCH2_OBJ): $(CATCH2DIR)/catch_amalgamated.cpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I$(CATCH2DIR) -c -o $@ $<

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

selfplay: build
	./scripts/selfplay.sh
