# AmiAtari2600 - host build (Linux/macOS) of the portable core + test harness.
# The Amiga build lives in Makefile.amiga (vbcc).

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c99 -pedantic
BUILD   := build

CORE_SRC := src/core/cpu.c src/core/bus.c src/core/tia.c src/core/riot.c \
            src/core/cart.c src/core/atari.c src/core/palette.c
HOST_SRC := src/host/main_host.c

.PHONY: all test clean m68k-test

all: $(BUILD)/a26host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/a26host: $(CORE_SRC) $(HOST_SRC) src/core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) $(HOST_SRC)

$(BUILD)/vidconv_test: tests/vidconv_test.c src/amiga/vidconv.c src/amiga/vidconv.h $(CORE_SRC) src/core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/vidconv_test.c src/amiga/vidconv.c $(CORE_SRC)

$(BUILD)/cpu_functional: tests/cpu_functional.c src/core/cpu.c src/core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/cpu_functional.c src/core/cpu.c

test: $(BUILD)/a26host $(BUILD)/cpu_functional $(BUILD)/vidconv_test
	sh tests/run_tests.sh

# Cross-check the core on a big-endian 68k CPU (needs m68k-linux-gnu-gcc + qemu-m68k).
m68k-test: | $(BUILD)
	m68k-linux-gnu-gcc -O2 -m68030 -static -std=c99 -o $(BUILD)/a26host.m68k $(CORE_SRC) $(HOST_SRC)
	m68k-linux-gnu-gcc -O2 -m68030 -static -std=c99 -o $(BUILD)/cpu_functional.m68k tests/cpu_functional.c src/core/cpu.c
	m68k-linux-gnu-gcc -O2 -m68030 -static -std=c99 -DA26_BIG_ENDIAN -o $(BUILD)/vidconv_test.m68k tests/vidconv_test.c src/amiga/vidconv.c $(CORE_SRC)
	A26HOST="qemu-m68k -cpu m68030 $(BUILD)/a26host.m68k" \
	CPUTEST="qemu-m68k -cpu m68030 $(BUILD)/cpu_functional.m68k" \
	VIDTEST="qemu-m68k -cpu m68030 $(BUILD)/vidconv_test.m68k" sh tests/run_tests.sh

clean:
	rm -rf $(BUILD)
