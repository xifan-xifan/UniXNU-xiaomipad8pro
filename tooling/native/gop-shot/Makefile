OUT ?= ../../../out/gop-shot
CFLAGS += -std=c11 -Wall -Wextra -Werror
DRIVER = ../../../package/NativeGopDisplay
HEADER = $(DRIVER)/GopShot.h

.PHONY: all test
all: $(OUT)/gop-shot

test: all $(OUT)/gop_shot_host $(OUT)/gop_cli_host
	"$(OUT)/gop_shot_host"
	scratch=$$(mktemp -d "$(OUT)/gop-cli.XXXXXXXX") && \
		trap 'rm -rf -- "$$scratch"' EXIT && "$(OUT)/gop_cli_host" "$$scratch/frame.png"

$(OUT)/gop-shot: gop-shot.c Pixels.h $(HEADER)
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -D_DARWIN_C_SOURCE -O2 -arch arm64 -mmacosx-version-min=13.0 \
		-framework CoreFoundation -framework CoreGraphics -framework ImageIO gop-shot.c -o "$@"

$(OUT)/gop_shot_host: $(DRIVER)/tests/gop_shot_host.c Pixels.h $(HEADER)
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) "$<" -o "$@"

$(OUT)/gop_cli_host: tests/gop_cli_host.c gop-shot.c Pixels.h $(HEADER)
	mkdir -p "$(OUT)"
	$(CC) $(CFLAGS) -D_DARWIN_C_SOURCE \
		-framework CoreFoundation -framework CoreGraphics -framework ImageIO "$<" -o "$@"
