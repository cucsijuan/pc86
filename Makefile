# pc86 suite - builds and tests every component
#
# make              build all libraries and tools
# make test         unit + integration + machine tests of every component
# make demo         demos (screenshots in vga86/build and pc86/build)
# make clean

COMPONENTS := vm86 pit86 kbd86 vga86 pc86
VM86_DIR   := $(CURDIR)/vm86

all:
	@for c in $(COMPONENTS); do $(MAKE) -C $$c all || exit 1; done

test: all
	$(MAKE) -C vm86 test
	$(MAKE) -C pit86 test
	$(MAKE) -C kbd86 test test-integration VM86_DIR=$(VM86_DIR)
	$(MAKE) -C vga86 test test-integration VM86_DIR=$(VM86_DIR)
	$(MAKE) -C pc86 test

demo:
	$(MAKE) -C vga86 demo VM86_DIR=$(VM86_DIR)
	$(MAKE) -C pc86 demo

clean:
	@for c in $(COMPONENTS); do $(MAKE) -C $$c clean; done

.PHONY: all test demo clean
