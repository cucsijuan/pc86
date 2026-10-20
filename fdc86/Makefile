# fdc86 - floppy controller (uPD765A), 3.5" drives, diskettes, FAT12
#
# make        static + shared library, build/fdimage
# make test   unit tests (no other packages needed)

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
CFLAGS  += -Iinclude
BUILD   := build

LIB_SRC := src/fdc.c src/fdc_bios.c src/fdd.c src/fat12.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)
HDR     := include/fdc.h include/fdd.h include/fat12.h src/fdc_internal.h

ifeq ($(OS),Windows_NT)
  SHARED := $(BUILD)/fdc86.dll
  EXE := .exe
else
  SHARED := $(BUILD)/libfdc86.so
  EXE :=
endif

all: $(BUILD)/libfdc86.a $(SHARED) $(BUILD)/fdimage$(EXE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libfdc86.a: $(LIB_OBJ)
	ar rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

$(BUILD)/fdimage$(EXE): tools/fdimage.c $(BUILD)/libfdc86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libfdc86.a -o $@

$(BUILD)/fdc_test$(EXE): tests/fdc_test.c $(BUILD)/libfdc86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libfdc86.a -o $@

test: $(BUILD)/fdc_test$(EXE)
	$(BUILD)/fdc_test$(EXE)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
