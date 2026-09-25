# AmiAtari2600 - host build (Linux/macOS) of the portable core + test harness.
# The Amiga build lives in Makefile.amiga (vbcc).

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c99 -pedantic
BUILD   := build

CORE_SRC := src/core/cpu.c src/core/bus.c src/core/tia.c src/core/riot.c \
            src/core/cart.c src/core/atari.c src/core/palette.c
HOST_SRC := src/host/main_host.c

.PHONY: all test clean m68k-build m68k-test m68k-profile

all: $(BUILD)/a26host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/a26host: $(CORE_SRC) $(HOST_SRC) src/core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) $(HOST_SRC)

$(BUILD)/vidconv_test: tests/vidconv_test.c src/amiga/vidconv.c src/amiga/vidconv.h $(CORE_SRC) src/core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/vidconv_test.c src/amiga/vidconv.c $(CORE_SRC)

$(BUILD)/tia_equiv: tests/tia_equiv.c $(CORE_SRC) src/core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -DA26_TIA_REFERENCE -o $@ tests/tia_equiv.c $(CORE_SRC)

$(BUILD)/cpu_functional: tests/cpu_functional.c src/core/cpu.c src/core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/cpu_functional.c src/core/cpu.c

test: $(BUILD)/a26host $(BUILD)/cpu_functional $(BUILD)/vidconv_test $(BUILD)/tia_equiv
	sh tests/run_tests.sh

# Cross-check the core on a big-endian 68k CPU (needs m68k-linux-gnu-gcc,
# qemu-m68k and vasmm68k_mot): runs the whole test suite twice, with the C
# CPU core and with the 68k assembler core (src/amiga/cpu6507.s).
M68K_CC    := m68k-linux-gnu-gcc -O2 -m68030 -static -std=c99 -Wl,-z,noexecstack
VASM       ?= vasmm68k_mot
QEMU       := qemu-m68k -cpu m68030

$(BUILD)/cpu6507.elf.o: src/amiga/cpu6507.s | $(BUILD)
	$(VASM) -m68030 -Felf -quiet -o $@ $<

m68k-build: $(BUILD)/cpu6507.elf.o
	$(M68K_CC) -o $(BUILD)/a26host.m68k $(CORE_SRC) $(HOST_SRC)
	$(M68K_CC) -DA26_ASM_CPU -o $(BUILD)/a26host_asm.m68k $(CORE_SRC) src/core/cpu_asm.c $(HOST_SRC) $(BUILD)/cpu6507.elf.o
	$(M68K_CC) -o $(BUILD)/cpu_functional.m68k tests/cpu_functional.c src/core/cpu.c
	$(M68K_CC) -DA26_ASM_CPU -o $(BUILD)/cpu_functional_asm.m68k tests/cpu_functional.c src/core/cpu.c src/core/cpu_asm.c $(BUILD)/cpu6507.elf.o
	$(M68K_CC) -DA26_BIG_ENDIAN -o $(BUILD)/vidconv_test.m68k tests/vidconv_test.c src/amiga/vidconv.c $(CORE_SRC)
	$(M68K_CC) -DA26_TIA_REFERENCE -o $(BUILD)/tia_equiv.m68k tests/tia_equiv.c $(CORE_SRC)

m68k-test: m68k-build
	@echo "######## 68030, C CPU core"
	A26HOST="$(QEMU) $(BUILD)/a26host.m68k" CPUTEST="$(QEMU) $(BUILD)/cpu_functional.m68k" \
	VIDTEST="$(QEMU) $(BUILD)/vidconv_test.m68k" TIATEST="$(QEMU) $(BUILD)/tia_equiv.m68k" sh tests/run_tests.sh
	@echo "######## 68030, assembler CPU core"
	A26HOST="$(QEMU) $(BUILD)/a26host_asm.m68k" CPUTEST="$(QEMU) $(BUILD)/cpu_functional_asm.m68k" \
	VIDTEST="$(QEMU) $(BUILD)/vidconv_test.m68k" TIATEST="$(QEMU) $(BUILD)/tia_equiv.m68k" sh tests/run_tests.sh
	@for r in bars_ntsc busy_ntsc bank_f8; do \
	  $(QEMU) $(BUILD)/a26host.m68k $(BUILD)/$$r.bin -frames 30 -ppm $(BUILD)/c_$$r.ppm -q >/dev/null; \
	  $(QEMU) $(BUILD)/a26host_asm.m68k $(BUILD)/$$r.bin -frames 30 -ppm $(BUILD)/a_$$r.ppm -q >/dev/null; \
	  cmp -s $(BUILD)/c_$$r.ppm $(BUILD)/a_$$r.ppm && echo "  ok   $$r: C and asm core render identical frames" \
	    || { echo "  FAIL $$r: C and asm core differ"; exit 1; }; \
	done

# 68k instructions per emulated frame, per function (see tools/qemu_icount.py).
# PROFILE_ROM=path/to/rom.bin to profile a real game.
PROFILE_ROM ?= $(BUILD)/busy_ntsc.bin
m68k-profile: m68k-build
	@for v in a26host a26host_asm; do \
	  a=$$(tools/qemu_icount.py $(BUILD)/$$v.m68k $(PROFILE_ROM) -frames 10 -q); \
	  b=$$(tools/qemu_icount.py $(BUILD)/$$v.m68k $(PROFILE_ROM) -frames 30 -q); \
	  echo "$$v: $$(( (b - a) / 20 )) 68k instructions per frame"; \
	done
	@echo "--- per function (asm core, 30 frames incl. startup):"
	@tools/qemu_icount.py --by-func $(BUILD)/a26host_asm.m68k $(PROFILE_ROM) -frames 30 -q | head -16

clean:
	rm -rf $(BUILD)
