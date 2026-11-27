/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_AUDIO_CLI_H
#define PIANO_AUDIO_CLI_H

#include <errno.h>
#include <math.h>
#include <stdlib.h>

static inline int audio_parse_double(const char *text, double *out)
{
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || *end || errno == ERANGE || !isfinite(value))
        return 0;
    *out = value;
    return 1;
}

static inline int audio_parse_long(const char *text, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (end == text || *end || errno == ERANGE || value < min || value > max)
        return 0;
    *out = value;
    return 1;
}

#endif
