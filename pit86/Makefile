# pit86 - 8253/8254 programmable interval timer
#
# make        static + shared library
# make test   unit tests (no other packages needed)

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
CFLAGS  += -Iinclude
BUILD   := build

LIB_SRC := src/pit.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o)

ifeq ($(OS),Windows_NT)
  SHARED := $(BUILD)/pit86.dll
  EXE := .exe
else
  SHARED := $(BUILD)/libpit86.so
  EXE :=
endif

all: $(BUILD)/libpit86.a $(SHARED)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/pit.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libpit86.a: $(LIB_OBJ)
	ar rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

$(BUILD)/pit_test$(EXE): tests/pit_test.c $(BUILD)/libpit86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libpit86.a -o $@

test: $(BUILD)/pit_test$(EXE)
	$(BUILD)/pit_test$(EXE)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
