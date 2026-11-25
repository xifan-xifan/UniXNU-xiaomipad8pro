CC = xcrun clang
CFLAGS = -O2 -Wall -Wextra -Werror
OUT = ../../../out/piano-vpio-probe

.PHONY: all test
all: $(OUT)/piano-vpio-probe

# arm64, macOS 14 (kAUVoiceIOProperty_OtherAudioDuckingConfiguration), ad-hoc signed with a fixed identifier; the embedded
# Info.plist carries the microphone purpose string for a process that is its own TCC responsible process (a LaunchAgent).
$(OUT)/piano-vpio-probe: piano-vpio-probe.m Info.plist
	mkdir -p $(OUT)
	$(CC) -arch arm64 -mmacosx-version-min=14.0 $(CFLAGS) -fobjc-arc piano-vpio-probe.m -sectcreate __TEXT __info_plist Info.plist \
	    -framework AudioToolbox -framework CoreAudio -framework AVFoundation -framework Foundation -o $@
	codesign -f -s - -i com.unixnu.piano-vpio-probe $@

# Host-only: the measurement's synthetic self-test (no audio device, no tablet).
test: all
	python3 piano-vpio-measure.py selftest $(OUT)/selftest
