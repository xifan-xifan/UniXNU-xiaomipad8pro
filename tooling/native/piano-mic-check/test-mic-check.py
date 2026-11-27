# SPDX-License-Identifier: MIT
"""Exercise both file analyzers without opening an audio device."""

from argparse import Namespace
import array
from contextlib import redirect_stdout
import io
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import wave

import analyze

RATE = 48000
SECONDS = 3


def write_wav(path, samples, rate=RATE):
    pcm = array.array("h", samples)
    if sys.byteorder == "big":
        pcm.byteswap()
    with wave.open(str(path), "wb") as wav:
        wav.setparams((1, 2, rate, 0, "NONE", "not compressed"))
        wav.writeframes(pcm.tobytes())


def run(command, status, *markers):
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == status and all(marker in result.stdout for marker in markers), (
        f"{command}: exit={result.returncode}, expected {status} and {markers}\n"
        f"{result.stdout}\n{result.stderr}"
    )
    return result.stdout


def tone_measurements(output):
    line = next(line for line in output.splitlines() if line.startswith("tone_frequency_hz="))
    return {name: float(value) for name, value in (field.split("=") for field in line.split())}


def pcm_sample(value):
    return min(32767, max(-32768, round(value * 32768)))


def test_extensible_wav(folder):
    def chunk(name, payload):
        return struct.pack("<4sI", name, len(payload)) + payload + bytes(len(payload) % 2)

    def riff(*chunks):
        return chunk(b"RIFF", b"WAVE" + b"".join(chunks))

    def analyze_tone(path):
        with redirect_stdout(io.StringIO()) as output:
            status = analyze.analyze_wav(path, Namespace(mode="tone", seconds=1))
        assert status == 0, output.getvalue()
        return output.getvalue()

    rate = 16000
    standard = folder / "standard-pcm16.wav"
    write_wav(standard, (round(4096 * math.sin(math.tau * 1000 * i / rate)) for i in range(rate)), rate)
    with wave.open(str(standard), "rb") as wav:
        pcm = wav.readframes(rate)

    pcm_guid = bytes.fromhex("01000000 0000 1000 8000 00aa00389b71")
    fmt = struct.pack("<HHIIHHHHI16s", 0xfffe, 1, rate, rate * 2, 2, 16, 22, 16, 4, pcm_guid)
    data_chunk = chunk(b"data", pcm)
    extensible = folder / "extensible-pcm16.wav"
    fixture = riff(chunk(b"JUNK", b"\x00"), chunk(b"fmt ", fmt), data_chunk)
    extensible.write_bytes(fixture)
    # Read both fixtures directly so afconvert cannot change their format tags.
    assert analyze_tone(extensible) == analyze_tone(standard)

    for description, invalid in (
        ("truncated RIFF", fixture[:-1]),
        ("short extensible format", riff(chunk(b"fmt ", fmt[:-1]), data_chunk)),
        ("non-PCM GUID", riff(chunk(b"fmt ", fmt[:-1] + b"\x00"), data_chunk)),
        ("non-16-bit container", riff(chunk(b"fmt ", fmt[:14] + b"\x18\x00" + fmt[16:]), data_chunk)),
        ("partial PCM frame", riff(chunk(b"fmt ", fmt), chunk(b"data", pcm[:-1]))),
    ):
        try:
            analyze.read_pcm16_header(io.BytesIO(invalid))
        except ValueError:
            continue
        raise AssertionError(f"accepted {description}")


def test_capture_levels(binary, host, folder):
    def sine(name, frequency, amplitude, dc=0):
        path = folder / f"{name}.wav"
        write_wav(path, (pcm_sample(dc + amplitude * math.sin(math.tau * frequency * i / RATE))
                         for i in range(SECONDS * RATE)))
        return path

    clean = sine("clean-minus-18", 1000, 10 ** (-18 / 20))
    clipped = sine("clipped-1khz", 1000, 2.5)
    detuned = sine("clipped-detuned", 1002.1, 2.5)
    for path, frequency, clipped_marker in (
        (clean, 1000, "clipping_detected=no"),
        (clipped, 1000, "clipping_detected=yes"),
        (detuned, 1002.1, "clipping_detected=yes"),
    ):
        markers = ("tone_detected=yes", clipped_marker)
        host_tone = tone_measurements(run([*host, path, "--mode", "tone", "--seconds", str(SECONDS)],
                                          0, *markers, "grade=pass"))
        native_tone = tone_measurements(run([binary, "--analyze", path, "--tone"], 0, *markers, "result=ok"))
        assert all(abs(native_tone[field] - value) <= 0.02 for field, value in host_tone.items()), (
            host_tone, native_tone
        )
        assert abs(host_tone["tone_frequency_hz"] - frequency) <= 0.02, host_tone
        if path == clean:
            assert abs(host_tone["tone_dbfs"] - (-18 - 10 * math.log10(2))) < 0.02, host_tone
            assert host_tone["tone_snr_db"] >= 60, host_tone
        else:
            assert 6 <= host_tone["tone_sinad_db"] < 12 <= host_tone["tone_snr_db"], host_tone

    wrong = sine("wrong-500hz", 500, 0.8)
    wrong_clipped = sine("wrong-500hz-clipped", 500, 2.5, dc=0.5)
    outside_band = sine("outside-band", 979, 0.8)
    broadband = folder / "broadband-minus-3.wav"
    rng = random.Random(51)
    # For a uniform signal clipped to +/-1, mean square is 1 - 2/(3 * limit).
    limit = 2 / (3 * (1 - 10 ** (-3 / 10)))
    noise = [pcm_sample(rng.uniform(-limit, limit)) for _ in range(SECONDS * RATE)]
    rms_dbfs = 10 * math.log10(sum(value ** 2 for value in noise) / (len(noise) * 32768 ** 2))
    assert abs(rms_dbfs - (-3)) < 0.05
    write_wav(broadband, noise)
    for path in (wrong, wrong_clipped, outside_band, broadband):
        run([*host, path, "--mode", "tone", "--seconds", str(SECONDS)], 1, "tone_detected=no", "grade=fail")
        run([binary, "--analyze", path, "--tone"], 5, "tone_detected=no", "result=tone_missing")


def test_replay(host, folder):
    rate = 16000
    rng = random.Random(72)

    def take(name, noise=12, amplitude=0, frequency=1000, length=0.8, seconds=SECONDS):
        path = folder / f"c920-{name}.wav"
        samples = []
        for i in range(seconds * rate):
            at = i / rate
            signal = amplitude * math.sin(math.tau * frequency * at) if 0.65 <= at < 0.65 + length else 0
            samples.append(round(signal) + rng.randint(-noise, noise))
        write_wav(path, samples, rate)
        return path

    quiet = take("quiet")
    tone = take("tone", amplitude=180)
    contaminated = take("contaminated", amplitude=100)
    low_snr = take("low-snr", noise=360, amplitude=180)
    transient = take("transient", amplitude=1800, length=0.2)
    wrong = take("wrong", amplitude=220, frequency=1300)
    tone_near_control = take("tone-near-control", noise=1, amplitude=11)
    weak_control = take("weak-control", noise=1, amplitude=5)
    blank = take("blank", noise=0)
    short = take("short", seconds=2)
    truncated = take("truncated")
    truncated.write_bytes(truncated.read_bytes()[:-256])
    for replay, control, status, marker in (
        (tone, quiet, 0, "replay_tone_detected=yes"),
        (quiet, quiet, 1, "replay_tone_detected=no"),
        (low_snr, quiet, 1, "replay_tone_detected=no"),
        (transient, quiet, 1, "replay_tone_detected=no"),
        (wrong, quiet, 1, "replay_tone_detected=no"),
        (tone, contaminated, 1, "quiet_control_clean=no"),
        (tone_near_control, weak_control, 1, "replay_tone_detected=yes"),
        (tone, blank, 2, "grade=error"),
        (short, quiet, 2, "grade=error"),
        (truncated, quiet, 2, "grade=error"),
    ):
        run([*host, replay, "--mode", "replay", "--control", control, "--seconds", str(SECONDS)],
            status, marker, ("grade=pass", "grade=fail", "grade=error")[status])


def main(binary, stimulus, folder):
    host = [sys.executable, Path(__file__).with_name("analyze.py")]
    test_extensible_wav(folder)
    with wave.open(stimulus, "rb") as wav:
        assert (wav.getframerate(), wav.getnframes(), wav.getsampwidth(), wav.getnchannels()) == (
            RATE, 2 * RATE, 2, 1
        )
        assert wav.readframes(RATE // 2) == bytes(RATE)
        burst = array.array("h", wav.readframes(RATE))
        if sys.byteorder == "big":
            burst.byteswap()
        peak_dbfs = 20 * math.log10(max(map(abs, burst)) / 32768)
        assert abs(peak_dbfs - (-36)) < 0.05
        assert wav.readframes(RATE // 2) == bytes(RATE)

    run([binary, "--analyze", stimulus, "--tone"], 0, "tone_frequency_hz=1000", "duration_s=2.000")
    run([*host, stimulus, "--mode", "tone", "--seconds", "2"], 0, "tone_detected=yes")
    run([*host, stimulus, "--mode", "tone", "--seconds", "3"], 1, "grade=fail")
    run([binary, "2", "unused.m4a", "--tone"], 2, "result=usage")

    rng = random.Random(17)
    quiet, zero, wrong, transient, dc, loud = (
        folder / f"{stem}.wav" for stem in ("quiet", "zero", "wrong", "transient", "dc", "loud")
    )
    write_wav(quiet, (rng.randint(-100, 100) for _ in range(SECONDS * RATE)))
    write_wav(zero, [0] * (SECONDS * RATE))
    write_wav(wrong, (round(1000 * math.sin(math.tau * 1300 * i / RATE)) for i in range(SECONDS * RATE)))
    write_wav(transient, (round(8000 * math.sin(math.tau * 1000 * i / RATE)) if i < RATE // 5 else 0
                          for i in range(SECONDS * RATE)))
    write_wav(dc, [800] * (SECONDS * RATE))
    write_wav(loud, (rng.randint(-25000, 25000) for _ in range(SECONDS * RATE)))
    run([*host, quiet, "--mode", "quiet", "--seconds", str(SECONDS)],
        0, "tone_detected=no", "clipping_detected=no", "noise_floor_sane=yes", "grade=pass")
    for data, mode, status, marker in (
        (quiet, "tone", 1, "tone_detected=no"),
        (zero, "quiet", 1, "noise_floor_sane=no"),
        (zero, "tone", 1, "tone_detected=no"),
        (dc, "quiet", 1, "noise_floor_sane=no"),
        (loud, "quiet", 1, "noise_floor_sane=no"),
        (wrong, "tone", 1, "tone_detected=no"),
        (transient, "tone", 1, "tone_detected=no"),
    ):
        run([*host, data, "--mode", mode, "--seconds", str(SECONDS)], status, marker)
    for data in (zero, dc):
        run([binary, "--analyze", data], 4, "result=silence")
        run([binary, "--analyze", data, "--tone"], 4, "tone_detected=no", "result=silence")
    for data in (quiet, wrong, transient):
        run([binary, "--analyze", data, "--tone"], 5, "tone_detected=no", "result=tone_missing")
    test_capture_levels(binary, host, folder)

    m4a = folder / "tone.m4a"
    subprocess.run(["/usr/bin/afconvert", "-f", "m4af", "-d", "LEI16", stimulus, m4a], check=True)
    run([*host, m4a, "--mode", "tone", "--seconds", "2"], 0, "tone_frequency_hz=1000", "grade=pass")
    run([binary, "--analyze", m4a, "--tone"], 0, "tone_frequency_hz=1000", "result=ok")
    corrupt = folder / "corrupt.m4a"
    corrupt.write_bytes(b"invalid audio")
    run([*host, corrupt, "--mode", "tone"], 2, "grade=error")
    run([binary, "--analyze", corrupt, "--tone"], 1, "result=error")
    run([binary, "--replay", stimulus], 2, "result=usage")
    test_replay(host, folder)
    print("mic-check host tests passed (WAV, PCM M4A, capture grades and C920 replay controls)")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="piano-mic-check-test.") as directory:
        main(sys.argv[1], sys.argv[2], Path(directory))
