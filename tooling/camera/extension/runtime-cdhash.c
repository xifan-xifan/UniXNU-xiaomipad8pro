// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// Read the running process's CodeDirectory hash from XNU, not from its file.
#include <errno.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

extern int csops(pid_t, unsigned int, void *, size_t);

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: piano-camera-runtime PID|self EXPECTED_CDHASH\n");
        return 2;
    }
    char *end = NULL;
    int self = strcmp(argv[1], "self") == 0;
    long number = self ? (long)getpid() : strtol(argv[1], &end, 10);
    if ((!self && (!end || *end)) || number < 1 || number > 0x7fffffff || strlen(argv[2]) != 40 ||
        strspn(argv[2], "0123456789abcdef") != 40) {
        fprintf(stderr, "invalid pid or hash\n");
        return 2;
    }
    unsigned char hash[20];
    if (csops((pid_t)number, 5 /* CS_OPS_CDHASH */, hash, sizeof(hash)) != 0) {
        fprintf(stderr, "runtime csops failed errno=%d\n", errno);
        return 1;
    }
    char hex[41], path[PROC_PIDPATHINFO_MAXSIZE];
    for (unsigned int i = 0; i < sizeof(hash); ++i)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    printf("runtime CDHash=%s\n", hex);
    int length = proc_pidpath((int)number, path, sizeof(path));
    const char prefix[] = "/Library/SystemExtensions/";
    const char suffix[] = "/com.unixnu.PianoCamera.Extension.systemextension/Contents/MacOS/com.unixnu.PianoCamera.Extension";
    size_t n = length > 0 ? strlen(path) : 0;
    int staged = n > sizeof(prefix) + sizeof(suffix) &&
                 strncmp(path, prefix, sizeof(prefix) - 1) == 0 &&
                 strcmp(path + n - (sizeof(suffix) - 1), suffix) == 0;
    printf("runtime staged-path=%d\n", staged);
    if ((!self && !staged) || strcmp(hex, argv[2]) != 0)
        return 1;
    puts("runtime hash matches registry");
    return 0;
}
