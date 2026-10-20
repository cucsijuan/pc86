# workbench - the pc86 machine in a web page
#
# make              build/workbench.wasm and index.html (needs wasi-sdk)
# make clean
#
# WASI_SDK points at an extracted wasi-sdk release
# (https://github.com/WebAssembly/wasi-sdk/releases).
# SW86 is the sw86 checkout: with it, the page can make a DOS86 diskette.

WASI_SDK ?= /opt/wasi-sdk
ROOT     := ..
SW86     ?= $(ROOT)/../sw86
BUILD    := build
CC       := $(WASI_SDK)/bin/clang
CFLAGS   := --target=wasm32-wasip1 -mexec-model=reactor -O2 -std=c99 -Wall -Wextra -pedantic \
            -I$(ROOT)/vm86/include -I$(ROOT)/pit86/include -I$(ROOT)/kbd86/include \
            -I$(ROOT)/vga86/include -I$(ROOT)/pc86/include -I$(ROOT)/dma86/include -I$(ROOT)/fdc86/include

SRC := src/web.c \
       $(ROOT)/pc86/src/pc.c $(ROOT)/pc86/src/pc_vga.c $(ROOT)/pc86/src/pc_fdc.c \
       $(ROOT)/vm86/src/cpu.c $(ROOT)/vm86/src/asm.c \
       $(ROOT)/pit86/src/pit.c \
       $(ROOT)/dma86/src/dma.c \
       $(ROOT)/kbd86/src/kbd.c $(ROOT)/kbd86/src/kbc.c $(ROOT)/kbd86/src/pic.c \
       $(ROOT)/vga86/src/vga.c $(ROOT)/vga86/src/monitor.c $(ROOT)/vga86/src/font_cp437.c \
       $(ROOT)/fdc86/src/fdc.c $(ROOT)/fdc86/src/fdc_bios.c $(ROOT)/fdc86/src/fdd.c $(ROOT)/fdc86/src/fat12.c

HEADERS := $(wildcard $(ROOT)/*/include/*.h $(ROOT)/vga86/src/*.h $(ROOT)/fdc86/src/*.h)
EXAMPLES := $(wildcard $(ROOT)/*/examples/*.asm examples/*.asm)
DOS86    := $(wildcard $(SW86)/dos/boot.asm $(SW86)/dos/dos86.asm $(SW86)/dos/command.asm)

all: index.html

$(BUILD)/workbench.wasm: $(SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) -lm -Wl,--strip-all -o $@

index.html: shell.html tools/embed.py $(BUILD)/workbench.wasm $(EXAMPLES) $(DOS86)
	python3 tools/embed.py shell.html $(BUILD)/workbench.wasm $(ROOT) $@ $(if $(DOS86),--sw86 $(SW86))

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
