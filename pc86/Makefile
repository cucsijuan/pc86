# pc86 - the motherboard: puts vm86, pit86, kbd86 and vga86 together
#
# make          build/libpc86.a and build/libpc86.so: the whole machine in one library
# make test     machine tests
# make demo     runs the examples on a full machine and writes screenshots (PGM)
#
# The other components are expected next to this one (override with *_DIR=...).

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
VM86_DIR ?= ../vm86
PIT86_DIR ?= ../pit86
KBD86_DIR ?= ../kbd86
VGA86_DIR ?= ../vga86
CFLAGS  += -Iinclude -I$(VM86_DIR)/include -I$(PIT86_DIR)/include -I$(KBD86_DIR)/include -I$(VGA86_DIR)/include
LDLIBS  := -lm
BUILD   := build

SRC := src/pc.c src/pc_vga.c \
       $(VM86_DIR)/src/cpu.c $(VM86_DIR)/src/asm.c \
       $(PIT86_DIR)/src/pit.c \
       $(KBD86_DIR)/src/kbd.c $(KBD86_DIR)/src/kbc.c $(KBD86_DIR)/src/pic.c \
       $(VGA86_DIR)/src/vga.c $(VGA86_DIR)/src/monitor.c $(VGA86_DIR)/src/font_cp437.c
OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(SRC)))
HDR := $(wildcard include/*.h $(VM86_DIR)/include/*.h $(PIT86_DIR)/include/*.h \
                  $(KBD86_DIR)/include/*.h $(VGA86_DIR)/include/*.h $(VGA86_DIR)/src/*.h)

vpath %.c src $(VM86_DIR)/src $(PIT86_DIR)/src $(KBD86_DIR)/src $(VGA86_DIR)/src

ifeq ($(OS),Windows_NT)
  SHARED := $(BUILD)/pc86.dll
  EXE := .exe
else
  SHARED := $(BUILD)/libpc86.so
  EXE :=
endif

all: $(BUILD)/libpc86.a $(SHARED) $(BUILD)/pcrun$(EXE)

$(BUILD)/obj:
	mkdir -p $(BUILD)/obj

$(BUILD)/obj/%.o: %.c $(HDR) | $(BUILD)/obj
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libpc86.a: $(OBJ)
	ar rcs $@ $^

$(SHARED): $(OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

$(BUILD)/pcrun$(EXE): tools/pcrun.c $(BUILD)/libpc86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libpc86.a $(LDLIBS) -o $@

$(BUILD)/pc_test$(EXE): tests/pc_test.c $(BUILD)/libpc86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libpc86.a $(LDLIBS) -o $@

$(BUILD)/asm86$(EXE): $(VM86_DIR)/tools/asm86_cli.c $(BUILD)/libpc86.a
	$(CC) $(CFLAGS) $< $(BUILD)/libpc86.a $(LDLIBS) -o $@

test: $(BUILD)/pc_test$(EXE)
	$(BUILD)/pc_test$(EXE)

demo: $(BUILD)/pcrun$(EXE) $(BUILD)/asm86$(EXE)
	$(BUILD)/asm86$(EXE) examples/clock.asm -o $(BUILD)/clock.com
	cd $(BUILD) && ./pcrun$(EXE) clock.com --seconds 3 --every 60 --out clock
	@echo "Screenshots written to $(BUILD)/*.pgm"

clean:
	rm -rf $(BUILD)

.PHONY: all test demo clean
