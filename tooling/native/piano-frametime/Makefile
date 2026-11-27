# SPDX-License-Identifier: MIT
CC = xcrun --sdk macosx clang
SDK = macosx27.3
MACOS_SDK ?= $(shell xcrun --sdk $(SDK) --show-sdk-path)
OUT = ../../../out/piano-frametime
CFLAGS = -O2 -Wall -Wextra -Werror
FRAMEWORKS = -framework AppKit -framework CoreGraphics -framework IOKit -framework Metal -framework QuartzCore
HOST_CC ?= cc
PYTHON ?= python3
HOST_TEST_FLAGS = -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all

.PHONY: all test
all:
	test -n "$(MACOS_SDK)" && test -d "$(MACOS_SDK)"
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -arch arm64 -isysroot "$(MACOS_SDK)" -mmacosx-version-min=14.0 -fobjc-arc \
		piano-frametime.m display_evidence.m frametime_stats.c $(FRAMEWORKS) -o "$(OUT)/piano-frametime"

test:
	mkdir -p "$(OUT)"
	$(HOST_CC) $(HOST_TEST_FLAGS) tests.c frametime_stats.c -lm -o "$(OUT)/stats_host"
	"$(OUT)/stats_host"
ifeq ($(shell uname -s),Darwin)
	$(CC) $(HOST_TEST_FLAGS) -fobjc-arc display_evidence_host.m display_evidence.m \
		-framework Foundation -framework IOKit -o "$(OUT)/display_evidence_host"
	$(PYTHON) display_evidence_cases.py "$(OUT)/display_evidence_host"
else
	@echo "SKIP display evidence tests (requires macOS Foundation and IOKit)"
endif
