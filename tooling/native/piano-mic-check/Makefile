# SPDX-License-Identifier: MIT
CC = xcrun --sdk macosx clang
SDK = macosx
MACOS_SDK ?= $(shell xcrun --sdk $(SDK) --show-sdk-path)
OUT = ../../../out/piano-mic-check
PYTHON ?= python3
CFLAGS = -O2 -Wall -Wextra -Werror
FRAMEWORKS = -framework AppKit -framework AVFoundation -framework AudioToolbox -framework CoreMedia -framework CoreAudio

APP = $(OUT)/PianoMicCheck.app
CONTENTS = $(APP)/Contents
BINARY = $(CONTENTS)/MacOS/piano-mic-check
TONE = $(CONTENTS)/Resources/mic-check-tone.wav

.PHONY: all test
all: $(OUT)/.signed

$(CONTENTS)/Info.plist: Info.plist
	mkdir -p "$(CONTENTS)"
	cp "$<" "$@"

$(BINARY): piano-mic-check.m ../audio-wav.h ../../../package/include/PianoBootArgs.h Makefile
	test -n "$(MACOS_SDK)" && test -d "$(MACOS_SDK)"
	mkdir -p "$(CONTENTS)/MacOS"
	$(CC) $(CFLAGS) -arch arm64 -isysroot "$(MACOS_SDK)" -mmacosx-version-min=14.0 -fobjc-arc \
		piano-mic-check.m $(FRAMEWORKS) -o "$@"

$(TONE): make-tone.py
	mkdir -p "$(CONTENTS)/Resources"
	$(PYTHON) "$<" "$@"

$(OUT)/.signed: $(BINARY) $(CONTENTS)/Info.plist $(TONE)
	/usr/bin/codesign --force --sign - "$(APP)"
	/usr/bin/codesign --verify --strict "$(APP)"
	touch "$@"

test: all
	$(PYTHON) test-mic-check.py "$(BINARY)" "$(TONE)"
