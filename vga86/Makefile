# vga86 - VGA card and monitors
#
# make                                  static + shared library
# make test                             unit tests (no other packages needed)
# make test-integration VM86_DIR=../vm86   tests with the vm86 CPU core
# make demo VM86_DIR=../vm86            runs the demos and writes screenshots (PGM)
# make font UNSCII_DIR=path/to/unscii/fontfiles   regenerates src/font_cp437.c

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
CFLAGS  += -Iinclude
LDLIBS  := -lm
BUILD   := build

LIB_SRC := src/vga.c src/monitor.c src/font_cp437.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)

ifeq ($(OS),Windows_NT)
  SHARED := $(BUILD)/vga86.dll
  EXE := .exe
else
  SHARED := $(BUILD)/libvga86.so
  EXE :=
endif

all: $(BUILD)/libvga86.a $(SHARED)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/vga.h include/monitor.h src/font_cp437.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libvga86.a: $(LIB_OBJ)
	ar rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

$(BUILD)/vga_test$(EXE): tests/vga_test.c $(BUILD)/libvga86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libvga86.a $(LDLIBS) -o $@

$(BUILD)/monitor_test$(EXE): tests/monitor_test.c $(BUILD)/libvga86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libvga86.a $(LDLIBS) -o $@

test: $(BUILD)/vga_test$(EXE) $(BUILD)/monitor_test$(EXE)
	$(BUILD)/vga_test$(EXE)
	$(BUILD)/monitor_test$(EXE)

# ---- targets that need the vm86 package ----

VM86_DIR ?=
VM86_SRC = $(VM86_DIR)/src/cpu.c $(VM86_DIR)/src/asm.c

check-vm86:
	@if [ -z "$(VM86_DIR)" ] || [ ! -f "$(VM86_DIR)/include/vm86.h" ]; then \
	  echo "Set VM86_DIR to the vm86 package, e.g. make $(MAKECMDGOALS) VM86_DIR=../vm86"; exit 1; fi

$(BUILD)/integration_test$(EXE): tests/integration_test.c $(BUILD)/libvga86.a | check-vm86
	$(CC) $(CFLAGS) -I$(VM86_DIR)/include $< $(VM86_SRC) $(BUILD)/libvga86.a $(LDLIBS) -o $@

test-integration: check-vm86 $(BUILD)/integration_test$(EXE)
	$(BUILD)/integration_test$(EXE)

$(BUILD)/vgarun$(EXE): examples/vm86_glue.c $(BUILD)/libvga86.a | check-vm86
	$(CC) $(CFLAGS) -I$(VM86_DIR)/include $< $(VM86_DIR)/src/cpu.c $(BUILD)/libvga86.a $(LDLIBS) -o $@

$(BUILD)/asm86$(EXE): | check-vm86 $(BUILD)
	$(CC) $(CFLAGS) -I$(VM86_DIR)/include $(VM86_DIR)/tools/asm86_cli.c $(VM86_SRC) -o $@

demo: check-vm86 $(BUILD)/vgarun$(EXE) $(BUILD)/asm86$(EXE)
	$(BUILD)/asm86$(EXE) examples/text_demo.asm -o $(BUILD)/text_demo.com
	$(BUILD)/asm86$(EXE) examples/gfx_demo.asm -o $(BUILD)/gfx_demo.com
	cd $(BUILD) && ./vgarun$(EXE) text_demo.com --frames 60 --every 15 --out text
	cd $(BUILD) && ./vgarun$(EXE) gfx_demo.com --frames 300 --every 30 --out gfx
	@echo "Screenshots written to $(BUILD)/*.pgm"

font:
	@if [ -z "$(UNSCII_DIR)" ]; then echo "Set UNSCII_DIR to unscii's fontfiles directory"; exit 1; fi
	python3 tools/gen_font.py $(UNSCII_DIR) > src/font_cp437.c

clean:
	rm -rf $(BUILD)

.PHONY: all test test-integration demo font clean check-vm86
