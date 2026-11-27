// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#define DATA "/System/Volumes/Data"
#define BLOCK 4096
#define MIB (1024 * 1024)
#define SPAN (32 * MIB)
#define SYNC_OPS 32
#define READ_OPS 4096
#define RECORDS 32

static const struct {
    const char *name;
    unsigned wb, queue;
} phases[] = {
    {"volatile", 0, 0}, {"control", 0, 0}, {"writeback", 1, 0},
    {"queue", 0, 1}, {"combined", 1, 1},
};
static unsigned phase;
static const char *run;
static uint64_t nonce, lba, blocks;
static dev_t data_device;

static _Noreturn void stop(const char *status, const char *reason)
{
    printf("DUR ERROR status=%s reason=%s errno=%d\n", status, reason, errno);
    exit(!strcmp(status, "FAIL") ? 1 : 2);
}

static uint64_t number(const char *text)
{
    char *end;
    if (*text < '0' || *text > '9') return UINT64_MAX;
    errno = 0;
    uint64_t value = strtoull(text, &end, 10);
    return errno || (*end && *end != ' ') ? UINT64_MAX : value;
}

static const char *field(const char *stats, const char *key)
{
    const size_t n = strlen(key);
    for (const char *p = stats; p; ++p) {
        if (!strncmp(p, key, n) && p[n] == '=') return p + n + 1;
        p = strchr(p, ' ');
    }
    return "";
}

static bool equals(const char *stats, const char *key, const char *value)
{
    const char *found = field(stats, key);
    const size_t size = strlen(value);
    return !strncmp(found, value, size) && (found[size] == ' ' || !found[size]);
}

static uint64_t statnum(const char *stats, const char *key)
{
    return number(field(stats, key));
}

static void sysstring(const char *key, char *buf, size_t capacity)
{
    size_t size = capacity;
    if (sysctlbyname(key, buf, &size, NULL, 0) || size < 2 || size > capacity || buf[size - 1])
        stop("VOID", "sysctl");
}

// Match the mounted APFS volume's physical ancestry in IOService, not its display name.
static bool native_data(const char *device)
{
    if (strncmp(device, "/dev/disk", 9)) return false;
    io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault,
        IOBSDNameMatching(kIOMainPortDefault, 0, device + 5));
    if (!media) return false;
    io_iterator_t parents;
    kern_return_t rc = IORegistryEntryCreateIterator(media, kIOServicePlane,
        kIORegistryIterateRecursively | kIORegistryIterateParents, &parents);
    IOObjectRelease(media);
    if (rc != KERN_SUCCESS) return false;

    unsigned native = 0, physical = 0;
    io_registry_entry_t parent;
    while ((parent = IOIteratorNext(parents))) {
        if (IOObjectConformsTo(parent, "NativeRamBlock")) ++native;
        if (IOObjectConformsTo(parent, "IOBlockStorageDevice")) ++physical;
        IOObjectRelease(parent);
    }
    const bool valid = IOIteratorIsValid(parents);
    IOObjectRelease(parents);
    return valid && native == 1 && physical == 1;
}

static void stats_read(char stats[2048])
{
    sysstring("machdep.piano_data_io", stats, 2048);
    if (!equals(stats, "ram", "1") || !equals(stats, "qual", "1"))
        stop("VOID", "ram-profile");
    if (!phase) {
        if (!equals(stats, "mode", "volatile") || !equals(stats, "persist", "0") ||
            statnum(stats, "state") != 0 || statnum(stats, "lba") != 0 || statnum(stats, "bound") != 0)
            stop("VOID", "volatile-store");
    } else {
        const uint64_t pages = statnum(stats, "pages");
        if (!equals(stats, "persist", "2") || !equals(stats, "scratch", "1") ||
            statnum(stats, "lba") != lba || statnum(stats, "bound") != lba ||
            !pages || pages > UINT32_MAX || 1 + (pages + 32767) / 32768 + pages != blocks ||
            statnum(stats, "flags") != 2 || strspn(field(stats, "seed"), "0123456789abcdef") != 8 ||
            strncmp(field(stats, "seed"), field(stats, "store_seed"), 8))
            stop("VOID", "scratch-store");
        if (statnum(stats, "state") == 5 || statnum(stats, "errors") != 0)
            stop("FAIL", "store-degraded");
        const uint64_t depth = statnum(stats, "depth");
        if (!equals(stats, "mode", "concurrent") || depth < 2 || depth > 8 ||
            (statnum(stats, "state") != 2 && statnum(stats, "state") != 3))
            stop("VOID", "direct-mode");
    }
    if (statnum(stats, "write_back") != phases[phase].wb ||
        statnum(stats, "queue") != phases[phase].queue)
        stop("VOID", "phase-switches");
    if (statnum(stats, "full") != 0) stop("FAIL", "queue-full");
}

static void snapshot(const char *row, const char *edge, unsigned op)
{
    char stats[2048];
    stats_read(stats);
    printf("DUR SNAP row=%s edge=%s op=%u %s\n", row, edge, op, stats);
}

static int open_data(void)
{
    int fd = open(DATA, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    struct statfs fs;
    if (fd < 0 || fstat(fd, &st) || fstatfs(fd, &fs) ||
        strcmp(fs.f_mntonname, DATA) || strcmp(fs.f_fstypename, "apfs") ||
        (fs.f_flags & MNT_RDONLY) || !native_data(fs.f_mntfromname))
        stop("VOID", "data-mount");
    data_device = st.st_dev;
    return fd;
}

static int workspace(int data, bool create)
{
    char name[64];
    snprintf(name, sizeof name, "piano-durability-%s-%s", phases[phase].name, run);
    if (create && mkdirat(data, name, 0700))
        stop(errno == EEXIST || errno == EACCES ? "VOID" : "FAIL", "create-workspace");
    int fd = openat(data, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || st.st_dev != data_device || st.st_uid != geteuid() || (st.st_mode & 077))
        stop("VOID", "workspace-identity");
    return fd;
}

static int file_at(int dir, const char *name, bool create, bool no_readahead)
{
    int flags = create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY;
    int fd = openat(dir, name, flags | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_dev != data_device ||
        st.st_nlink != 1 || st.st_uid != geteuid())
        stop("VOID", "file-identity");
    if (fcntl(fd, F_NOCACHE, 1) || (no_readahead && fcntl(fd, F_RDAHEAD, 0)))
        stop("VOID", "uncached-io");
    return fd;
}

// Use the run nonce and logical page/sequence in every block, so stale and misplaced pages fail.
static void pattern(void *page, uint64_t tag, uint64_t index)
{
    uint64_t value = nonce ^ tag ^ (index * UINT64_C(0x9e3779b97f4a7c15));
    uint64_t *words = page;
    for (unsigned i = 0; i < BLOCK / sizeof(*words); ++i) {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        words[i] = value * UINT64_C(0x2545f4914f6cdd1d);
    }
    words[0] = tag;
    words[1] = index;
    words[2] = nonce;
}

static void write_exact(int fd, const void *buf, size_t size, off_t offset)
{
    if (pwrite(fd, buf, size, offset) != (ssize_t)size) stop("FAIL", "write");
}

static void read_exact(int fd, void *buf, size_t size, off_t offset)
{
    if (pread(fd, buf, size, offset) != (ssize_t)size) stop("FAIL", "read");
}

static void sync_rows(int dir)
{
    static const char *rows[] = {"plain", "fsync", "full", "barrier"};
    _Alignas(BLOCK) uint64_t buf[BLOCK / 8], expected[BLOCK / 8];
    int fd = file_at(dir, "sync.bin", true, false);
    if (fd < 0) stop("FAIL", "create-sync-file");
    snapshot("fill", "before", 0);
    for (unsigned i = 0; i < SYNC_OPS; ++i) {
        pattern(buf, 1, i);
        write_exact(fd, buf, BLOCK, (off_t)i * BLOCK);
    }
    if (fcntl(fd, F_FULLFSYNC, 0)) stop("FAIL", "prefill-sync");
    snapshot("fill", "after", 0);

    for (unsigned row = 0; row < sizeof rows / sizeof rows[0]; ++row) {
        for (unsigned i = 0; i < SYNC_OPS; ++i) {
            pattern(buf, row + 2, i);
            snapshot(rows[row], "before", i);
            write_exact(fd, buf, BLOCK, (off_t)i * BLOCK);
            snapshot(rows[row], "write", i);
            int rc = 0;
            if (row == 1) rc = fsync(fd);
            if (row == 2) rc = fcntl(fd, F_FULLFSYNC, 0);
            if (row == 3) rc = fcntl(fd, F_BARRIERFSYNC, 0);
            const int saved_errno = errno;
            snapshot(rows[row], "after", i);
            errno = saved_errno;
            if (rc) stop("FAIL", rows[row]);
        }
        for (unsigned i = 0; i < SYNC_OPS; ++i) {
            pattern(expected, row + 2, i);
            read_exact(fd, buf, BLOCK, (off_t)i * BLOCK);
            if (memcmp(buf, expected, BLOCK)) stop("FAIL", "sync-readback");
        }
        printf("DUR ROW row=%s ops=%d\n", rows[row], SYNC_OPS);
    }
    if (close(fd)) stop("FAIL", "close-sync-file");
}

struct writer {
    int fd;
    atomic_bool stop, active;
    atomic_uint writes;
    const void *buf;
};

static void *write_mixed(void *opaque)
{
    struct writer *w = opaque;
    unsigned n = 0;
    do {
        atomic_store(&w->active, true);
        write_exact(w->fd, w->buf, MIB, SPAN + (off_t)(n % (SPAN / MIB)) * MIB);
        atomic_store(&w->writes, ++n);
        atomic_store(&w->active, false);
    } while (!atomic_load(&w->stop));
    return NULL;
}

static int compare_ns(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static void mixed(int dir)
{
    _Alignas(BLOCK) uint64_t buf[BLOCK / 8], expected[BLOCK / 8];
    uint8_t *chunk;
    if (posix_memalign((void **)&chunk, BLOCK, MIB)) stop("VOID", "allocation");
    int fd = file_at(dir, "mixed.bin", true, true);
    if (fd < 0) stop("FAIL", "create-mixed-file");
    for (unsigned offset = 0; offset < 2 * SPAN; offset += MIB) {
        for (unsigned n = 0; n < MIB; n += BLOCK)
            pattern(chunk + n, 9, (offset + n) / BLOCK);
        write_exact(fd, chunk, MIB, offset);
    }
    if (fcntl(fd, F_FULLFSYNC, 0)) stop("FAIL", "mixed-prefill-sync");
    for (unsigned n = 0; n < MIB; n += BLOCK)
        pattern(chunk + n, 10, n / BLOCK);

    snapshot("mixed", "before", 0);
    uint64_t began = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    struct writer w = {.fd = fd, .buf = chunk};
    pthread_t thread;
    if (pthread_create(&thread, NULL, write_mixed, &w)) stop("VOID", "writer-start");
    while (!atomic_load(&w.active) && !atomic_load(&w.writes)) usleep(100);
    uint64_t samples[READ_OPS];
    unsigned overlap = 0;
    uint32_t random = 5;
    for (unsigned i = 0; i < READ_OPS; ++i) {
        random = random * 1664525u + 1013904223u;
        unsigned index = (random >> 16) % (SPAN / BLOCK);
        unsigned completed = atomic_load(&w.writes);
        bool active = atomic_load(&w.active);
        uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        read_exact(fd, buf, BLOCK, (off_t)index * BLOCK);
        samples[i] = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start;
        if (active || completed != atomic_load(&w.writes)) ++overlap;
        pattern(expected, 9, index);
        if (memcmp(buf, expected, BLOCK)) stop("FAIL", "mixed-reader-data");
    }
    atomic_store(&w.stop, true);
    if (pthread_join(thread, NULL)) stop("VOID", "writer-join");
    uint64_t duration = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - began;
    snapshot("mixed", "after", 0);
    unsigned writes = atomic_load(&w.writes);
    off_t last = SPAN + (off_t)((writes - 1) % (SPAN / MIB)) * MIB;
    for (unsigned i = 0; i < MIB; i += BLOCK) {
        read_exact(fd, buf, BLOCK, last + i);
        if (memcmp(buf, chunk + i, BLOCK)) stop("FAIL", "mixed-writer-data");
    }
    qsort(samples, READ_OPS, sizeof(samples[0]), compare_ns);
    printf("DUR MIX reads=%d writes=%u overlap=%u p99_ns=%llu duration_ns=%llu\n",
        READ_OPS, writes, overlap, samples[(99 * READ_OPS + 99) / 100 - 1], duration);
    if (close(fd)) stop("FAIL", "close-mixed-file");
    free(chunk);
}

static void prepare(int dir, uint64_t reset)
{
    int fd = file_at(dir, "crash.bin", true, false);
    if (fd < 0) stop(errno == EEXIST ? "VOID" : "FAIL", "create-crash-file");
    _Alignas(BLOCK) uint64_t buf[BLOCK / 8];
    for (unsigned seq = 1; seq <= RECORDS + 1; ++seq) {
        pattern(buf, 0, seq);
        write_exact(fd, buf, BLOCK, (off_t)(seq - 1) * BLOCK);
    }
    if (fsync(dir) || fcntl(fd, F_FULLFSYNC, 0)) stop("FAIL", "anchor-crash-file");
    uint64_t uptime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000;
    if (uptime + 3 >= reset) stop("VOID", "reset-too-close");
    if (uptime + 10 < reset) sleep((unsigned)(reset - uptime - 10));
    snapshot("crash", "before", 0);

    for (unsigned seq = 1; seq <= RECORDS; ++seq) {
        pattern(buf, 0x5049414e4f4c4f47ULL, seq);
        write_exact(fd, buf, BLOCK, (off_t)(seq - 1) * BLOCK);
        if (fcntl(fd, F_FULLFSYNC, 0)) stop("FAIL", "ack-fullsync");
        printf("DUR ACK seq=%u\n", seq);
    }
    snapshot("crash", "acked", 0);
    pattern(buf, 0x5049414e4f4c4f47ULL, RECORDS + 1);
    write_exact(fd, buf, BLOCK, (off_t)RECORDS * BLOCK);
    snapshot("crash", "ready", 0);
    printf("DUR READY last=%d tail=%d\n", RECORDS, RECORDS + 1);
    // Keep the log open, with no close, unmount, sync or cleanup before the cut.
    for (;;) pause();
}

static void verify(int data)
{
    int dir = workspace(data, false);
    if (dir < 0 && errno != ENOENT) stop("FAIL", "open-crash-workspace");
    int fd = dir < 0 ? -1 : file_at(dir, "crash.bin", false, false);
    if (fd < 0 && errno != ENOENT) stop("FAIL", "open-crash-file");
    _Alignas(BLOCK) uint64_t buf[BLOCK / 8], expected[BLOCK / 8];
    for (unsigned seq = 1; seq <= RECORDS + 1; ++seq) {
        pattern(expected, 0x5049414e4f4c4f47ULL, seq);
        ssize_t n = fd < 0 ? 0 : pread(fd, buf, BLOCK, (off_t)(seq - 1) * BLOCK);
        if (n < 0) stop("FAIL", "read-crash-file");
        printf("DUR RECORD seq=%u bytes=%zd match=%u\n", seq, n,
            n == BLOCK && !memcmp(buf, expected, BLOCK));
    }
    snapshot("recover", "after", 0);
    if (fd >= 0) close(fd);
    if (dir >= 0) close(dir);
}

int main(int argc, char **argv)
{
    // Line buffering is required when stdout is an SSH pipe.
    if (setvbuf(stdout, NULL, _IOLBF, 0)) return 2;
    if (argc != 7) stop("VOID", "usage");
    const char *action = argv[1];
    const bool measure = !strcmp(action, "measure"), prep = !strcmp(action, "prepare");
    if (!measure && !prep && strcmp(action, "verify")) stop("VOID", "action");
    for (; phase < sizeof phases / sizeof phases[0]; ++phase)
        if (!strcmp(phases[phase].name, argv[2])) break;
    if (phase == sizeof phases / sizeof phases[0] || (!phase && !measure))
        stop("VOID", "phase");
    run = argv[3];
    if (strlen(run) != 32 || strspn(run, "0123456789abcdef") != 32)
        stop("VOID", "run-id");
    for (const char *p = run; *p; ++p) nonce = nonce * 1099511628211ULL ^ (uint8_t)*p;
    lba = number(argv[4]);
    blocks = number(argv[5]);
    if (strspn(argv[4], "0123456789") != strlen(argv[4]) ||
        strspn(argv[5], "0123456789") != strlen(argv[5]) ||
        (phase ? (!lba || !blocks || lba >= UINT32_MAX || blocks > UINT32_MAX - lba)
               : (lba || blocks)))
        stop("VOID", "window");

    char stats[2048], boot[64];
    uuid_t parsed;
    stats_read(stats);
    sysstring("kern.bootsessionuuid", boot, sizeof boot);
    if (uuid_parse(boot, parsed)) stop("VOID", "boot-uuid");
    uuid_t expected;
    if (!measure && (uuid_parse(argv[6], expected) ||
                     (uuid_compare(parsed, expected) == 0) != prep))
        stop("VOID", "unexpected-boot-uuid");
    int data = open_data();
    printf("DUR BEGIN action=%s phase=%s run=%s boot=%s\n", action, phases[phase].name, run, boot);
    printf("DUR SNAP row=identity edge=before op=0 %s\n", stats);
    if (prep && (!equals(stats, "reset_fastboot", "1") || !statnum(stats, "reset_after") ||
                 statnum(stats, "reset_after") > 3600))
        stop("VOID", "reset-profile");
    if (!measure && !prep) {
        verify(data);
    } else {
        int dir = workspace(data, measure);
        if (dir < 0) stop("VOID", "open-workspace");
        if (prep) prepare(dir, statnum(stats, "reset_after"));
        sync_rows(dir);
        mixed(dir);
        close(dir);
    }
    close(data);
    puts("DUR END");
    return fflush(stdout) || ferror(stdout) ? 2 : 0;
}
