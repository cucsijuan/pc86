# kbd86 - PC keyboard (8042 + keyboard BIOS) and 8259 PIC
#
# make                                     static + shared library
# make test                                unit tests (no other packages needed)
# make test-integration VM86_DIR=../vm86   tests with the vm86 CPU core

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
CFLAGS  += -Iinclude
BUILD   := build

LIB_SRC := src/kbd.c src/pic.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)

ifeq ($(OS),Windows_NT)
  SHARED := $(BUILD)/kbd86.dll
  EXE := .exe
else
  SHARED := $(BUILD)/libkbd86.so
  EXE :=
endif

all: $(BUILD)/libkbd86.a $(SHARED)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/kbd.h include/pic.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libkbd86.a: $(LIB_OBJ)
	ar rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

$(BUILD)/kbd_test$(EXE): tests/kbd_test.c $(BUILD)/libkbd86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libkbd86.a -o $@

test: $(BUILD)/kbd_test$(EXE)
	$(BUILD)/kbd_test$(EXE)

VM86_DIR ?=

check-vm86:
	@if [ -z "$(VM86_DIR)" ] || [ ! -f "$(VM86_DIR)/include/vm86.h" ]; then \
	  echo "Set VM86_DIR to the vm86 package, e.g. make $(MAKECMDGOALS) VM86_DIR=../vm86"; exit 1; fi

$(BUILD)/integration_test$(EXE): tests/integration_test.c $(BUILD)/libkbd86.a | check-vm86
	$(CC) $(CFLAGS) -I$(VM86_DIR)/include $< $(VM86_DIR)/src/cpu.c $(VM86_DIR)/src/asm.c $(BUILD)/libkbd86.a -o $@

test-integration: check-vm86 $(BUILD)/integration_test$(EXE)
	$(BUILD)/integration_test$(EXE)

clean:
	rm -rf $(BUILD)

.PHONY: all test test-integration clean check-vm86
