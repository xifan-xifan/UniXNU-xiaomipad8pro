// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>

#define main charge_config_main
#include "../piano-charge-config.c"
#undef main

static void replace(const char *path, const char *data, size_t length)
{
    FILE *stream = fopen(path, "wb");
    assert(stream && fwrite(data, 1, length, stream) == length && !fclose(stream));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *path = argv[1];
    unsigned value = 0;
    assert(parse("4294967295", &value) && value == 0xffffffffu);
    assert(!parse("4294967296", &value));
    assert(!parse(" +1", &value));
    assert(!parse("1 ", &value));
    assert(!parse("-0", &value));

    uid_t owner = geteuid();
    unsigned v[6] = {0};
    assert(load_config(path, owner, v) == 2);
    const unsigned saved[6] = {1, 80, 75, 1, 0, 0};
    assert(!persist(path, saved));
    assert(!load_config(path, owner, v) && !memcmp(saved, v, sizeof v));
    assert(persist(path, (unsigned[]){1, 80, 75, 1, 1, 0}));
    assert(!chmod(path, 0666) && load_config(path, owner, v) == 1 && !chmod(path, 0600));
    const char embedded[] = "1 80 75 1\0trailing";
    replace(path, embedded, sizeof embedded - 1);
    assert(load_config(path, owner, v) == 1);
    char long_line[100];
    memset(long_line, ' ', sizeof long_line);
    memcpy(long_line, "1 80 75 1", 9);
    long_line[99] = '2';
    replace(path, long_line, sizeof long_line);
    assert(load_config(path, owner, v) == 1);
    const char *bad[] = {"", "1 80 75", "1 4294967376 75 1", "-0 80 75 1", "1 80 75 1 0"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        replace(path, bad[i], strlen(bad[i]));
        assert(load_config(path, owner, v) == 1);
    }
    assert(!unlink(path) && !mkfifo(path, 0600) && load_config(path, owner, v) == 1 && !unlink(path));

    CFMutableDictionaryRef state = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    assert(state && !number(state, "ConfigError", &value));
    double fractional = 0.5;
    CFNumberRef error = CFNumberCreate(NULL, kCFNumberDoubleType, &fractional);
    CFDictionarySetValue(state, CFSTR("ConfigError"), error);
    assert(!number(state, "ConfigError", &value));
    CFRelease(error);
    int64_t revision = UINT32_MAX;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, &revision);
    CFDictionarySetValue(state, CFSTR("ConfigRevision"), n);
    assert(number(state, "ConfigRevision", &value) && value == UINT32_MAX);
    CFRelease(n);
    CFRelease(state);
    puts("charge config: numeric readback and saved file boundaries passed");
}
