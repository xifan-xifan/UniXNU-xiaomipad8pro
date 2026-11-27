/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_BOOT_ARGS_H
#define PIANO_BOOT_ARGS_H

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/sysctl.h>

/* k27 0066's merged line, including the terminating NUL. */
#define PIANO_BOOT_ARGS_MAX 4096

/* The userland stager recognizes readers built with this header. */
static const char pianoBootArgsOID[] __attribute__((used)) = "kern.piano_bootargs\0PianoBootArgs ABI 1";

/* Read the whole line (k27 0076), or the legacy clip when the OID is absent. */
static inline void
pianoBootArgs(char args[PIANO_BOOT_ARGS_MAX])
{
    memset(args, 0xff, PIANO_BOOT_ARGS_MAX);
    size_t size = PIANO_BOOT_ARGS_MAX;
    int status = sysctlbyname(pianoBootArgsOID, args, &size, NULL, 0);
    if (status != 0 && errno == ENOENT) {
        memset(args, 0xff, PIANO_BOOT_ARGS_MAX);
        size = PIANO_BOOT_ARGS_MAX;
        status = sysctlbyname("kern.bootargs", args, &size, NULL, 0);
    }
    if (status != 0 || size == 0 || size > PIANO_BOOT_ARGS_MAX ||
        memchr(args, '\0', size) != args + size - 1)
        args[0] = '\0';
}

/* Like PE's named options: space/tab separators, an exact key and the last occurrence. */
static inline const char *
pianoBootArgValue(const char *args, const char *name, size_t *length)
{
    const char *value = NULL;
    const size_t key = strlen(name);
    *length = 0;
    while (*args) {
        args += strspn(args, " \t");
        const size_t word = strcspn(args, " \t");
        if (word >= key && !strncmp(args, name, key)) {
            if (word == key) {
                value = "1";
                *length = 1;
            } else if (args[key] == '=') {
                value = args + key + 1;
                *length = word - key - 1;
            }
        }
        args += word;
    }
    return value;
}

#endif /* PIANO_BOOT_ARGS_H */
