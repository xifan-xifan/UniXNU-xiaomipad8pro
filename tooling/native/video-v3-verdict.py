#!/usr/bin/env python3
"""Read V3 lifecycle evidence after the shared runner finishes; never access a tablet.

Console failure is a capture failure, not a driver verdict. Fall back to the
runner's recovered mailbox, and keep the firmware outcome separate from the
SMMU lifecycle result. Missing evidence always remains inconclusive/failed.
"""
import argparse
import re
from pathlib import Path

BANNER = r"PianoVideo: V3 evidence contract vid-v3b:"
TERMINAL = r"PianoVideo: bring-up: V3 (?:ok:|FAILED)"


def evaluate(folder, start=0):
    def read(name):
        path = folder / name
        try:
            if start and path.stat().st_mtime < start:
                return ""
            return path.read_text(errors="replace")
        except OSError:
            return ""

    positive = []
    for name in ("console.out", "mailbox.txt"):
        capture = read(name)
        banner = re.search(BANNER, capture)
        if banner:
            positive.append((name, capture[banner.start():]))
    complete = [(n, t) for n, t in positive if re.search(TERMINAL, t)]
    source, text = (complete or positive or [("none", "")])[0]
    rows = []

    def check(label, pattern, minimum=1, distinct=False):
        matches = list(re.finditer(pattern, text))
        hits = len({m[1] for m in matches}) if distinct else len(matches)
        rows.append((label, hits >= minimum, hits, minimum, pattern,
                     text[max(0, text.rfind("\n", 0, matches[0].start()) + 1):text.find("\n", matches[0].end()) if "\n" in text[matches[0].end():] else len(text)][:239] if matches else "(none)"))
        return matches

    # Stop before the harness's deliberate end-of-run reset. Require the driver's terminal line itself.
    terminal = re.search(TERMINAL, text)
    if terminal:
        text = text[:text.find("\n", terminal.end()) if "\n" in text[terminal.end():] else len(text)]
    check("this image's known-positive banner", BANNER)
    check("driver terminal result", TERMINAL)
    for phase in ("memory-prepared", "power-on", "S1-apply", "routes-effective", "release-begin", "device-quiesced",
                  "domain-release", "power-off", "DMA-freed", "release-end"):
        check(phase, r"PianoVideo: V3 lifecycle [0-9]+ " + re.escape(phase) + r" result 0(?:\s|$)")
    check("power ready", r"PianoVideo: V3 power ready 1 rails 1 bus 1 controller 1 codec 1 AXI 1/1")
    check("SCTLR required fields", r"PianoVideo: smmu: verify SCTLR want 00001027 got [0-9a-f]+ mask 00000027 mismatch 00000000 PASS")
    check("TCR2 defined fields", r"PianoVideo: smmu: verify TCR2 want [0-9a-f]+ got [0-9a-f]+ mask 0003801f mismatch 00000000 PASS")
    check("ASID/owner stability", r"PianoVideo: smmu: owners before-routes [0-9a-f]+->[0-9a-f]+ ASID [0-9]+ result 1")
    check("both S2CRs before matching", r"PianoVideo: smmu: verify S2CR .* mismatch 00000000 PASS", 2)
    check("both exact SID matches", r"PianoVideo: smmu: verify SMR want (8000194[07]) got \1 mask ffffffff mismatch 00000000 PASS",
          2, distinct=True)
    check("both matches invalidated", r"PianoVideo: smmu: verify release-SMR .* mismatch 00000000 PASS", 2)
    check("both legal S2CR releases", r"PianoVideo: smmu: release group .* result 1", 2)
    check("legal M0 bank release", r"PianoVideo: smmu: release bank [0-9]+ M0 1 .* result 1")
    check("release TLB completed", r"PianoVideo: smmu: release complete result 1 routes invalid bank M0 TLB complete")
    check("clean aggregate release", r"PianoVideo: V3 teardown result 0 PAS 0 power 0 smmu 1 dma-held 0 watchdog 0")
    outcome = check("separate firmware outcome", r"PianoVideo: V3 outcome first-HFI (PASS|FAIL) first-error (-?[0-9]+) stage ([0-9]+) teardown (-?[0-9]+) dma-held ([01])")
    bad = list(re.finditer(r"PE_halt_restart\(|MACH Reboot|panic\(cpu|Kernel data abort|Unhandled implementation specific error|(?:^|[ :])SError(?:[ :]|$)", text, re.M))
    rows.append(("no unexpected abort/reboot before terminal", not bad and bool(terminal), len(bad), 0, "abort/reboot before terminal", bad[0].group() if bad else "(none)"))
    power = re.search(r"V3 power ready 1", text)
    first_store = re.search(r"PianoVideo: write smmu\+", text)
    first_core = re.search(r"PianoVideo: V3 core (?:after-power|after-PAS|before-CTRL_INIT)", text)
    ordered = bool(power and first_store and first_core and power.start() < first_store.start() and power.start() < first_core.start())
    rows.append(("power precedes core and SMMU writes/TLB", ordered, int(ordered), 1, "power boundary order", "checked positions"))
    firmware = "unknown: no complete firmware outcome"
    if outcome:
        hfi, error, stage, teardown, held = outcome[-1].groups()
        firmware = f"first-HFI {hfi}; first-error {error}; stage {stage}; teardown {teardown}; DMA-held {held}"
        if hfi == "PASS":
            check("cold INIT/version and consumed command queue", r"PianoVideo: V3 first-HFI PASS: INIT success version received cmd-consumed 113 doorbells 3 fsr 00000000 watchdog 0")
        else:
            # A successful lifecycle can diagnose a later firmware gate. A bank/power failure cannot pass the SMMU transaction.
            check("exact later firmware failing stage", r"PianoVideo: V3 version error -[0-9]+ stage (?:3[0-8]|4[4-8]) received [01]")
            if not re.fullmatch(r"3[0-8]|4[4-8]", stage):
                rows.append(("failure belongs to firmware after applied routes", False, 0, 1, "later firmware stage", stage))
    if folder.name == "dry":
        status = "DRY ONLY"
    elif not positive:
        status = "INCONCLUSIVE: no current-boot V3 banner in console or mailbox"
    else:
        status = "PASS" if all(row[1] for row in rows) else "FAIL / INCOMPLETE"
    return source, status, firmware, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--start", type=int, default=0)
    args = parser.parse_args()
    source, status, firmware, rows = evaluate(args.folder, args.start)
    lines = ["## Verdict", f"- V3 lifecycle: {status}", f"- Evidence source: {source}", f"- Firmware: {firmware}",
             "- Tablet results only; a dry run is never hardware evidence.", "", "## Checks", ""]
    for label, ok, hits, minimum, pattern, sample in rows:
        lines.append(f"- {'PASS' if ok else 'FAIL'} {label}: hits={hits}, required={minimum}; /{pattern}/; sample: {sample}")
    content = "\n".join(lines) + "\n"
    try:
        (args.folder / "V3-VERDICT.md").write_text(content)
        result = args.folder / "RESULT.md"
        if result.is_file():
            original = result.read_text(errors="replace").split("\n<!-- video-v3-verdict -->", 1)[0]
            result.write_text(original + "\n<!-- video-v3-verdict -->\n" + content)
    except OSError as exc:
        parser.exit(1, f"V3 verdict: cannot write report: {exc}\n")
    print(f"V3 verdict: {status}; source={source}; {firmware}; {args.folder / 'V3-VERDICT.md'}")
    return 0 if status in ("PASS", "DRY ONLY") else 1


if __name__ == "__main__":
    raise SystemExit(main())
