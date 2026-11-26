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
	$(CC) -std=c11 $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all tests/sync_policy_host.c -o $(OUT)/sync_policy_host
	$(OUT)/sync_policy_host
