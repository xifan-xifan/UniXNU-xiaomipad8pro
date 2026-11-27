CC = xcrun clang
CFLAGS = -O2 -Wall -Wextra -Werror
OUT = ../../../out/piano-brightness-sync

.PHONY: all test
all: $(OUT)/piano-brightness-sync

# The same flags as configs/userland.toml's unit (tooling/userland/stage.py's cc build).
$(OUT)/piano-brightness-sync: piano-brightness-sync.m SyncPolicy.h ../../../package/include/PianoBootArgs.h
	mkdir -p $(OUT)
	$(CC) -arch arm64 -mmacosx-version-min=14.0 $(CFLAGS) -fobjc-arc piano-brightness-sync.m -framework Foundation -framework IOKit -o $@

test: all
	bash tests/run-sync-host.sh
