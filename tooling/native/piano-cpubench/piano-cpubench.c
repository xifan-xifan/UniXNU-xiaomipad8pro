// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-cpubench: a fixed-work CPU micro-benchmark for the native macOS on piano (the perf lane, 2026-11-22). Each test does a fixed
// amount of work and prints its wall time, so the same binary on the same boot compares two CPU policies
// (machdep.piano_cpufreq_perf 0 / 1) or two machines. No device access, no privileges, no network.
//   st-int   one thread: a dependent integer chain (xorshift + add), SCALE x 200 M steps
//   st-fp    one thread: four dependent double FMA chains, SCALE x 100 M steps each
//   mt-int   one thread per online CPU, each SCALE x 50 M steps of st-int's chain; the time until the last one ends
//   burst    the UI-like case: N times (sleep GAP ms, then a fixed small job of SCALE x 4 M steps), each job's latency; a slow ramp of
//            the clock shows here first (p50/p90/max)
//   launch APP  open -a APP (or --exec PATH: that executable directly, no LaunchServices) and the time until a window of APP
//            (layer 0, > 50 x 50) is on screen (CGWindowList; no AppleEvents, no TCC), then (--close) SIGTERM to that app
//   now      CPUBENCH_NOW mach_s=<CACurrentMediaTime's base: mach_absolute_time in s> (aligns host actions with PianoGpuBench's
//            frame times, which are in the same base)
// Lines: "CPUBENCH test=NAME ms=N ..." per test, "CPUBENCH_BURST ...", "CPUBENCH_LAUNCH app=... ms=...", "CPUBENCH_END rc=N".
// Build (mini): clang -O2 -arch arm64 -mmacosx-version-min=14.0 -framework CoreFoundation -framework CoreGraphics -o piano-cpubench
// piano-cpubench.c (the linker signs it ad hoc).
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static double nowMs(void)
{
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static uint64_t chainInt(uint64_t steps, uint64_t seed)
{
  uint64_t x = seed | 1, acc = 0;
  for (uint64_t i = 0; i < steps; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    acc += x;
  }
  return acc;
}

static double chainFp(uint64_t steps)
{
  double a = 1.0, b = 1.1, c = 1.2, d = 1.3;
  const double m = 0.999999, k = 1e-7;
  for (uint64_t i = 0; i < steps; ++i) {
    a = a * m + k;
    b = b * m + k;
    c = c * m + k;
    d = d * m + k;
  }
  return a + b + c + d;
}

static void qos(const char *q)
{
  qos_class_t c = QOS_CLASS_USER_INTERACTIVE;
  if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
  else if (q && !strcmp(q, "bg")) c = QOS_CLASS_BACKGROUND;
  pthread_set_qos_class_self_np(c, 0);
}

struct Job {
  uint64_t steps, out;
  const char *qos;
  double endMs;
};
static void *mtWorker(void *p)
{
  struct Job *j = p;
  qos(j->qos);
  j->out = chainInt(j->steps, (uint64_t)(uintptr_t)p);
  j->endMs = nowMs();
  return NULL;
}

static int cmpd(const void *a, const void *b)
{
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static int ncpu(void)
{
  int n = 0;
  size_t len = sizeof n;
  if (sysctlbyname("hw.activecpu", &n, &len, NULL, 0) || n < 1) n = 1;
  return n > 64 ? 64 : n;
}

// The on-screen windows of an owner (by process id, or by name when pid <= 0; the owner name is localized, the pid is not): true when
// one is at layer 0 and larger than 50 x 50; -1 without a window list.
static int hasWindow(const char *owner, pid_t pid)
{
  CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
  if (!list) return -1;
  int found = 0;
  CFStringRef want = CFStringCreateWithCString(NULL, owner, kCFStringEncodingUTF8);
  for (CFIndex i = 0; i < CFArrayGetCount(list) && !found; ++i) {
    CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
    CFStringRef o = CFDictionaryGetValue(w, kCGWindowOwnerName);
    CFNumberRef l = CFDictionaryGetValue(w, kCGWindowLayer), op = CFDictionaryGetValue(w, kCGWindowOwnerPID);
    int opid = -1;
    if (op) CFNumberGetValue(op, kCFNumberIntType, &opid);
    CFDictionaryRef bd = CFDictionaryGetValue(w, kCGWindowBounds);
    int layer = -1;
    CGRect r = CGRectZero;
    if (l) CFNumberGetValue(l, kCFNumberIntType, &layer);
    if (bd) CGRectMakeWithDictionaryRepresentation(bd, &r);
    const int mine = pid > 0 ? opid == pid : o && CFStringCompare(o, want, 0) == kCFCompareEqualTo;
    if (mine && layer == 0 && r.size.width > 50 && r.size.height > 50) found = 1;
  }
  CFRelease(want);
  CFRelease(list);
  return found;
}

// The process whose name (p_comm, the executable's) is name; -1 when none.
static pid_t pidOf(const char *name)
{
  static pid_t pids[4096];
  const int n = proc_listallpids(pids, (int)sizeof pids);
  for (int i = 0; i < n && i < 4096; ++i) {
    char buf[2 * MAXCOMLEN + 1] = "";
    if (pids[i] > 0 && proc_name(pids[i], buf, sizeof buf) > 0 && !strcmp(buf, name)) return pids[i];
  }
  return -1;
}

static int launch(const char *app, double timeoutMs, int kill_, const char *exe)
{
  if (hasWindow(app, -1) < 0) {
    printf("CPUBENCH_LAUNCH app=%s ms=-1 why=no_window_list\n", app);
    return 2;
  }
  if (pidOf(app) > 0) {
    printf("CPUBENCH_LAUNCH app=%s ms=-1 why=already_running pid=%d\n", app, pidOf(app));
    return 2;
  }
  const double t0 = nowMs();
  pid_t pid;
  char *argv[] = {"/usr/bin/open", "-a", (char *)app, NULL};
  char *xargv[] = {(char *)exe, NULL};
  if (exe ? posix_spawn(&pid, exe, NULL, NULL, xargv, environ) : posix_spawn(&pid, "/usr/bin/open", NULL, NULL, argv, environ)) {
    printf("CPUBENCH_LAUNCH app=%s ms=-1 why=spawn\n", app);
    return 2;
  }
  int st = 0, opened = 0, polls = 0;
  double openMs = -1, procMs = -1, ms = -1;
  pid_t ap = -1;
  if (exe) opened = 1;   // the app itself: never waited for here
  while (nowMs() - t0 < timeoutMs) {
    if (!opened && waitpid(pid, &st, WNOHANG) == pid) {
      opened = 1;
      openMs = nowMs() - t0;
    }
    ++polls;
    if (ap <= 0 && (ap = pidOf(app)) > 0) procMs = nowMs() - t0;
    if (ap > 0 && hasWindow(app, ap) > 0) {
      ms = nowMs() - t0;
      break;
    }
    usleep(4000);
  }
  if (!opened) waitpid(pid, &st, 0);
  printf("CPUBENCH_LAUNCH app=%s ms=%.1f process_ms=%.1f open_rc_ms=%.1f polls=%d pid=%d via=%s t0_mach_s=%.6f%s\n", app, ms, procMs,
         openMs, polls, ap, exe ? "exec" : "open", t0 / 1000.0, ms < 0 ? " why=timeout" : "");
  if (kill_ && ap > 0) {
    kill(ap, SIGTERM);
    for (int i = 0; i < 200 && pidOf(app) > 0; ++i) usleep(10000);
  }
  return ms < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *test = argc > 1 ? argv[1] : "all";
  double scale = 1.0, gapMs = 250, timeoutMs = 20000;
  int n = 30, kill_ = 0, reps = 1;
  const char *q = "ui", *app = NULL, *exe = NULL;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atof(argv[++i]);
    else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--gap-ms") && i + 1 < argc) gapMs = atof(argv[++i]);
    else if (!strcmp(argv[i], "--qos") && i + 1 < argc) q = argv[++i];
    else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) timeoutMs = atof(argv[++i]);
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--close")) kill_ = 1;
    else if (!strcmp(argv[i], "--exec") && i + 1 < argc) exe = argv[++i];
    else if (!app && argv[i][0] != '-') app = argv[i];
  }
  if (scale <= 0 || scale > 100 || n < 1 || n > 1000 || reps < 1 || reps > 20) {
    fprintf(stderr, "usage: piano-cpubench all|st-int|st-fp|mt-int|burst|launch APP [--scale S] [--n N] [--gap-ms MS] [--qos ui|default|bg] "
                    "[--reps R] [--close]\n");
    return 64;
  }
  if (!strcmp(test, "now")) {
    printf("CPUBENCH_NOW mach_s=%.6f\n", nowMs() / 1000.0);
    return 0;
  }
  qos(q);
  printf("CPUBENCH_START test=%s scale=%.2f qos=%s cpus=%d\n", test, scale, q, ncpu());
  const int all = !strcmp(test, "all");
  int rc = 0;
  for (int r = 0; r < reps; ++r) {
    if (all || !strcmp(test, "st-int")) {
      const double t = nowMs();
      const uint64_t s = chainInt((uint64_t)(200e6 * scale), 7);
      printf("CPUBENCH test=st-int rep=%d ms=%.1f check=%llx\n", r, nowMs() - t, (unsigned long long)(s & 0xffff));
    }
    if (all || !strcmp(test, "st-fp")) {
      const double t = nowMs();
      const double s = chainFp((uint64_t)(100e6 * scale));
      printf("CPUBENCH test=st-fp rep=%d ms=%.1f check=%.3f\n", r, nowMs() - t, s);
    }
    if (all || !strcmp(test, "mt-int")) {
      const int c = ncpu();
      pthread_t th[64];
      struct Job jobs[64];
      const double t = nowMs();
      for (int i = 0; i < c; ++i) {
        jobs[i] = (struct Job){(uint64_t)(50e6 * scale), 0, q, 0};
        pthread_create(&th[i], NULL, mtWorker, &jobs[i]);
      }
      double first = 1e18, last = 0;
      for (int i = 0; i < c; ++i) {
        pthread_join(th[i], NULL);
        if (jobs[i].endMs < first) first = jobs[i].endMs;
        if (jobs[i].endMs > last) last = jobs[i].endMs;
      }
      printf("CPUBENCH test=mt-int rep=%d threads=%d ms=%.1f first_ms=%.1f\n", r, c, last - t, first - t);
    }
    if (all || !strcmp(test, "burst")) {
      double *lat = calloc((size_t)n, sizeof *lat), sum = 0;
      for (int i = 0; i < n; ++i) {
        usleep((useconds_t)(gapMs * 1000));
        const double t = nowMs();
        volatile uint64_t s = chainInt((uint64_t)(4e6 * scale), (uint64_t)i + 3);
        (void)s;
        lat[i] = nowMs() - t;
        sum += lat[i];
      }
      char first[160];
      snprintf(first, sizeof first, "%.1f,%.1f,%.1f,%.1f,%.1f", lat[0], n > 1 ? lat[1] : 0, n > 2 ? lat[2] : 0, n > 3 ? lat[3] : 0,
               n > 4 ? lat[4] : 0);
      qsort(lat, (size_t)n, sizeof *lat, cmpd);
      printf("CPUBENCH_BURST rep=%d n=%d gap_ms=%.0f steps=%.0f min_ms=%.2f p50_ms=%.2f p90_ms=%.2f max_ms=%.2f mean_ms=%.2f first5_ms=%s\n",
             r, n, gapMs, 4e6 * scale, lat[0], lat[n / 2], lat[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1], lat[n - 1], sum / n, first);
      free(lat);
    }
    if (!strcmp(test, "launch")) {
      if (!app) {
        fprintf(stderr, "launch needs an application name\n");
        return 64;
      }
      rc |= launch(app, timeoutMs, kill_, exe);
    }
  }
  printf("CPUBENCH_END rc=%d\n", rc);
  return rc;
}
