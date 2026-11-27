// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int fixture_socket(int domain, int type, int protocol)
{
    assert(domain == AF_INET && type == SOCK_DGRAM && protocol == 0);
    return 5;
}
static int fixture_ioctl(int fd, unsigned long command, ...);
static int fixture_close(int fd) { assert(fd == 5); return 0; }

#define socket fixture_socket
#define ioctl fixture_ioctl
#define close fixture_close
#define main wifi_controls_main
#include "../piano-wifi-controls.c"
#undef main

static int set_error, short_read;

static int fixture_ioctl(int fd, unsigned long command, ...)
{
    assert(fd == 5);
    va_list args;
    va_start(args, command);
    struct request *r = va_arg(args, struct request *);
    va_end(args);
    assert(r->type == 51);
    void *data = (void *)(uintptr_t)r->data;
    if (command == 0xc02869c9UL) {
        assert(r->length == 3);
        memcpy(data, "WW", 3);
        if (short_read) r->length = 2;
        return 0;
    }
    assert(command == 0x802869c8UL && r->length == 8);
    assert(!memcmp(data, (uint8_t[]){1, 0, 0, 0, 'W', 'W', 0, 0}, 8));
    errno = EIO;
    return set_error ? -1 : 0;
}

int main(void)
{
    assert(country_probe(0) == 0);
    assert(country_probe(1) == 0);
    set_error = 1;
    assert(country_probe(1) == 2);
    short_read = 1;
    assert(country_probe(0) == 2);
    puts("Wi-Fi country probe: short reads and SET refusal passed");
}
