#!/usr/bin/env python3
"""Grade both panel DSPPs from a DPU snapshot: --expect off|warm.

PCC +0x10..+0x30 groups inputs R/G/B, with outputs R/G/B in each
group. Coefficients are signed 18-bit S3.15. Bypass has unity response.
Off requires unity gray within 2%; warm requires R >= G >= B, R >= 0.9.
This checks register response, not optical colour or the transfer curve.
"""
import argparse
import re


def responses(text):
    pattern = re.compile(
        r"^(dspp[01]): pcc en ([0-9a-fA-F]+) c "
        r"([0-9a-fA-F |]+?)(?:\s*\(|;|$)", re.MULTILINE
    )
    rows = []
    for match in pattern.finditer(text):
        words = [int(word, 16) & 0x3FFFF
                 for word in re.findall(r"[0-9a-fA-F]+", match[3])]
        if len(words) != 9:
            raise ValueError(f"{match[1]}: expected nine PCC words")
        q = [(word - 0x40000 if word & 0x20000 else word) / 32768
             for word in words]
        enabled = bool(int(match[2], 16) & 1)
        gray = ([sum(q[3 * i + o] for i in range(3)) for o in range(3)]
                if enabled else [1.0, 1.0, 1.0])
        rows.append((match[1], gray, enabled))
    if {name for name, _, _ in rows} != {"dspp0", "dspp1"}:
        raise ValueError("expected both dspp0 and dspp1")
    return rows


def passes(gray, expect):
    r, g, b = gray
    return (all(abs(v - 1.0) <= 0.02 + 1e-12 for v in gray) if expect == "off"
            else r >= g >= b and r >= 0.9)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot")
    parser.add_argument("--expect", choices=("off", "warm"), required=True)
    args = parser.parse_args()
    try:
        with open(args.snapshot, errors="replace") as snapshot:
            rows = responses(snapshot.read())
    except (OSError, ValueError) as error:
        print(f"PCC_GRAY invalid: {error}")
        return 2
    ok = all(passes(gray, args.expect) and (enabled or args.expect == "off")
             for _, gray, enabled in rows)
    print(f"PCC_GRAY {'PASS' if ok else 'FAIL'} {args.expect} " + "; ".join(
        f"{name}{'' if enabled else ' bypass'} R {gray[0]:.3f} G {gray[1]:.3f} B {gray[2]:.3f}"
        for name, gray, enabled in rows))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
