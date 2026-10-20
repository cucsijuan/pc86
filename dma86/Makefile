# dma86 - 8237A DMA controller
#
# make        static + shared library
# make test   unit tests (no other packages needed)

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
CFLAGS  += -Iinclude
BUILD   := build

LIB_SRC := src/dma.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)

ifeq ($(OS),Windows_NT)
  SHARED := $(BUILD)/dma86.dll
  EXE := .exe
else
  SHARED := $(BUILD)/libdma86.so
  EXE :=
endif

all: $(BUILD)/libdma86.a $(SHARED)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/dma.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libdma86.a: $(LIB_OBJ)
	ar rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

$(BUILD)/dma_test$(EXE): tests/dma_test.c $(BUILD)/libdma86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libdma86.a -o $@

test: $(BUILD)/dma_test$(EXE)
	$(BUILD)/dma_test$(EXE)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
