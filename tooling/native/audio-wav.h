/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_AUDIO_WAV_H
#define PIANO_AUDIO_WAV_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define AUDIO_WAV_MAX_MONO_FRAMES ((UINT32_MAX - 36u) / 2u)

typedef struct {
    uint32_t rate, frames;
    uint16_t channels;
} AudioWav;

static inline uint16_t audio_le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | b[1] << 8);
}

static inline uint32_t audio_le32(const uint8_t *b)
{
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline void audio_put_le32(uint8_t *b, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) b[i] = (uint8_t)(value >> (8 * i));
}

static inline int audio_wav_write_header(FILE *f, uint32_t rate, uint64_t frames)
{
    if (!rate || rate > UINT32_MAX / 2 || frames > AUDIO_WAV_MAX_MONO_FRAMES)
        return -1;
    uint8_t header[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 0, 0, 0, 0
    };
    uint32_t bytes = (uint32_t)frames * 2u;
    audio_put_le32(header + 4, bytes + 36u);
    audio_put_le32(header + 24, rate);
    audio_put_le32(header + 28, rate * 2u);
    audio_put_le32(header + 40, bytes);
    return fseek(f, 0, SEEK_SET) || fwrite(header, 1, sizeof header, f) != sizeof header ? -1 : 0;
}

/* On success, leave f positioned at the first audio frame. A general
 * container check also allows formats to be decoded by AVFoundation. */
static inline int audio_wav_read_header(FILE *f, AudioWav *out, int pcm16_only)
{
    static const uint8_t pcm_guid[] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    static const uint8_t float_guid[] = {3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    uint8_t header[12];
    if (fread(header, 1, sizeof header, f) != sizeof header || memcmp(header, "RIFF", 4) ||
        memcmp(header + 8, "WAVE", 4))
        return -1;
    const off_t end = 8 + (off_t)audio_le32(header + 4);
    if (end < 12 || fseeko(f, 0, SEEK_END) || ftello(f) < end || fseeko(f, 12, SEEK_SET))
        return -1;
    AudioWav wav = {0};
    uint16_t tag = 0, align = 0;
    off_t data_offset = 0;
    for (off_t offset = 12; offset < end;) {
        uint8_t chunk[8];
        if (end - offset < (off_t)sizeof chunk || fread(chunk, 1, sizeof chunk, f) != sizeof chunk)
            return -1;
        const uint32_t size = audio_le32(chunk + 4);
        const off_t next = offset + 8 + (off_t)size + (size & 1u);
        if (next > end)
            return -1;
        if (!memcmp(chunk, "fmt ", 4)) {
            uint8_t format[40];
            const size_t count = size < sizeof format ? size : sizeof format;
            if (align || count < 16 || fread(format, 1, count, f) != count)
                return -1;
            tag = audio_le16(format);
            wav.channels = audio_le16(format + 2);
            wav.rate = audio_le32(format + 4);
            const uint32_t byte_rate = audio_le32(format + 8);
            align = audio_le16(format + 12);
            const uint16_t bits = audio_le16(format + 14);
            if (tag == 0xfffe) {
                if (count < 40 || audio_le16(format + 16) < 22 || audio_le16(format + 16) > size - 18 ||
                    audio_le16(format + 18) > bits)
                    return -1;
                if (!memcmp(format + 24, pcm_guid, sizeof pcm_guid)) tag = 1;
                else if (!memcmp(format + 24, float_guid, sizeof float_guid)) tag = 3;
            }
            if (!wav.rate || !wav.channels || !align || (pcm16_only && (tag != 1 || bits != 16)))
                return -1;
            if ((tag == 1 || tag == 3) && (!bits || align != (uint32_t)wav.channels * ((bits + 7u) / 8u) ||
                                          byte_rate != (uint64_t)wav.rate * align))
                return -1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!align || !size || data_offset || ((tag == 1 || tag == 3) && size % align))
                return -1;
            wav.frames = size / align;
            data_offset = offset + 8;
        }
        if (fseeko(f, next, SEEK_SET))
            return -1;
        offset = next;
    }
    if (!data_offset || fseeko(f, data_offset, SEEK_SET))
        return -1;
    *out = wav;
    return 0;
}

#endif
