# pc86 suite - builds and tests every component
#
# make              build all libraries and tools
# make test         unit + integration tests of every component
# make demo         vga86 demos (writes PGM screenshots to vga86/build)
# make clean

COMPONENTS := vm86 vga86 kbd86
VM86_DIR   := $(CURDIR)/vm86

all:
	@for c in $(COMPONENTS); do $(MAKE) -C $$c all || exit 1; done

test: all
	$(MAKE) -C vm86 test
	$(MAKE) -C vga86 test test-integration VM86_DIR=$(VM86_DIR)
	$(MAKE) -C kbd86 test test-integration VM86_DIR=$(VM86_DIR)

demo:
	$(MAKE) -C vga86 demo VM86_DIR=$(VM86_DIR)

clean:
	@for c in $(COMPONENTS); do $(MAKE) -C $$c clean; done

.PHONY: all test demo clean
