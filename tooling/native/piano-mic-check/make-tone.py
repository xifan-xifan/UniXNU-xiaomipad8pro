#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the two-second WAV bundled with PianoMicCheck.app."""

import array
import math
import sys
import wave

RATE = 48000
FREQUENCY = 1000
PEAK_DBFS = -36


def write_tone(path):
    samples = array.array("h", [0]) * (2 * RATE)
    # Leave headroom for the tablet's built-in speaker-to-mic coupling.
    # The one-second burst has 20 ms ramps and quiet at both edges.
    ramp = RATE // 50
    amplitude = 32767 * 10 ** (PEAK_DBFS / 20)
    for frame in range(RATE):
        gain = min(1, frame / ramp, (RATE - 1 - frame) / ramp)
        samples[RATE // 2 + frame] = round(
            gain * amplitude * math.sin(math.tau * FREQUENCY * frame / RATE)
        )
    if sys.byteorder == "big":
        samples.byteswap()
    with wave.open(str(path), "wb") as wav:
        wav.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        wav.writeframes(samples.tobytes())


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT.wav", file=sys.stderr)
        return 2
    try:
        write_tone(sys.argv[1])
    except (OSError, wave.Error) as error:
        print(f"make-tone: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
