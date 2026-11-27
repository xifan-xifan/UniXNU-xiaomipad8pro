# SPDX-License-Identifier: MIT
CC = xcrun --sdk $(SDK) clang
SDK = macosx
MACOS_SDK ?= $(shell xcrun --sdk $(SDK) --show-sdk-path)
OUT = ../../../out/piano-metal-sustain
CFLAGS = -O2 -Wall -Wextra -Werror
FRAMEWORKS = -framework AppKit -framework CoreGraphics -framework Metal -framework QuartzCore

.PHONY: all test
all:
	test -n "$(MACOS_SDK)" && test -d "$(MACOS_SDK)"
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -arch arm64 -isysroot "$(MACOS_SDK)" -mmacosx-version-min=14.0 -fobjc-arc \
		piano-metal-sustain.m scene.m $(FRAMEWORKS) -o "$(OUT)/piano-metal-sustain"

test:
	python3 -B -m unittest -v
