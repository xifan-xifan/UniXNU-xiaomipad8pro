// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int fixture_sysctl(const char *, void *, size_t *, void *, size_t);
static uid_t fixture_euid(void) { return 0; }

#define sysctlbyname fixture_sysctl
#define geteuid fixture_euid
#define main gop_shot_main
#include "../gop-shot.c"
#undef main

static size_t reply_bytes;
static unsigned calls, busy;

static int fixture_sysctl(const char *name, void *reply, size_t *size, void *input, size_t input_size)
{
    assert(!strcmp(name, GOP_SHOT_SYSCTL) && *size == GOP_SHOT_BYTES && !input && !input_size);
    ++calls;
    if (calls <= busy) {
        errno = EBUSY;
        return -1;
    }
    *size = reply_bytes;
    if (reply_bytes == GOP_SHOT_BYTES) {
        GopShotHeader *header = reply;
        *header = (GopShotHeader){GOP_SHOT_VERSION, GOP_SHOT_WIDTH, GOP_SHOT_HEIGHT, GOP_SHOT_BGA, GOP_SHOT_FLIP};
        memset(header + 1, 0, GOP_SHOT_BYTES - sizeof *header);
        memcpy(header + 1, (uint8_t[]){0, 1, 0xf8, 0x1f}, 4);
    }
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(gop_shot_main(0, (char *[]){NULL}) == 2);
    char *args[] = {"gop-shot", "--10bit", argv[1], NULL};
    for (size_t i = 0; i < 3; ++i) {
        const size_t sizes[] = {0, GOP_SHOT_BYTES - 1, GOP_SHOT_BYTES + 1};
        calls = 0;
        reply_bytes = sizes[i];
        assert(gop_shot_main(3, args) == 1 && calls == 1);
        assert(access(argv[1], F_OK) == -1 && errno == ENOENT);
    }

    calls = 0;
    busy = 2;
    reply_bytes = GOP_SHOT_BYTES;
    assert(gop_shot_main(3, args) == 0 && calls == 3);
    CGDataProviderRef provider = CGDataProviderCreateWithFilename(argv[1]);
    assert(provider);
    CGImageRef image = CGImageCreateWithPNGDataProvider(provider, NULL, false, kCGRenderingIntentDefault);
    assert(image && CGImageGetWidth(image) == GOP_SHOT_WIDTH && CGImageGetHeight(image) == GOP_SHOT_HEIGHT);
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    assert(gop_shot_main(3, args) == 1); // Existing file must survive.
    assert(!unlink(argv[1]));
    puts("gop-shot CLI: short replies, busy retry, PNG decode and exclusive output passed");
    return 0;
}
