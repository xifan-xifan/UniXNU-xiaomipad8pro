# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano's pre-UEFI OS selector: one translation unit, no linker. -mcmodel=tiny with every symbol local and in .text lets the
# assembler resolve all references PC-relative; the build refuses an object with any relocation or a second allocated section.
#   make            -> $(OUT)/selector.bin (and selector.o)
#   make test       -> host tests of bootflag.h (the decision and the writers' codec) and of the built binary
LLVM ?= /opt/homebrew/opt/llvm/bin
CC := $(LLVM)/clang
OBJCOPY := $(LLVM)/llvm-objcopy
OBJDUMP := $(LLVM)/llvm-objdump
OUT ?= ../../out/selector
HOSTCC ?= cc
HOSTCFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

# MMU off = Device memory: no unaligned accesses, no FP/SIMD.
CFLAGS := --target=aarch64-none-elf -O2 -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -fno-pic -fno-jump-tables -mgeneral-regs-only -mstrict-align -mcmodel=tiny \
	-fno-zero-initialized-in-bss -Wall -Wextra -Werror

all: $(OUT)/selector.bin

$(OUT):
	mkdir -p $@
$(OUT)/selector.o: selector.c bootflag.h selector_fdt.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
	@if $(OBJDUMP) -r $@ | grep -q R_AARCH64; then $(OBJDUMP) -r $@; echo "selector.o has relocations" >&2; rm -f $@; exit 1; fi
	@n=$$($(OBJDUMP) -h $@ | awk '$$NF=="TEXT"||$$NF=="DATA"||$$NF=="BSS"' | wc -l | tr -d ' '); \
	  if [ "$$n" != 1 ]; then $(OBJDUMP) -h $@; echo "selector.o has $$n allocated sections, want 1 (.text)" >&2; rm -f $@; exit 1; fi
	@if $(OBJDUMP) -d $@ | grep -E '\s(ldr|str|ldp|stp)\s+q|\sv[0-9]+\.' >/dev/null; then echo "selector.o uses FP/SIMD" >&2; rm -f $@; exit 1; fi
$(OUT)/selector.bin: $(OUT)/selector.o
	$(OBJCOPY) -O binary -j .text $< $@

$(OUT)/test-bootflag: test/test-bootflag.c bootflag.h selector_fdt.h | $(OUT)
	$(HOSTCC) -std=c99 -Wall -Wextra -Werror $(HOSTCFLAGS) -I. $< -o $@

test: $(OUT)/test-bootflag $(OUT)/selector.bin
	$(OUT)/test-bootflag
	LLVM="$(LLVM)" python3 -I test/test-selector-bin.py $(OUT)/selector.o $(OUT)/selector.bin

clean:
	rm -rf $(OUT)
.PHONY: all test clean
