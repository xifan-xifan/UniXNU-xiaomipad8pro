# SPDX-License-Identifier: MIT
CC = xcrun --sdk macosx clang
SDK = macosx27.3
MACOS_SDK ?= $(shell xcrun --sdk $(SDK) --show-sdk-path)
OUT = ../../../out/piano-frametime
CFLAGS = -O2 -Wall -Wextra -Werror
FRAMEWORKS = -framework AppKit -framework CoreGraphics -framework IOKit -framework Metal -framework QuartzCore

.PHONY: all test
all:
	test -n "$(MACOS_SDK)" && test -d "$(MACOS_SDK)"
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -arch arm64 -isysroot "$(MACOS_SDK)" -mmacosx-version-min=14.0 -fobjc-arc \
		piano-frametime.m display_evidence.m frametime_stats.c $(FRAMEWORKS) -o "$(OUT)/piano-frametime"

test:
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all \
		tests.c frametime_stats.c -o "$(OUT)/stats_host"
	"$(OUT)/stats_host"
