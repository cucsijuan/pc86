# vm86 - 8086 core + NASM-style assembler
# make            builds the static and shared libraries and the tools
# make test       quick tests (API, assembler vs NASM, errors, example programs)
# make test-cpu   validates the CPU against SingleStepTests 8088 (~700 MB download)

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
CFLAGS  += -Iinclude
BUILD   := build

LIB_SRC := src/cpu.c src/asm.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)

ifeq ($(OS),Windows_NT)
  SHARED := $(BUILD)/vm86.dll
  EXE := .exe
else
  SHARED := $(BUILD)/libvm86.so
  EXE :=
endif

all: $(BUILD)/libvm86.a $(SHARED) $(BUILD)/asm86$(EXE) $(BUILD)/run86$(EXE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/vm86.h include/asm86.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libvm86.a: $(LIB_OBJ)
	ar rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

$(BUILD)/asm86$(EXE): tools/asm86_cli.c $(BUILD)/libvm86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libvm86.a -o $@

$(BUILD)/run86$(EXE): tools/run86.c $(BUILD)/libvm86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libvm86.a -o $@

$(BUILD)/api_test$(EXE): tests/api_test.c $(BUILD)/libvm86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libvm86.a -o $@

test: all $(BUILD)/api_test$(EXE)
	$(BUILD)/api_test$(EXE)
	python3 tests/asm_vs_nasm.py
	python3 tests/asm_errors.py
	python3 tests/programs.py

test-cpu: $(SHARED)
	python3 tests/cpu_singlestep.py --lib $(SHARED)

clean:
	rm -rf $(BUILD)

.PHONY: all test test-cpu clean
