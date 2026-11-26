// SPDX-License-Identifier: BSD-2-Clause-Patent
// vaprobe: what V8's sandbox meets on this kernel's user address space (read-only for the system: it maps, touches and unmaps
// its own memory only). Known positive: a real 16 KiB macOS (128 TiB, ok, 200/200, all checks ok); known negative: k27 0010's
// 39-bit user space (y12k / uva47-ctl1: ~512 GiB, FAIL, ~146/200, the 100/127 TiB pages FAIL (KERN_NO_SPACE / KERN_INVALID_ADDRESS)).
//  1) the highest user address a 64 KiB VM_FLAGS_FIXED mapping is accepted at (bisection, 2^32 .. 2^47);
//  2) whether a 1 TiB + guards (1.34 TiB) and then 512/256/.../8 GiB PROT_NONE reservation fits (V8 Sandbox::Initialize order);
//  3) with the reservation R at base B kept, V8's EmulatedVirtualAddressSubspace::AllocateSharedPages: 64 KiB of a
//     mach_make_memory_entry_64 region mapped FIXED at a random 16 KiB-aligned hint in [B+R, B+1 TiB), else ANYWHERE, accepted only
//     inside [B+R, B+1 TiB); 10 tries per mapping, 200 mappings: the per-mapping success rate;
//  4) (k27 0075) the time to mmap and munmap the full 1344 GiB reservation untouched (on 4 KiB pages the pmap's remove loop steps
//     ~690k 2 MiB twigs), and again with 64 pages touched across it;
//  5) (k27 0075) a FIXED page at 100 TiB and at 127 TiB: mapped, written, read back;
//  6) (k27 0075) fork: the child reads the high page copy-on-write, writes its own value; the parent's page must keep its own.
// Plus machdep.virtual_address_size, vm.user_tte_root / user_tte_pages / arm_pt_root_size (absent sysctls print -1).
// Build: cc -O2 -o vaprobe vaprobe.c (arm64; plain arm64 is fine). Output: one line per result, "vaprobe: done" last.
#include <errno.h>
#include <mach/mach_error.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#define GiB (1ULL << 30)
#define TiB (1ULL << 40)

static mach_port_t entry64k(void) {
  memory_object_size_t sz = 65536; mach_port_t p = MACH_PORT_NULL;
  kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &sz, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &p, MACH_PORT_NULL);
  return kr == KERN_SUCCESS ? p : MACH_PORT_NULL;
}
static kern_return_t mapat(mach_vm_address_t *a, int flags, mach_port_t p) {
  return mach_vm_map(mach_task_self(), a, 65536, 0, flags, p, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
}
static double ms_since(uint64_t t0) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}
extern kern_return_t task_get_exc_guard_behavior(task_t, task_exc_guard_behavior_t *);
extern kern_return_t task_set_exc_guard_behavior(task_t, task_exc_guard_behavior_t);
static long long sysctl_int(const char *name) {
  int64_t v = 0; size_t sz = sizeof(v);
  if (sysctlbyname(name, &v, &sz, NULL, 0) != 0) return -1;
  return sz == 4 ? (long long)(int32_t)v : (long long)v;
}

// 5) one FIXED anonymous page at addr: map, write a pattern, read it back. Returns the page or NULL.
// Not mmap(MAP_FIXED): that overwrites, and vm_map_delete's preflight raises kGUARD_EXC_DEALLOC_GAP for the hole it replaces, which is
// FATAL (SIGKILL) for a platform binary (restrictions >= 1: this probe is one on the pad, its CDHash in the trust cache; uva47-qual1).
// mach_vm_allocate(VM_FLAGS_FIXED) takes the hole without a delete.
static volatile uint64_t *high_page(uint64_t addr, const char *label) {
  printf("high page %s (0x%llx): start\n", label, (unsigned long long)addr);
  mach_vm_address_t a = addr;
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, 16384, VM_FLAGS_FIXED);
  if (kr != KERN_SUCCESS) { printf("high page %s (0x%llx): FAIL mach_vm_allocate kr %d (%s)\n", label, (unsigned long long)addr, kr, mach_error_string(kr)); return NULL; }
  volatile uint64_t *q = (volatile uint64_t *)(uintptr_t)a;
  for (int i = 0; i < 2048; i++) q[i] = addr ^ (uint64_t)i * 0x9E3779B97F4A7C15ULL;
  int bad = 0;
  for (int i = 0; i < 2048; i++) bad += q[i] != (addr ^ (uint64_t)i * 0x9E3779B97F4A7C15ULL);
  printf("high page %s (0x%llx): %s (map + touch + read back, %d bad words)\n", label, (unsigned long long)addr, bad ? "FAIL" : "ok", bad);
  return bad ? NULL : q;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  // VAPROBE_FATAL_GUARDS=1 (host check): make this task's VM guards fatal, as a platform binary's are on the pad
  task_exc_guard_behavior_t gb = 0;
  if (getenv("VAPROBE_FATAL_GUARDS")) {
    task_get_exc_guard_behavior(mach_task_self(), &gb);
    task_set_exc_guard_behavior(mach_task_self(), (gb & ~TASK_EXC_GUARD_VM_ONCE) | TASK_EXC_GUARD_VM_DELIVER | TASK_EXC_GUARD_VM_FATAL);
  }
  gb = 0; task_get_exc_guard_behavior(mach_task_self(), &gb);
  printf("task exc guard behavior 0x%x (VM fatal %d)\n", gb, (gb & TASK_EXC_GUARD_VM_FATAL) != 0);
  printf("machdep.virtual_address_size %lld; vm.arm_pt_root_size %lld; vm.user_tte_root %lld; vm.user_tte_pages %lld\n",
         sysctl_int("machdep.virtual_address_size"), sysctl_int("vm.arm_pt_root_size"), sysctl_int("vm.user_tte_root"),
         sysctl_int("vm.user_tte_pages"));
  mach_port_t p = entry64k(); if (!p) { puts("entry FAIL"); return 1; }

  // 1)
  uint64_t lo = 1ULL << 32, hi = 1ULL << 47;   // lo accepted (or occupied), hi refused
  while (hi - lo > (1ULL << 24)) {
    uint64_t mid = ((lo + hi) / 2) & ~0x3fffULL; mach_vm_address_t a = mid; kern_return_t kr = mapat(&a, VM_FLAGS_FIXED, p);
    if (kr == KERN_SUCCESS) { mach_vm_deallocate(mach_task_self(), a, 65536); lo = mid; }
    else if (kr == KERN_NO_SPACE) lo = mid;      // occupied but inside the map
    else hi = mid;                               // KERN_INVALID_ADDRESS: beyond the map
  }
  printf("fixed-map limit ~ %.1f GiB (0x%llx)\n", lo / (double)GiB, (unsigned long long)lo);

  // 2) + 4)
  uint64_t full = 1024 * GiB + 32 * GiB + 288 * GiB;
  uint64_t t0 = mach_absolute_time();
  void *f = mmap(NULL, full, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  double t_map = ms_since(t0);
  printf("full sandbox reservation %llu GiB: %s\n", full / GiB, f == MAP_FAILED ? "FAIL (V8 falls back to partially reserved)" : "ok");
  if (f != MAP_FAILED) {
    printf("full reservation at 0x%llx (%.1f GiB)\n", (unsigned long long)(uintptr_t)f, (uintptr_t)f / (double)GiB);
    t0 = mach_absolute_time(); int rc = munmap(f, full); double t_unmap = ms_since(t0);
    printf("munmap 1344 GiB untouched: rc %d, mmap %.2f ms, munmap %.2f ms\n", rc, t_map, t_unmap);
    // again with 64 pages touched across the range (mprotect them RW first, as V8 commits its pages)
    f = mmap(NULL, full, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (f != MAP_FAILED) {
      int touched = 0;
      for (int i = 0; i < 64; i++) {
        char *pg = (char *)f + (full / 64) * (uint64_t)i;
        if (mprotect(pg, 16384, PROT_READ | PROT_WRITE) == 0) { pg[0] = (char)i; touched += pg[0] == (char)i; }
      }
      t0 = mach_absolute_time(); rc = munmap(f, full); t_unmap = ms_since(t0);
      printf("munmap 1344 GiB with %d/64 pages touched: rc %d, munmap %.2f ms\n", touched, rc, t_unmap);
    }
  }

  // 3)
  uint64_t R = 512 * GiB; void *b = MAP_FAILED;
  for (; R >= 8 * GiB; R /= 2) { b = mmap(NULL, R, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0); if (b != MAP_FAILED) break; }
  if (b == MAP_FAILED) { puts("no reservation >= 8 GiB"); }
  else {
    uint64_t B = (uint64_t)b, ub = B + R, ue = B + 1024 * GiB, span = ue - ub; int ok = 0, fixed_ok = 0, n = 200;
    printf("partial reservation R=%llu GiB at B=%.1f GiB; unmapped region [%.1f, %.1f) GiB\n", R / GiB, B / (double)GiB, ub / (double)GiB, ue / (double)GiB);
    srandom(12345);
    for (int m = 0; m < n; m++) {
      for (int i = 0; i < 10; i++) {
        mach_vm_address_t a = (ub + ((((uint64_t)random() << 31) ^ random()) % (span - 65536))) & ~0x3fffULL;
        if (mapat(&a, VM_FLAGS_FIXED, p) == KERN_SUCCESS) { fixed_ok++; mach_vm_deallocate(mach_task_self(), a, 65536); ok++; break; }
        a = 0; if (mapat(&a, VM_FLAGS_ANYWHERE, p) == KERN_SUCCESS) { int in = a >= ub && a + 65536 <= ue; mach_vm_deallocate(mach_task_self(), a, 65536); if (in) { ok++; break; } }
      }
    }
    printf("AllocateSharedPages emulation: %d/%d mappings succeeded (%d by the FIXED hint)\n", ok, n, fixed_ok);
    munmap(b, R);
  }

  // 5)
  volatile uint64_t *h100 = high_page(100 * TiB, "100 TiB");
  volatile uint64_t *h127 = high_page(127 * TiB, "127 TiB");

  // 6) fork: the child sees the parent's high page, then writes its own copy; the parent's must not change.
  volatile uint64_t *cow = h127 ? h127 : h100;
  if (!cow) { puts("fork COW on a high page: FAIL (no high page mapped)"); }
  else {
    uint64_t want = cow[7];
    pid_t pid = fork();
    if (pid == 0) {
      int ok = cow[7] == want;           // the parent's value, read through the child's (copied) tables
      cow[7] = 0xC0FFEE0000C0FFEEULL;    // copy-on-write
      ok = ok && cow[7] == 0xC0FFEE0000C0FFEEULL;
      _exit(ok ? 0 : 3);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid) printf("fork COW on the high page: FAIL (fork/wait errno %d)\n", errno);
    else printf("fork COW on the high page (0x%llx): %s (child status %d, parent's word %s)\n", (unsigned long long)(uintptr_t)cow,
                (WIFEXITED(st) && WEXITSTATUS(st) == 0 && cow[7] == want) ? "ok" : "FAIL", WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                cow[7] == want ? "kept" : "CHANGED");
  }
  if (h100) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)h100, 16384);
  if (h127) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)h127, 16384);
  printf("vm.user_tte_root %lld; vm.user_tte_pages %lld (after)\n", sysctl_int("vm.user_tte_root"), sysctl_int("vm.user_tte_pages"));
  puts("vaprobe: done");
  return 0;
}
