#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the shipped diffs and the A830 wire ABI using only the checkout and a host C++ compiler."""

import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
A830 = ROOT / "package/PianoA830/adreno830"
BUILD = HERE.parent / "build-turnip-a830.sh"
HUNK = re.compile(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(?: .*)?")
PRIVATE_PATH = re.compile(r"/(?:Users|home)/[^\s/]+|\bprivate/(?!var/(?:ngpu/debug/|tmp/))")

# Fixed little-endian ABI v2 records. Field offsets are derived from these fixed-width types
# using ARM64 natural alignment, then checked against the compiler's layout of the real header.
RECORDS = {
    "Params": "u32 abi_version; u32 input_bits; u32 output_bits; u32 flags; u64 max_bo_bytes",
    "BoAlloc": "u64 bytes; u32 align_log2; u32 contiguous",
    "BoAllocReply": "u64 handle",
    "BoImport": "u64 address; u64 bytes",
    "BoMap": "u64 handle; u64 iova; u32 permissions; u32 writeback; u32 shareability; u32 reserved",
    "BoMapReply": "u64 iova",
    "BoFree": "u64 handle",
    "SubmitBuffer": "u64 handle; u64 iova; u64 bytes",
    "Submit": "u32 buffer_count; u32 reserved; u64 queue; u64 wait_fence; u64 wait_timeout_ns; buffer buffers[128]",
    "SubmitReply": "u64 fence",
    "WaitFence": "u64 fence; u64 timeout_ns",
    "Timestamp": "u64 ticks",
    "Metadata": "u64 handle; u32 bytes; u32 reserved; char text[64]",
    "BoCpuMap": "u64 handle",
    "BoCpuMapReply": "u32 token; u32 reserved; u64 bytes",
    "BoCpuUnmap": "u64 handle; u32 token; u32 reserved",
    "Queue": "u64 queue",
    "QueueReply": "u64 queue",
    "Status": "u64 faults; u64 suspends",
    "DeviceInfo": (
        "u32 chip_id; u32 gmem_bytes; u64 gmem_base; u64 uche_trap_base; u64 va_start; u64 va_size; "
        "u32 highest_bank_bit; u32 ubwc_swizzle; u32 macrotile_mode; u32 ubwc_version; u32 flags; u32 reserved"
    ),
}

# IOKit method selectors in wire order, with request and reply records.
METHODS = (
    ("Query", None, "Params"),
    ("BoAlloc", "BoAlloc", "BoAllocReply"),
    ("BoMap", "BoMap", "BoMapReply"),
    ("BoFree", "BoFree", None),
    ("Submit", "Submit", "SubmitReply"),
    ("WaitFence", "WaitFence", None),
    ("GetTimestamp", None, "Timestamp"),
    ("GetStatus", None, "Status"),
    ("SetMetadata", "Metadata", None),
    ("GetMetadata", "Metadata", "Metadata"),
    ("CreateQueue", "Queue", "QueueReply"),
    ("CloseQueue", "Queue", None),
    ("BoCpuMap", "BoCpuMap", "BoCpuMapReply"),
    ("BoCpuUnmap", "BoCpuUnmap", None),
    ("GetDeviceInfo", None, "DeviceInfo"),
    ("BoImport", "BoImport", "BoAllocReply"),
)

LIBRARIES = ("libANGLE-shared.dylib", "libGLESv2.dylib", "libwebgpu_dawn.dylib")
FRAMEWORKS = tuple(f"/{name} Framework.framework/" for name in (
    "Google Chrome", "Chromium", "Microsoft Edge", "Brave Browser", "Electron",
))


def read(path):
    return path.read_bytes().decode("utf-8")


def parse_patch(path, preimage=False):
    """Read the unified diffs accepted by the builder and return one side of each hunk."""
    source = read(path)
    lines = source.split("\n")
    i = 0
    files = {}

    def require(ok, message):
        if not ok:
            raise ValueError(f"{path.relative_to(ROOT)}:{i + 1}: {message}")

    require(lines[-1] == "", "missing final newline")
    for i, line in enumerate(lines):
        # The single space is the required unified-diff prefix for an empty context line.
        require(line == " " or line == line.rstrip(), "trailing whitespace")
        require(not PRIVATE_PATH.search(line), "private path")
    i = next((n for n, line in enumerate(lines) if line.startswith(("diff -ruN ", "--- "))), len(lines) - 1)
    require(i < len(lines) - 1, "missing file diff")
    while i < len(lines) - 1:
        diff = None
        if lines[i].startswith("diff -ruN "):
            diff = lines[i].split()[2:]
            i += 1
        require(i + 1 < len(lines) - 1, "truncated file headers")
        old, new = lines[i:i + 2]
        require(old.startswith("--- ") and new.startswith("+++ "), "expected paired file headers")
        old = old[4:].split("\t", 1)[0].removeprefix("a/")
        new = new[4:].split("\t", 1)[0].removeprefix("b/")
        require(old == new and re.fullmatch(r"[\w./-]+", new) and
                all(part not in ("", ".", "..") for part in new.split("/")), "unexpected file path")
        require(diff is None or diff == ["a/" + old, "b/" + new], "diff command disagrees with file headers")
        require(new not in files, "duplicate file diff")
        i += 2
        hunks = []
        old_end = new_end = 0
        while match := HUNK.fullmatch(lines[i]):
            old_start, old_count, new_start, new_count = (int(n or 1) for n in match.groups())
            require(old_start >= old_end and new_start >= new_end and old_count + new_count,
                    "empty, overlapping or out-of-order hunk")
            old_end, new_end = old_start + old_count, new_start + new_count
            body = []
            i += 1
            while old_count or new_count:
                require(i < len(lines) - 1, "truncated hunk")
                line = lines[i] or " "  # GNU patch and git apply accept bare empty context lines.
                require(line.startswith((" ", "+", "-")), "invalid or truncated hunk line")
                if line[0] != "+":
                    old_count -= 1
                if line[0] != "-":
                    new_count -= 1
                if line[0] != ("+" if preimage else "-"):
                    body.append(line[1:])
                require(old_count >= 0 and new_count >= 0, "incorrect hunk counts")
                i += 1
            hunks.append("\n".join(body))
        require(hunks, "missing hunks")
        files[new] = "\n\n".join(hunks)
        while i < len(lines) - 1 and not lines[i]:
            i += 1
    return files


def layout_checks():
    checks = []
    types = {"u32": ("uint32_t", 4, 4), "u64": ("uint64_t", 8, 8),
             "char": ("char", 1, 1), "buffer": ("NativeAdrenoAbiSubmitBuffer", 24, 8)}
    for name, fields in RECORDS.items():
        record = "NativeAdrenoAbi" + name
        size = 0
        alignment = 1
        for declaration in fields.split("; "):
            kind, member = declaration.split()
            member, _, suffix = member.partition("[")
            cpp_type, width, align = types[kind]
            count = int(suffix[:-1]) if suffix else 1
            size = (size + align - 1) // align * align
            alignment = max(alignment, align)
            array = "[" + suffix if suffix else ""
            checks.extend((
                f'static_assert(std::is_same_v<decltype({record}::{member}), {cpp_type}{array}>, "{record}.{member} type");',
                f'static_assert(offsetof({record}, {member}) == {size}, "{record}.{member} offset");',
            ))
            size += width * count
        size = (size + alignment - 1) // alignment * alignment
        checks.append(f'static_assert(sizeof({record}) == {size} && alignof({record}) == {alignment}, "{record} layout");')
    return "\n".join(checks)


class TurnipContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.series = {name: sorted((HERE / name).glob("*.patch")) for name in ("mesa", "frontend", "plugin")}
        cls.builder = read(BUILD)
        cls.client = read(A830 / "NativeAdrenoUserClient.cpp")

    def patch(self, series, number, preimage=False):
        paths = [path for path in self.series[series] if path.name.startswith(f"{number:04d}-")]
        self.assertEqual(len(paths), 1, (series, number))
        return parse_patch(paths[0], preimage=preimage)

    def capture(self, pattern, source):
        match = re.search(pattern, source, re.M)
        self.assertIsNotNone(match, f"missing contract: {pattern}")
        return match[1]

    def compile_and_run(self, source):
        with tempfile.TemporaryDirectory(prefix="turnip-abi-") as directory:
            fixture = Path(directory) / "contract.cpp"
            binary = fixture.with_suffix("")
            fixture.write_text(source, encoding="utf-8")
            for command in (["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(A830),
                             "-I", str(HERE), str(fixture), "-o", str(binary)], [str(binary)]):
                process = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)

    def test_patch_series(self):
        provenance = read(HERE.parent / "UPSTREAM")
        names = re.findall(r"\b\d{4}-[\w-]+\.patch\b", provenance)
        for series, first, last in (("mesa", 1, 15), ("frontend", 1, 11), ("plugin", 12, 12)):
            patches = self.series[series]
            with self.subTest(series=series):
                self.assertRegex(self.builder, rf'"\$P"/{series}/\*\.patch')
                actual = [path.name for path in patches]
                self.assertEqual([name for name in names if name in actual], actual)
                self.assertEqual([int(name[:4]) for name in actual], list(range(first, last + 1)))
            for path in patches:
                with self.subTest(patch=path.name):
                    self.assertRegex(path.name, r"^\d{4}-[a-z0-9-]+\.patch$")
                    self.assertTrue(parse_patch(path))
        self.assertCountEqual(names, [path.name for patches in self.series.values() for path in patches])

    def test_wire_abi(self):
        header = "package/PianoA830/adreno830/NativeAdrenoABI.h"
        self.assertIn(f'[ -f "$D/{header}" ] && ABI=$D/{header}', self.builder)
        self.assertIn('put "$ABI" "$O/abi/NativeAdrenoABI.v2.h"', self.builder)
        self.assertIn('#include "NativeAdrenoABI.v2.h"', self.builder)
        self.assertIn('#include "../adreno830/NativeAdrenoABI.h"', read(A830.parent / "abi/NativeAdrenoABI.h"))
        fallback = self.capture(r"""grep -q NativeAdrenoParamsGpuPrivate "\$ABI" \|\| echo '([^']+)'""", self.builder)
        constants = {
            "AbiVersion": 2, "AbiMaxBuffers": 128, "AbiMaxBoBytesShift": 28,
            "AbiMetadataBytes": 64, "AbiMaxQueues": 8, "ParamsHostImport": 1, "ParamsGpuPrivate": 4,
            "BoAllocGpuPrivate": 4, "DeviceClientIova": 1, "DeviceCachedCoherent": 2,
        }
        constants.update({f"Method{name}": n for n, (name, _, _) in enumerate(METHODS, 1)})
        checks = "\n".join(f"static_assert(NativeAdreno{name} == {value});" for name, value in constants.items())
        selectors = self.patch("mesa", 3)["src/freedreno/vulkan/tu_knl_darwin.cc"]
        self.assertEqual(re.findall(r'case NativeAdrenoMethod(\w+): return "(\w+)";', selectors),
                         [(name, name) for name, _, _ in METHODS])
        sizes = [self.capture(rf"(size_t NativeAdrenoUserClient::{name}Size\([^\n]*\n\{{[\s\S]*?^\}})",
                              self.client) for name in ("input", "output")]
        size_checks = []
        for method, request, reply in METHODS:
            for direction, record in (("input", request), ("output", reply)):
                size = f"sizeof(NativeAdrenoAbi{record})" if record else "0"
                size_checks.append(f"assert(client.{direction}Size(NativeAdrenoMethod{method}) == {size});")
        self.compile_and_run(f"""
#include <cassert>
#include <bit>
#include <cstddef>
#include <type_traits>
#include "NativeAdrenoABI.h"
#include "NativeAdrenoABICompat.h"
namespace kmd {{
#include "../abi/NativeAdrenoABI.h"
}}
namespace fallback {{ {fallback} }}
{checks}
{layout_checks()}
static_assert(std::endian::native == std::endian::little);
static_assert(NativeAdrenoParamsOwnedCoherent == 2);
static_assert(unsigned(kmd::NativeAdrenoParamsOwnedCoherent) == unsigned(NativeAdrenoParamsOwnedCoherent));
static_assert(unsigned(fallback::NativeAdrenoParamsGpuPrivate) == unsigned(NativeAdrenoParamsGpuPrivate));
static_assert(unsigned(fallback::NativeAdrenoBoAllocGpuPrivate) == unsigned(NativeAdrenoBoAllocGpuPrivate));
struct NativeAdrenoUserClient {{
    size_t inputSize(uint32_t);
    size_t outputSize(uint32_t);
}};
{sizes[0]}
{sizes[1]}
int main() {{
    NativeAdrenoUserClient client;
    {" ".join(size_checks)}
    assert(client.inputSize(0) == SIZE_MAX && client.outputSize(17) == SIZE_MAX);
}}
""")

    def test_iokit_transport(self):
        handshake = read(A830.parent / "abi/abi_handshake.cpp")
        shim = read(A830 / "tests/hostsim/host_iokit_shim.cpp")
        for path, pattern, expected in (
            ("NativeAdrenoPublish.cpp", r"NativeAdrenoTunnelMagic\s*=\s*(0x[0-9a-f]+)", 0x4E544E4C),
            ("NativeAdrenoPublish.h", r"NativeAdrenoPluginOpenType\s*=\s*(0x[0-9a-f]+)", 0x4147),
        ):
            self.assertEqual(int(self.capture(pattern, read(A830 / path)), 16), expected)
        for source in (handshake, shim):
            self.assertRegex(source, r"\b(?:k)?TunnelMagic\s*=\s*0x4e544e4c\b")
        self.assertRegex(handshake, r"IOServiceOpen\(service, mach_task_self\(\), 0x4147, &conn\)")
        self.assertRegex(shim, r"if\(service==2 && type!=0x4147\)")

    def test_turnip_private_request(self):
        patch = self.patch("mesa", 15)
        backend = "src/freedreno/vulkan/tu_knl_darwin.cc"
        darwin = patch[backend]
        capacity = self.patch("mesa", 12)[backend]
        # 0012 rewrites the retry immediately before BoAlloc. Require 0015's preimage
        # to match that output so applying 0015 cannot depend on patch fuzz.
        before = self.patch("mesa", 15, preimage=True)[backend]
        context = self.capture(r"([^\n]+\n[^\n]+\n\n +NativeAdrenoAbiBoAlloc alloc = [^\n]+\n"
                               r" +NativeAdrenoAbiBoAllocReply reply = [^\n]+)", before)
        self.assertIn(context, capacity)
        self.assertRegex(capacity, r"dw_call\(NativeAdrenoMethodBoImport, &import, sizeof\(import\), &reply, sizeof\(reply\)\)")
        self.assertRegex(capacity, r"dw_call\(NativeAdrenoMethodBoMap, &map, sizeof\(map\), &mapped, sizeof\(mapped\)\)")
        self.assertRegex(capacity, r"\.permissions = 3u, \.writeback = 0, \.shareability = 2")
        alloc = self.capture(r"(const bool gpu_private = [^;]+;[\s\S]*?const bool cacheable = [^;]+;)", darwin)
        request = self.capture(r"(NativeAdrenoAbiBoAlloc alloc = \{[\s\S]*?\};)", darwin)
        flags = self.capture(r"(enum tu_bo_alloc_flags flags = [^;]+;[\s\S]*?TU_BO_ALLOC_GPU_PRIVATE\);)",
                             patch["src/freedreno/vulkan/tu_device.cc"])
        self.assertRegex(patch["src/freedreno/vulkan/tu_device.cc"],
                         r"tu_bo_init_new\([^;]+bo_size,\s*flags,\s*\"scratch\"\)")
        self.assertIn("dw_call(NativeAdrenoMethodBoAlloc, &alloc, sizeof(alloc), &reply, sizeof(reply))", darwin)
        self.assertRegex(self.client, r"const bool gpuPrivate\s*=\s*request->contiguous==NativeAdrenoBoAllocGpuPrivate;")
        self.assertRegex(self.client, r"const bool tryTier\s*=\s*gpuPrivate && native_adreno_wc_enabled && "
                         r"NativeAdrenoMemory::tierEligible\(request->bytes\);")
        self.assertRegex(self.client, r"if\s*\(native_adreno_wc_enabled && NativeAdrenoMemory::tierEligible"
                         r"\(a830tier::kSlab\)\)\s*params->flags\s*\|=\s*NativeAdrenoParamsGpuPrivate;")
        self.compile_and_run(f"""
#include <cassert>
#include <initializer_list>
#include "NativeAdrenoABI.h"
enum tu_bo_alloc_flags {{
    TU_BO_ALLOC_ALLOW_DUMP = 1 << 0, TU_BO_ALLOC_GPU_READ_ONLY = 1 << 1,
    TU_BO_ALLOC_REPLAYABLE = 1 << 2, TU_BO_ALLOC_INTERNAL_RESOURCE = 1 << 3,
    {patch["src/freedreno/vulkan/tu_knl.h"].split('};', 1)[0]}
}};
static_assert(TU_BO_ALLOC_GPU_PRIVATE == 1 << 7);
#define FD_RD_DUMP(option) dumping
static unsigned scratch_flags(bool dumping) {{
    {flags}
    return flags;
}}
int main() {{
    assert(scratch_flags(true) == TU_BO_ALLOC_INTERNAL_RESOURCE);
    assert(scratch_flags(false) == (TU_BO_ALLOC_INTERNAL_RESOURCE | TU_BO_ALLOC_GPU_PRIVATE));
    // Exercise the real 0015 predicate with every allocation flag and backend combination.
    const bool cache_enabled = true;
    const uint64_t alloc_size = 2 << 20;
    for (unsigned flags = 0; flags < 256; ++flags)
      for (bool lazy_vma : {{false, true}})
        for (bool virtgpu : {{false, true}})
          for (uint32_t capability : {{0u, 4u}}) {{
            struct {{ bool virtgpu; NativeAdrenoAbiParams params; }} dw{{virtgpu, {{2, 38, 36, 1u | capability, 0}}}};
            {alloc}
            {request}
            const unsigned required = TU_BO_ALLOC_GPU_PRIVATE | TU_BO_ALLOC_INTERNAL_RESOURCE;
            const unsigned forbidden = TU_BO_ALLOC_DMABUF | TU_BO_ALLOC_SHAREABLE |
                                       TU_BO_ALLOC_ALLOW_DUMP | TU_BO_ALLOC_REPLAYABLE;
            const bool expected = (flags & required) == required && !(flags & forbidden) &&
                                  !lazy_vma && !virtgpu && capability != 0;
            assert(alloc.bytes == alloc_size && alloc.align_log2 == 12);
            assert(alloc.contiguous == (expected ? 4u : 0u));
            assert(cacheable == (!expected && !lazy_vma && !(flags & TU_BO_ALLOC_REPLAYABLE)));
          }}
}}
""")

    def test_default_tier_and_cpu_views(self):
        board = read(A830.parent / "board/PianoA830.cpp")
        self.assertRegex(board, r"const unsigned mode\s*=\s*a830tier::bootArgMode\(PE_boot_args\(\)\);\s*if\s*\(!mode\)\s*return;")
        memory = read(A830 / "NativeAdrenoMemory.cpp")
        self.assertRegex(memory, r"unsigned mode\s*=\s*0,\s*active\s*=\s*0,")
        cpu_map = self.capture(r"(IOReturn NativeAdrenoUserClient::cpuMap\([\s\S]+?^\})", self.client)
        self.assertRegex(cpu_map, r"if\s*\(entry->memory\.tier\(\)\)\s*\{[^}]*return kIOReturnUnsupported;\s*\}")
        self.assertLess(cpu_map.index("entry->memory.tier()"), cpu_map.index("++entry->mapping_leases"))
        token_lookup = self.capture(r"(NativeAdrenoMemory \*NativeAdrenoUserClient::memoryForToken\([\s\S]+?^\})", self.client)
        self.assertRegex(token_lookup, r"entry\.mapping_leases\s*&&\s*!entry\.memory\.tier\(\)\s*\?\s*&entry\.memory\s*:\s*nullptr")
        publish = read(A830 / "NativeAdrenoPublish.cpp")
        self.assertRegex(publish, r"NativeAdrenoMemory \*backing\s*=\s*live_\?box_->core\.memoryForToken\(type\):nullptr;")
        self.compile_and_run("""
#include <cassert>
#include "A830GpuTier.h"
#include "frontend/SurfaceResidency.h"
int main() {
    using a830tier::bootArgMode;
    assert(bootArgMode(nullptr) == 0 && bootArgMode("") == 0);
    assert(bootArgMode("piano-gpu=2 piano-gpu-mem=paged") == 0);
    assert(bootArgMode("piano-gpu-tier=0") == 0 && bootArgMode("piano-gpu-tier=01") == 0);
    assert(bootArgMode("xpiano-gpu-tier=1 piano-gpu-tier=1x") == 0);
    assert(bootArgMode("piano-gpu-tier=1\\tpiano-gpu-tier=0") == 0);
    assert(bootArgMode("piano-gpu-tier=0\\tpiano-gpu-tier=1") == 1);
    assert(bootArgMode("piano-gpu-tier=2") == 2 && bootArgMode("piano-gpu-tier=3") == 0);
    static_assert(a830tier::kPage == 4096 && a830tier::kSlab == 2 * 1024 * 1024);
    assert(!a830tier::eligible(0, UINT64_MAX));
    assert(!a830tier::eligible(1, a830tier::kSlab - 1) && a830tier::eligible(1, a830tier::kSlab));
    assert(a830tier::eligible(2, a830tier::kPage));
    assert(!SurfaceResidencyEnabled("") && !SurfaceResidencyEnabled("piano-surface-residency=0"));
    assert(SurfaceResidencyEnabled("piano-surface-residency=1"));
}
""")

    def test_turnip_overrides(self):
        backend = self.patch("mesa", 1)["src/freedreno/vulkan/tu_knl_darwin_virtgpu.cc"]
        enabled = self.capture(r"^(bool\ndwv_enabled\([\s\S]*?^\})", backend)
        clear = self.patch("mesa", 13)["src/freedreno/vulkan/tu_knl_darwin.cc"]
        self.assertRegex(clear, r"char value\[32\]\s*=\s*\{\};[\s\S]*?"
                         r'darwin_bo_clear_late_mode\s*=\s*!strcmp\(value, "late"\);')
        self.compile_and_run(f"""
#include <cassert>
#include <cstring>
static const char *overrideValue;
static const char *option(const char *name) {{
    assert(strcmp(name, "TU_DARWIN_VIRTGPU") == 0);
    return overrideValue;
}}
static bool dwv_board_default() {{ return true; }}
#define getenv option
{enabled}
int main() {{
    assert(!dwv_enabled());
    overrideValue = "";
    assert(!dwv_enabled());
    overrideValue = "0";
    assert(!dwv_enabled());
    overrideValue = "1";
    assert(dwv_enabled());
    overrideValue = "probe";
    assert(dwv_enabled());
}}
""")

    def test_mac2_allowlist(self):
        frontend = next(iter(self.patch("frontend", 11).values()))
        kinds = self.capture(r"^(enum FamilyCaller [^\n]+)", frontend)
        matcher = self.capture(r"^(static bool browserGpuImage\([\s\S]*?^\})", frontend)
        self.assertCountEqual(re.findall(r'"([^"]+)"', matcher), LIBRARIES + FRAMEWORKS)
        classify = self.capture(r"^( if\(dladdr\(address,&caller\)[\s\S]*?^ \})", frontend)
        dispatch = self.capture(r"^(static BOOL familyForCaller\([\s\S]*?^ \})", frontend)
        positives = ["/Frameworks/" + name for name in LIBRARIES]
        positives += ["/App" + framework + "Versions/A/GPU" for framework in FRAMEWORKS]
        negatives = ["/App/WindowServer", "/App/QuartzCore.framework/QuartzCore",
                     "/App/WebGPU.framework/WebGPU", "/App/RenderBox.framework/RenderBox",
                     "/App/Google Chrome Helper (GPU)", "", *LIBRARIES]
        negatives += [path + extension for path in positives[:len(LIBRARIES)] for extension in (".backup", "/Tool")]
        negatives += ["/App/" + framework.strip("/") + ".backup/GPU" for framework in FRAMEWORKS]
        cases = [(path, True) for path in positives] + [(path, False) for path in negatives]
        calls = [f"check({json.dumps(path)}, {str(expected).lower()});" for path, expected in cases]
        self.compile_and_run(f"""
#include <cassert>
#include <cstring>
using BOOL = bool;
using id = void *;
using SEL = void *;
using NSInteger = long;
constexpr BOOL YES = true, NO = false;
{kinds}
{matcher}
static const char *image;
struct Dl_info {{ const char *dli_fname; }};
static bool dladdr(const void *, Dl_info *caller) {{ caller->dli_fname = image; return image != nullptr; }}
static FamilyCaller familyCaller(const void *address) {{
    Dl_info caller{{}};
    FamilyCaller kind = FamilyCallerOther;
    {classify}
    return kind;
}}
static bool originalAnswer;
static BOOL gFamilyAnswer(id, SEL, NSInteger) {{ return originalAnswer; }}
{dispatch}
    return gFamilyAnswer(self, _cmd, family);
}}
static void check(const char *path, bool mac2) {{
    image = path;
    originalAnswer = false;
    assert(familyForCaller(nullptr, nullptr, 2002) == mac2);
    assert(!familyForCaller(nullptr, nullptr, 2001));
    originalAnswer = true;
    assert(familyForCaller(nullptr, nullptr, 2002));
}}
int main() {{
    check(nullptr, false);
    {" ".join(calls)}
}}
""")

    def test_frontend_census_abi(self):
        for producer, consumer, symbol, result, slots, output in (
            (8, 3, "tu_darwin_limits", "void", 12, "gTurnipLimits"),
            (12, 6, "tu_darwin_capacity_counts", "bool", 14, "bc"),
        ):
            with self.subTest(symbol=symbol):
                darwin = self.patch("mesa", producer)["src/freedreno/vulkan/tu_knl_darwin.cc"]
                frontend = next(iter(self.patch("frontend", consumer).values()))
                self.assertRegex(darwin, rf'{result}\s+{symbol}\(uint64_t out\[{slots}\]\)')
                self.assertRegex(darwin, rf'memset\(out, 0, {slots} \* sizeof\(')
                self.assertRegex(frontend, rf'{result} \(\*\)\(uint64_t \*\)\)dlsym\(turnip,"{symbol}"\)')
                self.assertIn(f"uint64_t {output}[{slots}]", frontend)
                indices = re.findall(r"\bout\[(\d+)\]\s*=", darwin)
                self.assertEqual([int(i) for i in indices], list(range(11 if producer == 8 else 14)))
                if producer == 12:
                    self.assertIn("out[13] = 1;", darwin)
                    self.assertEqual(re.findall(r"\bbc\[(\d+)\]", frontend), ["14", *map(str, range(14))])


if __name__ == "__main__":
    unittest.main()
