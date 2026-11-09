CC      ?= clang
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := $(wildcard zevic0/*.c)
OBJ     := $(patsubst zevic0/%.c,build/obj/%.o,$(SRC))
BIN     := build/zevic
PM      := build/zevpm
PMSRC   := $(wildcard zevpm/*.c) zevic0/util.c
VERSION := $(shell cat VERSION)

.PHONY: test-interop all test clean install zevpm2 zevfmt fmt fmt-check

all: $(BIN) $(PM)

build/obj/%.o: zevic0/%.c $(wildcard zevic0/*.h) VERSION
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -DZEVIC_VERSION='"$(VERSION)"' -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@

$(PM): $(PMSRC) $(wildcard zevpm/*.h) zevic0/util.h VERSION
	@mkdir -p build
	$(CC) $(CFLAGS) -DZEVPM_VERSION='"$(VERSION)"' $(PMSRC) -o $@

# zevpm written in Zevic (tools/zevpm), built by zevic
PM2     := build/zevpm2
tools/zevpm/src/Version.zev: VERSION
	@printf 'module Zevpm.Version\n\n// Generated from the file VERSION by `make` (do not edit).\nlet Current: String = "%s"\n' "$(VERSION)" > $@

$(PM2): $(BIN) $(wildcard tools/zevpm/src/*.zev) tools/zevpm/src/Version.zev $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(BIN) tools/zevpm/src/*.zev -o $@

zevpm2: $(PM2)

# the formatter, written in Zevic (tools/zevfmt)
FMT     := build/zevfmt
$(FMT): $(BIN) $(wildcard tools/zevfmt/src/*.zev) $(wildcard packages/Zevic/src/*.zev) runtime/zevic_rt.c
	./$(BIN) tools/zevfmt/src/*.zev -o $@

zevfmt: $(FMT)

FMT_DIRS := packages examples tools

fmt: $(FMT)
	./$(FMT) -w $(FMT_DIRS)

test-interop: $(BIN)
	sh examples/interop/run.sh $(BIN)

fmt-check: $(FMT)
	./$(FMT) -check $(FMT_DIRS)

# the test runner (tests/zevtest.c) is C too
TESTER  := build/zevtest
$(TESTER): tests/zevtest.c
	@mkdir -p build
	$(CC) -std=c11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -pthread $< -o $@

test: $(BIN) $(PM) $(PM2) $(FMT) $(TESTER)
	ZEVIC=$(BIN) ZEVPM=$(PM) ZEVFMT=$(FMT) $(TESTER) golden
	ZEVIC=$(BIN) ZEVPM=$(PM) ZEVFMT=$(FMT) $(TESTER) zevpm
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) zevpm
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) examples
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) zevfmt
	ZEVIC=$(BIN) ZEVPM=$(PM2) ZEVFMT=$(FMT) $(TESTER) install

clean:
	rm -rf build
