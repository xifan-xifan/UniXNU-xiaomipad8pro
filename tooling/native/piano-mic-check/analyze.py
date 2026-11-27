#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Grade a pulled piano-mic-check take or a pair of C920 replay recordings."""

import argparse
import math
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pcm16_wav import read_pcm16_header, read_samples, validate_wav

INPUT_MIN_DBFS = -85.0
TONE_MIN_DBFS = -55.0
TONE_MIN_SNR_DB = 12.0
CLIPPED_MIN_SINAD_DB = 6.0
TONE_MIN_HZ = 980
TONE_MAX_HZ = 1020
REPLAY_MIN_DBFS = -75.0
REPLAY_MIN_MARGIN_DB = 10.0
C920_RATE = 16000
QUIET_MAX_DBFS = -25.0
QUIET_PEAK_MAX_DBFS = -6.0
PCM_SCALE = 32768
PCM_POWER_SCALE = PCM_SCALE ** 2


def power_db(power):
    return 10 * math.log10(max(power, 1e-20))


def ac_power(samples):
    mean = sum(samples) / len(samples)
    power = sum(value * value for value in samples) / len(samples)
    return mean, (power - mean * mean) / PCM_POWER_SCALE


class SineFit(NamedTuple):
    frequency: float
    power: float
    cosine_gain: float
    sine_gain: float
    cosine_mean: float


def fit_sine(samples, rate, frequency):
    """Fit a sine and cosine with DC removed, even between DFT bins."""
    count = len(samples)
    step = math.tau * frequency / rate
    cosine, sine = math.cos(step), math.sin(step)
    coefficient = 2 * cosine
    previous = older = 0.0
    for sample in samples:
        older, previous = previous, sample + coefficient * previous - older

    # Rotate the Goertzel result to the center of the window. The centered sine
    # is odd and the cosine is even, so their least-squares terms decouple.
    phase = step * (count - 1) / 2
    real, imag = previous - older * cosine, older * sine
    cosine_dot = real * math.cos(phase) + imag * math.sin(phase)
    sine_dot = imag * math.cos(phase) - real * math.sin(phase)
    cosine_mean = math.sin(count * step / 2) / (count * math.sin(step / 2))
    cosine_energy = (count + math.sin(count * step) / sine) / 2
    sine_gain = sine_dot / (count - cosine_energy)
    cosine_gain = cosine_dot / (cosine_energy - count * cosine_mean ** 2)
    power = (cosine_gain * cosine_dot + sine_gain * sine_dot) / count
    return SineFit(frequency, power, cosine_gain, sine_gain, cosine_mean)


def remove_sine(samples, rate, fit):
    step = math.tau * fit.frequency / rate
    center = (len(samples) - 1) / 2
    for i in range(len(samples)):
        phase = step * (center - i)
        samples[i] -= (fit.cosine_gain * (math.cos(phase) - fit.cosine_mean)
                       + fit.sine_gain * math.sin(phase))


class Tone(NamedTuple):
    frequency: float
    level: float
    sinad: float
    snr: float


def tone_in_band(frequency):
    # Keep the frequency test consistent with the reported precision.
    return TONE_MIN_HZ - 0.005 <= frequency <= TONE_MAX_HZ + 0.005


def clipped_samples(samples):
    return samples.count(-PCM_SCALE) + samples.count(PCM_SCALE - 1)


def tone_in_window(samples, rate, mean, ac, allow_clipping=False):
    # Guard bins keep a strong tone just outside the band from pinning the fit
    # to the edge. Refine the best bin so nearby leakage stays in the tone fit.
    signal = [(sample - mean) / PCM_SCALE for sample in samples]
    best = max((fit_sine(signal, rate, hz) for hz in range(TONE_MIN_HZ - 5, TONE_MAX_HZ + 6, 5)),
               key=lambda fit: fit.power)
    step = 2.5
    for _ in range(6):
        lower = fit_sine(signal, rate, best.frequency - step)
        upper = fit_sine(signal, rate, best.frequency + step)
        best = max(best, lower, upper, key=lambda fit: fit.power)
        step /= 2
    lower = fit_sine(signal, rate, best.frequency - step)
    upper = fit_sine(signal, rate, best.frequency + step)
    curvature = lower.power + upper.power - 2 * best.power
    if curvature < 0:
        shift = min(1, max(-1, (lower.power - upper.power) / (2 * curvature)))
        refined = fit_sine(signal, rate, best.frequency + step * shift)
        best = max(best, refined, key=lambda fit: fit.power)

    level = power_db(best.power)
    residual = ac - best.power
    sinad = level - power_db(residual)
    # Only remove harmonics for substantial clipping with a dominant
    # fundamental. Leave lower-frequency and broadband energy in the residual.
    if (allow_clipping and tone_in_band(best.frequency) and sinad >= CLIPPED_MIN_SINAD_DB
            and clipped_samples(samples) * 100 >= len(samples)):
        remove_sine(signal, rate, best)
        for harmonic in range(2, 10):
            frequency = best.frequency * harmonic
            if frequency >= rate / 2 - 5:
                break
            fit = fit_sine(signal, rate, frequency)
            remove_sine(signal, rate, fit)
            residual -= fit.power
    return Tone(best.frequency, level, sinad, level - power_db(residual))


def analyze_wav(path, args):
    with open(path, "rb") as wav:
        rate, channels, frames = read_pcm16_header(wav)
        if channels != 1 or not 4000 <= rate <= 192000:
            raise ValueError("expected mono 16-bit PCM at 4–192 kHz")
        if not frames:
            raise ValueError("recording contains no frames")
        duration = frames / rate
        print(f"sample_rate_hz={rate}\nframes={frames}\nduration_s={duration:.3f}")
        duration_ok = args.seconds is None or abs(duration - args.seconds) <= 0.25

        quiet_levels = []
        peak_dbfs = -200.0
        best_tone = previous_tone = candidate = None
        rail_samples = 0
        window = rate // 5
        for second, offset in enumerate(range(0, frames, rate)):
            count = min(rate, frames - offset)
            samples = read_samples(wav, count, channels)
            rms_dbfs = power_db(sum(value * value for value in samples) / (count * PCM_POWER_SCALE))
            peak = power_db((max(abs(value) for value in samples) / PCM_SCALE) ** 2)
            peak_dbfs = max(peak_dbfs, peak)
            rail_samples += clipped_samples(samples)
            print(f"second={second} rms_dbfs={rms_dbfs:.2f} peak_dbfs={peak:.2f}")

            if count == rate:
                quiet_levels.append(power_db(ac_power(samples)[1]))
            for start in range(0, count - window + 1, window):
                segment = samples[start:start + window]
                mean, ac = ac_power(segment)
                tone = tone_in_window(segment, rate, mean, ac, allow_clipping=True)
                if candidate is None or tone.level > candidate.level:
                    candidate = tone
                if not tone_in_band(tone.frequency) or tone.level < TONE_MIN_DBFS or tone.snr < TONE_MIN_SNR_DB:
                    previous_tone = None
                    continue
                if previous_tone is not None:
                    stronger = max(previous_tone, tone, key=lambda match: match.snr)
                    if best_tone is None or stronger.snr > best_tone.snr:
                        best_tone = stronger
                previous_tone = tone

    print(f"clipping_detected={'yes' if rail_samples else 'no'}\nclipped_samples={rail_samples}")
    print(f"tone_detected={'yes' if best_tone is not None else 'no'}")
    reported = best_tone or (candidate if args.mode == "tone" else None)
    if reported is not None and reported.level > -200:
        field = "tone_frequency_hz" if best_tone is not None else "tone_candidate_hz"
        print(f"{field}={reported.frequency:.2f} tone_snr_db={reported.snr:.2f} "
              f"tone_dbfs={reported.level:.2f} tone_sinad_db={reported.sinad:.2f}")

    if args.mode == "quiet":
        if quiet_levels:
            print(f"noise_floor_dbfs={statistics.median(quiet_levels):.2f}")
        sane = bool(quiet_levels) and all(
            INPUT_MIN_DBFS <= level <= QUIET_MAX_DBFS for level in quiet_levels
        ) and peak_dbfs <= QUIET_PEAK_MAX_DBFS and best_tone is None
        print(f"noise_floor_sane={'yes' if sane else 'no'}")
        passed = duration_ok and sane
    else:
        passed = duration_ok and best_tone is not None
    if not duration_ok:
        print("recording duration differs from request by more than 0.25 s", file=sys.stderr)
    print(f"grade={'pass' if passed else 'fail'}")
    return 0 if passed else 1


def replay_pairs(path, seconds):
    """Measure adjacent 200 ms windows from a complete, native C920 WAV."""
    with open(path, "rb") as wav:
        rate, channels, frames = read_pcm16_header(wav)
        if (rate, channels) != (C920_RATE, 1):
            raise ValueError(f"{path}: expected 16 kHz mono 16-bit PCM")
        if abs(frames / rate - seconds) > 0.25:
            raise ValueError(f"{path}: recorded {frames / rate:.3f} s, expected {seconds} s")
        samples = read_samples(wav, frames, channels)
    if min(samples) == max(samples):
        raise ValueError(f"{path}: constant PCM")

    window = rate // 5
    previous = None
    for start in range(0, frames - window + 1, window):
        segment = samples[start:start + window]
        mean, ac = ac_power(segment)
        tone = tone_in_window(segment, rate, mean, ac)
        level, snr = tone.level, tone.snr if tone_in_band(tone.frequency) else -200.0
        if previous is not None:
            yield min(previous[0], level), min(previous[1], snr), (start - window) / rate
        previous = level, snr


def analyze_replay(replay, control, seconds):
    quiet = list(replay_pairs(control, seconds))
    quiet_ceiling = max(level for level, _, _ in quiet)
    quiet_clean = not any(level >= REPLAY_MIN_DBFS and snr >= TONE_MIN_SNR_DB for level, snr, _ in quiet)
    pairs = list(replay_pairs(replay, seconds))
    above_snr = [pair for pair in pairs if pair[1] >= TONE_MIN_SNR_DB]
    level, snr, at = max(above_snr or pairs)
    margin = level - quiet_ceiling
    detected = level >= REPLAY_MIN_DBFS and snr >= TONE_MIN_SNR_DB
    passed = quiet_clean and detected and margin >= REPLAY_MIN_MARGIN_DB
    print(f"quiet_1khz_dbfs={quiet_ceiling:.2f} quiet_control_clean={'yes' if quiet_clean else 'no'}")
    print(f"replay_start_s={at:.2f} replay_1khz_dbfs={level:.2f} replay_snr_db={snr:.2f} "
          f"replay_above_quiet_db={margin:.2f}")
    print(f"replay_tone_detected={'yes' if detected else 'no'}")
    print(f"grade={'pass' if passed else 'fail'}")
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--mode", choices=("tone", "quiet", "replay"), required=True)
    parser.add_argument("--control", type=Path, help="C920 quiet-replay WAV for replay mode")
    parser.add_argument("--seconds", type=int, help="expected capture duration")
    args = parser.parse_args()
    if args.seconds is not None and args.seconds < 1:
        parser.error("--seconds must be positive")
    if args.mode == "replay" and (args.control is None or args.seconds is None):
        parser.error("replay mode requires --control and --seconds")
    if args.mode != "replay" and args.control is not None:
        parser.error("--control is for replay mode")
    if args.recording.suffix.lower() not in (".m4a", ".wav"):
        parser.error("expected an .m4a or .wav")
    try:
        if args.mode == "replay":
            return analyze_replay(args.recording, args.control, args.seconds)
        if not args.recording.is_file() or args.recording.stat().st_size == 0:
            raise ValueError("missing or empty recording")
        if args.recording.suffix.lower() == ".wav":
            with args.recording.open("rb") as wav:
                validate_wav(wav)
        with tempfile.TemporaryDirectory(prefix="piano-mic-check.") as folder:
            pcm = Path(folder) / "normalized.wav"
            subprocess.run(
                ["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16", "-c", "1",
                 str(args.recording.resolve()), str(pcm)],
                check=True,
            )
            return analyze_wav(pcm, args)
    except (OSError, ValueError, OverflowError, subprocess.CalledProcessError) as error:
        print(f"analyze: {error}", file=sys.stderr)
        print("grade=error")
        return 2


if __name__ == "__main__":
    sys.exit(main())
