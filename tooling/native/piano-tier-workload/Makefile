# SPDX-License-Identifier: MIT
CC = xcrun --sdk macosx clang
SDK = macosx27.3
MACOS_SDK ?= $(shell xcrun --sdk $(SDK) --show-sdk-path)
OUT = ../../../out/piano-tier-workload
CFLAGS = -O2 -Wall -Wextra -Werror
FRAMEWORKS = -framework Foundation -framework Metal

.PHONY: all test
all:
	test -n "$(MACOS_SDK)" && test -d "$(MACOS_SDK)"
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -arch arm64 -isysroot "$(MACOS_SDK)" -mmacosx-version-min=14.0 -fobjc-arc \
		piano-tier-workload.m tier_stats.c $(FRAMEWORKS) -o "$(OUT)/piano-tier-workload"

test:
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all \
		tests.c tier_stats.c -o "$(OUT)/tier_stats_host"
	"$(OUT)/tier_stats_host"
