# SPDX-License-Identifier: BSD-2-Clause-Patent
SDK ?= macosx27.3
OUT ?= ../../../out/piano-durability/$(SDK)
CFLAGS ?= -O2 -Wall -Wextra -Werror

.PHONY: all test
all: $(OUT)/piano-durability

$(OUT)/piano-durability: piano-durability.c Makefile
	mkdir -p "$(OUT)"
	xcrun --sdk $(SDK) clang -arch arm64 -std=c11 -D_DARWIN_C_SOURCE $(CFLAGS) \
	    -framework CoreFoundation -framework IOKit $< -o "$@"

test:
	python3 -B test_durability.py
