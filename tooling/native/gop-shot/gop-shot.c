// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "Pixels.h"

static CFDataRef encode_png(const uint8_t *pixels)
{
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, GOP_SHOT_BYTES - sizeof(GopShotHeader), NULL);
    CGImageRef image = srgb && provider ?
        CGImageCreate(GOP_SHOT_WIDTH, GOP_SHOT_HEIGHT, 8, 32, GOP_SHOT_ROW_BYTES, srgb,
                      kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast, provider, NULL, false,
                      kCGRenderingIntentDefault) : NULL;
    CFMutableDataRef png = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef destination = png ? CGImageDestinationCreateWithData(png, CFSTR("public.png"), 1, NULL) : NULL;
    bool encoded = false;
    if (image && destination) {
        CGImageDestinationAddImage(destination, image, NULL);
        encoded = CGImageDestinationFinalize(destination);
    }
    if (destination)
        CFRelease(destination);
    if (image)
        CGImageRelease(image);
    if (provider)
        CGDataProviderRelease(provider);
    if (srgb)
        CGColorSpaceRelease(srgb);
    if (png && !encoded)
        CFRelease(png);
    return encoded ? png : NULL;
}

static int write_png(const char *path, CFDataRef png)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return errno;
    const uint8_t *next = CFDataGetBytePtr(png);
    size_t remaining = (size_t)CFDataGetLength(png);
    int error = 0;
    while (remaining) {
        const ssize_t count = write(fd, next, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = count < 0 ? errno : EIO;
            break;
        }
        next += count;
        remaining -= (size_t)count;
    }
    if (close(fd) && !error)
        error = errno;
    if (error)
        unlink(path);
    return error;
}

static int read_frame(GopShotHeader *frame, GopShotLayout override_layout, GopShotLayout *layout)
{
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        size_t size = GOP_SHOT_BYTES;
        if (sysctlbyname(GOP_SHOT_SYSCTL, frame, &size, NULL, 0) == 0)
            return gop_shot_decode_reply(frame, size, override_layout, layout);
        if (errno != EBUSY)
            return errno;
        if (attempt != 7)
            usleep(37000 + attempt * 7000);
    }
    return EBUSY;
}

int main(int argc, char **argv)
{
    GopShotLayout override_layout = GOP_SHOT_LAYOUT_AUTO;
    if (argc == 3 && strcmp(argv[1], "--bga8") == 0)
        override_layout = GOP_SHOT_LAYOUT_BGRA8;
    else if (argc == 3 && strcmp(argv[1], "--10bit") == 0)
        override_layout = GOP_SHOT_LAYOUT_PACKED10;
    if ((argc != 2 && override_layout == GOP_SHOT_LAYOUT_AUTO) ||
        argv[argc - 1][0] == '-' || !argv[argc - 1][0]) {
        fputs("usage: gop-shot [--bga8|--10bit] OUTPUT.png\n", stderr);
        return 2;
    }
    if (geteuid() != 0) {
        fputs("gop-shot: root required\n", stderr);
        return 1;
    }
    GopShotHeader *frame = malloc(GOP_SHOT_BYTES);
    if (!frame) {
        fputs("gop-shot: cannot allocate the caller frame buffer\n", stderr);
        return 1;
    }
    GopShotLayout layout;
    const int read_error = read_frame(frame, override_layout, &layout);
    if (read_error) {
        const char *message = strerror(read_error);
        if (read_error == ENOENT)
            message = "shot sysctl absent (requires native-gop-display-shot=1 and NativeGopDisplay)";
        else if (read_error == ENXIO)
            message = "no readable scanout";
        else if (read_error == EBUSY)
            message = "scanout kept changing; retry on a still frame";
        fprintf(stderr, "gop-shot: %s\n", message);
        free(frame);
        return 1;
    }
    CFDataRef png = encode_png((const uint8_t *)(frame + 1));
    const uint32_t source = frame->source, format = frame->format;
    free(frame);
    if (!png) {
        fputs("gop-shot: PNG encoding failed\n", stderr);
        return 1;
    }
    const int write_error = write_png(argv[argc - 1], png);
    CFRelease(png);
    if (write_error) {
        fprintf(stderr, "gop-shot: cannot write output: %s\n", strerror(write_error));
        return 1;
    }
    printf("source=%s format=%c%c%c%c layout=%s\n", source == GOP_SHOT_FLIP ? "flip" : "boot",
           (uint8_t)(format >> 24), (uint8_t)(format >> 16), (uint8_t)(format >> 8), (uint8_t)format,
           layout == GOP_SHOT_LAYOUT_PACKED10 ? "10-bit" : "8-bit");
    return 0;
}
