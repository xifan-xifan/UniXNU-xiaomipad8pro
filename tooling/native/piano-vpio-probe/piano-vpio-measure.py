#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""piano-vpio-measure: stimuli and offline numbers for the VPIO proof (piano-vpio-probe). Pure Python, no numpy.

  stimuli DIR [--rate 24000] [--voice Samantha]
      far.wav       the tablet's far end at RATE (64 s): 8 s silence, music A (16 s), 24 s silence, music B (14 s), 2 s silence
      music30.wav   the 30 s music-like reference itself (synthetic and seeded: pad chords, bass, melody, drums, a pink bed)
      near.wav      the Mac speaker's near end at 48 kHz (62 s): a 0.5 s sync chirp (500-3000 Hz) at 0.4 s, speech 1 at 24.5-34.5 s,
                    pink noise at 35.5-43.5 s, speech 2 at 47-61 s (against music B: double talk); speech from `say` reading
                    Aesop's "The North Wind and the Sun" (public domain)
      far8k.wav, near8k.wav (the analysis rate), schedule.json (every time above)
  block ASSETS RUN TAG [--other]
      one block: RUN/TAG.wav (probe), RUN/c920-TAG.wav (C920), RUN/TAG-probe.txt (probe log) -> RUN/TAG-measure.json + one INFO line.
      --other: the probe played silence while afplay played far.wav (other audio): music windows come from the C920's onset.
  verdict RUN [TAGS]        PASS / FAIL / VOID lines from the block files (default tags: vpio bypass raw other vpio2)
  selftest DIR              synthetic recordings with known changes through block + verdict (host only, no audio device)

Timeline. Probe time p = seconds into the probe's recording. The far end starts at p = F + 8 s, where F = the probe's "timing
far-start-in-rec-s" plus the echo delay found by GCC-PHAT (an echo VPIO removed falls back to 0.15 s and says so). The near end's
origin D comes from the chirp in the recording (matched filter): near time h is at p = h + D. The C920 maps by its own chirp: c = p + K.
Numbers: 200-3500 Hz band at the 8 kHz analysis rate, dBFS (a full-scale sine is -3 dB).
  floor   between the chirp and music A (no far, no near)
  echo    music A only, from 3 s after its start (convergence) to its end: band level, plus its music-coherent part (level x
          sum|Sxy|^2/Sxx / sum Syy, Welch with 64 ms Hann frames, the far end aligned by the echo delay): "music correlation"
  speech  speech 1 only, over the 20 ms frames where the source is within 30 dB of its peak; and the envelope correlation r with it
  noise   pink noise only
  DT      music B + speech 2: speech level, envelope r, and the music-coherent residual
  C920    the tablet's music A and the Mac's speech 1 at the webcam: a VPIO echo result whose C920 music level differs by more than
          1 dB from bypass or raw is ducking, not cancellation (VOID)
Pass (MIC-DSP.md section 3): echo >= 15 dB under bypass (band level; when bypass's echo stands less than 18 dB over the floor, the
coherent part decides and the band level must sit at the floor), noise >= 6 dB under bypass, speech within 3 dB of bypass with r >= 0.7,
the C920 music within 1 dB.
"""
import argparse, array, cmath, concurrent.futures, json, math, operator, os, random, re, subprocess, sys, tempfile, wave

FA = 8000
BAND = (200.0, 3500.0)
FAR = {'musicA': (8.0, 24.0), 'musicB': (48.0, 62.0), 'length': 64.0}
NEAR = {'chirp': (0.4, 0.9), 'chirp_f': (500.0, 3000.0), 'speech1': (24.5, 34.5), 'noise': (35.5, 43.5), 'speech2': (47.0, 61.0),
        'length': 62.0}
PROBE_SECONDS = 66
CONVERGE = 3.0
DEFAULT_DELAY = 0.15
FABLE = ("The North Wind and the Sun were disputing which was the stronger, when a traveler came along wrapped in a warm cloak. "
         "They agreed that the one who first succeeded in making the traveler take his cloak off should be considered stronger than "
         "the other.",
         "Then the North Wind blew as hard as he could, but the more he blew the more closely did the traveler fold his cloak around "
         "him; and at last the North Wind gave up the attempt. Then the Sun shined out warmly, and immediately the traveler took off "
         "his cloak. And so the North Wind was obliged to confess that the Sun was the stronger of the two.")
mul = operator.mul


# ---------------------------------------------------------------- basic signal tools
def db(p):
    return 10.0 * math.log10(p) if p > 1e-20 else -200.0


def read_wav(path):
    w = wave.open(path, 'rb')
    if w.getsampwidth() != 2:
        raise SystemExit('%s: 16-bit PCM only' % path)
    rate, ch = w.getframerate(), w.getnchannels()
    a = array.array('h', w.readframes(w.getnframes()))
    if sys.byteorder == 'big':
        a.byteswap()
    return rate, [v / 32768.0 for v in a[0::ch]]


def write_wav(path, rate, x):
    a = array.array('h', (32767 if v >= 1 else -32768 if v <= -1 else int(round(v * 32767)) for v in x))
    if sys.byteorder == 'big':
        a.byteswap()
    w = wave.open(path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(a.tobytes())
    w.close()


def lowpass_taps(rate_in, f_pass=3400.0, f_stop=4400.0):
    n = int(3.3 * rate_in / (f_stop - f_pass)) | 1   # Hamming
    fc = (f_pass + f_stop) / 2 / rate_in
    c = (n - 1) / 2
    h = []
    for k in range(n):
        x = k - c
        s = 2 * fc if x == 0 else math.sin(2 * math.pi * fc * x) / (math.pi * x)
        h.append(s * (0.54 - 0.46 * math.cos(2 * math.pi * k / (n - 1))))
    g = sum(h)
    return [v / g for v in h]


def to_fa(rate, x):
    """Decimate to the analysis rate (integer ratios only)."""
    if rate == FA:
        return list(x)
    m = rate // FA
    if m * FA != rate:
        raise SystemExit('rate %d is not a multiple of %d' % (rate, FA))
    h = lowpass_taps(rate)
    n = len(h); half = n // 2
    xp = [0.0] * half + list(x) + [0.0] * (half + 1)
    return [sum(map(mul, h, xp[i:i + n])) for i in range(0, len(x), m)]


def highpass(x, f0, fs=FA, q=0.7071067811865476):
    w0 = 2 * math.pi * f0 / fs; al = math.sin(w0) / (2 * q); cw = math.cos(w0); a0 = 1 + al
    b0 = (1 + cw) / 2 / a0; b1 = -(1 + cw) / a0; b2 = b0; a1 = -2 * cw / a0; a2 = (1 - al) / a0
    y = [0.0] * len(x); x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def band(x):
    return highpass(highpass(x, BAND[0]), BAND[0])


def seg(y, t0, t1):
    i0, i1 = max(0, int(round(t0 * FA))), min(len(y), int(round(t1 * FA)))
    return y[i0:i1] if i1 > i0 else []


def power(s):
    return sum(map(mul, s, s)) / len(s) if s else 0.0


def level(y, t0, t1):
    return db(power(seg(y, t0, t1)))


def env(s, frame):
    return [sum(map(mul, s[i:i + frame], s[i:i + frame])) / frame for i in range(0, len(s) - frame + 1, frame)]


def hann(n):
    return [0.5 - 0.5 * math.cos(2 * math.pi * k / n) for k in range(n)]


_tw = {}


def fft(a, inverse=False):
    n = len(a)
    a = list(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        half = size >> 1
        tw = _tw.get((size, inverse))
        if tw is None:
            s = (2j if inverse else -2j) * math.pi / size
            tw = _tw[(size, inverse)] = [cmath.exp(s * k) for k in range(half)]
        for start in range(0, n, size):
            for k in range(half):
                u = a[start + k]
                v = a[start + k + half] * tw[k]
                a[start + k] = u + v
                a[start + k + half] = u - v
        size <<= 1
    if inverse:
        a = [v / n for v in a]
    return a


def pearson(a, b):
    n = len(a)
    if n < 3:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    sab = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    saa = sum((x - ma) ** 2 for x in a); sbb = sum((y - mb) ** 2 for y in b)
    return sab / math.sqrt(saa * sbb) if saa > 0 and sbb > 0 else 0.0


def shifted(x, off):
    """x delayed by off samples (positive: later), zero-filled, same length."""
    n = len(x)
    if off >= 0:
        return ([0.0] * off + x[:n - off]) if off < n else [0.0] * n
    off = -off
    return x[off:] + [0.0] * off if off < n else [0.0] * n


# ---------------------------------------------------------------- stimuli
def chirp(rate, dur=None, f0=None, f1=None):
    t0, t1 = NEAR['chirp']; dur = dur or (t1 - t0)
    f0 = f0 or NEAR['chirp_f'][0]; f1 = f1 or NEAR['chirp_f'][1]
    n = int(round(dur * rate)); fade = int(0.01 * rate)
    out = []
    for i in range(n):
        t = i / rate
        g = 1.0
        if i < fade:
            g = 0.5 - 0.5 * math.cos(math.pi * i / fade)
        elif i >= n - fade:
            g = 0.5 - 0.5 * math.cos(math.pi * (n - 1 - i) / fade)
        out.append(g * math.sin(2 * math.pi * (f0 * t + (f1 - f0) * t * t / (2 * dur))))
    return out


def pink(n, rnd):
    b = [0.0] * 7; out = []
    for _ in range(n):
        w = rnd.uniform(-1, 1)
        b[0] = 0.99886 * b[0] + w * 0.0555179; b[1] = 0.99332 * b[1] + w * 0.0750759; b[2] = 0.96900 * b[2] + w * 0.1538520
        b[3] = 0.86650 * b[3] + w * 0.3104856; b[4] = 0.55000 * b[4] + w * 0.5329522; b[5] = -0.7616 * b[5] - w * 0.0168980
        out.append(b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362)
        b[6] = w * 0.115926
    return out


def scale_rms(x, target_db):
    p = power(x)
    g = 10 ** (target_db / 20) / math.sqrt(p) if p > 0 else 0
    return [v * g for v in x]


def music30(rate, seconds=30.0, seed=20261121):
    """Synthetic, music-like and broadband: a continuous pad (its envelope never drops), bass, melody, kick/snare/hats, pink bed."""
    rnd = random.Random(seed)
    n = int(rate * seconds)
    out = [0.0] * n
    beat = 60.0 / 108.0
    bar = 4 * beat

    def hz(m):
        return 440.0 * 2 ** ((m - 69) / 12.0)

    def tone(t0, dur, f, amp, harm, attack, release, decay=None):
        i0 = max(0, int(t0 * rate)); i1 = min(n, int((t0 + dur + release) * rate))
        ws = [2 * math.pi * f * k / rate for k in range(1, harm + 1) if f * k < rate * 0.45]
        amps = [amp / (k + 1) ** 1.2 for k in range(len(ws))]
        for i in range(i0, i1):
            t = i / rate - t0
            g = min(1.0, t / attack) if attack > 0 else 1.0
            if t > dur:
                g *= max(0.0, 1 - (t - dur) / release)
            if decay:
                g *= 0.35 + 0.65 * math.exp(-t / decay)
            s = 0.0
            for w, a in zip(ws, amps):
                s += a * math.sin(w * (i - i0))
            out[i] += g * s

    chords = [(48, (0, 4, 7)), (43, (0, 4, 7)), (45, (0, 3, 7)), (41, (0, 4, 7)),
              (50, (0, 3, 7)), (43, (0, 4, 7)), (48, (0, 4, 7)), (41, (0, 4, 7))]
    scale = [60, 62, 64, 65, 67, 69, 71, 72, 74, 76]
    t = 0.0; c = 0
    while t < seconds:
        root, iv = chords[c % len(chords)]
        for k in iv:
            tone(t, bar, hz(root + 12 + k), 0.10, 6, 0.12, 0.35)
        for b in range(4):
            tone(t + b * beat, beat * 0.9, hz(root - 12), 0.16, 4, 0.005, 0.08, decay=0.18)
        for e in range(8):
            if rnd.random() < 0.8:
                tone(t + e * beat / 2, beat / 2 * 0.85, hz(rnd.choice(scale)), 0.07, 5, 0.01, 0.05, decay=0.12)
        t += bar; c += 1
    for b in range(int(seconds / beat)):          # drums
        tb = b * beat
        i0 = int(tb * rate)
        if b % 2 == 0:                            # kick: a falling sine
            for i in range(min(int(0.15 * rate), n - i0)):
                tt = i / rate
                out[i0 + i] += 0.35 * math.exp(-tt / 0.05) * math.sin(2 * math.pi * (50 * tt + 70 * 0.04 * (1 - math.exp(-tt / 0.04))))
        else:                                     # snare: a noise burst
            for i in range(min(int(0.12 * rate), n - i0)):
                out[i0 + i] += 0.12 * math.exp(-(i / rate) / 0.04) * rnd.uniform(-1, 1)
        for h in (0, 0.5):                        # hats: differentiated noise
            j0 = int((tb + h * beat) * rate); last = 0.0
            for i in range(min(int(0.04 * rate), n - j0)):
                w = rnd.uniform(-1, 1)
                out[j0 + i] += 0.05 * math.exp(-(i / rate) / 0.012) * (w - last)
                last = w
    bed = pink(n, rnd)
    pb = math.sqrt(power(bed))
    out = [v + 0.02 * b / pb for v, b in zip(out, bed)]
    out = scale_rms(out, -18.0)
    peak = max(abs(v) for v in out)
    if peak > 0.89:
        out = [0.89 * math.tanh(v / 0.89) for v in out]
    return out


def say_speech(text, voice, rate=48000):
    with tempfile.TemporaryDirectory() as td:
        aiff, wav = os.path.join(td, 's.aiff'), os.path.join(td, 's.wav')
        cmd = ['say', '-o', aiff, text] if not voice else ['say', '-v', voice, '-o', aiff, text]
        subprocess.run(cmd, check=True)
        subprocess.run(['afconvert', '-f', 'WAVE', '-d', 'LEI16@%d' % rate, '-c', '1', aiff, wav], check=True)
        r, x = read_wav(wav)
    if r != rate:
        raise SystemExit('say/afconvert gave %d Hz' % r)
    return x


def fit(x, n, rate):
    """Trim (with a 30 ms fade) or pad x to n samples."""
    if len(x) >= n:
        x = x[:n]
        f = int(0.03 * rate)
        for i in range(f):
            x[n - 1 - i] *= i / f
        return x
    return x + [0.0] * (n - len(x))


def active_rms_scale(x, target_db, rate):
    fr = int(0.02 * rate)
    e = env(x, fr)
    pk = max(e) if e else 0
    act = [v for v in e if v > pk * 1e-3]
    p = sum(act) / len(act) if act else 0
    g = 10 ** (target_db / 20) / math.sqrt(p) if p > 0 else 0
    return [v * g for v in x]


def stimuli(a):
    os.makedirs(a.dir, exist_ok=True)
    rate = a.rate
    rnd = random.Random(1121)
    m = music30(rate)
    write_wav(os.path.join(a.dir, 'music30.wav'), rate, m)
    la = FAR['musicA'][1] - FAR['musicA'][0]; lb = FAR['musicB'][1] - FAR['musicB'][0]
    far = [0.0] * int(FAR['length'] * rate)
    ia, ib = int(FAR['musicA'][0] * rate), int(FAR['musicB'][0] * rate)
    far[ia:ia + int(la * rate)] = m[:int(la * rate)]
    far[ib:ib + int(lb * rate)] = m[int(la * rate):int(la * rate) + int(lb * rate)]
    far = far[:int(FAR['length'] * rate)]
    write_wav(os.path.join(a.dir, 'far.wav'), rate, far)
    nr = 48000
    near = [0.0] * int(NEAR['length'] * nr)

    def put(x, t0):
        i0 = int(round(t0 * nr))
        for i, v in enumerate(x):
            if i0 + i < len(near):
                near[i0 + i] += v

    put([0.5 * v for v in chirp(nr)], NEAR['chirp'][0])
    s1 = active_rms_scale(fit(say_speech(FABLE[0], a.voice), int((NEAR['speech1'][1] - NEAR['speech1'][0]) * nr), nr), -20.0, nr)
    s2 = active_rms_scale(fit(say_speech(FABLE[1], a.voice), int((NEAR['speech2'][1] - NEAR['speech2'][0]) * nr), nr), -20.0, nr)
    nz = scale_rms(pink(int((NEAR['noise'][1] - NEAR['noise'][0]) * nr), rnd), -22.0)
    f = int(0.05 * nr)
    for i in range(f):
        nz[i] *= i / f; nz[-1 - i] *= i / f
    put(s1, NEAR['speech1'][0]); put(nz, NEAR['noise'][0]); put(s2, NEAR['speech2'][0])
    write_wav(os.path.join(a.dir, 'near.wav'), nr, near)
    write_wav(os.path.join(a.dir, 'far8k.wav'), FA, to_fa(rate, far))
    write_wav(os.path.join(a.dir, 'near8k.wav'), FA, to_fa(nr, near))
    sched = {'fa': FA, 'band_hz': BAND, 'rate': rate, 'far': FAR, 'near': NEAR, 'probe_seconds': PROBE_SECONDS,
             'convergence_s': CONVERGE, 'voice': a.voice, 'text': 'Aesop, The North Wind and the Sun (public domain)'}
    json.dump(sched, open(os.path.join(a.dir, 'schedule.json'), 'w'), indent=1)
    print('stimuli: far.wav %.0f s at %d Hz (music RMS %.1f dBFS), near.wav %.0f s at 48 kHz, speech %s' % (
        len(far) / rate, rate, db(power(m)), len(near) / nr, a.voice or 'default voice'))


# ---------------------------------------------------------------- measurements
def find_chirp(y, lo_s, hi_s):
    """Matched filter: the chirp's start (s) in [lo_s, hi_s) and the normalised correlation there."""
    t = band(chirp(FA))
    lo, hi = max(0, int(lo_s * FA)), min(len(y), int(hi_s * FA))
    s = y[lo:hi + len(t)]
    if len(s) < len(t) + 2:
        return None, 0.0
    n = 1
    while n < len(s) + len(t):
        n <<= 1
    Y = fft(s + [0.0] * (n - len(s)))
    T = fft(t + [0.0] * (n - len(t)))
    r = fft([p * q.conjugate() for p, q in zip(Y, T)], inverse=True)
    cs = [0.0]; acc = 0.0
    for v in s:
        acc += v * v; cs.append(acc)
    et = sum(v * v for v in t)
    best, at = 0.0, 0
    for lag in range(0, min(hi - lo, len(s) - len(t) + 1)):
        e = cs[lag + len(t)] - cs[lag]
        if e > 1e-18:
            rho = abs(r[lag].real) / math.sqrt(e * et)
            if rho > best:
                best, at = rho, lag
    return (lo + at) / FA, best


def gcc_delay(y, x, t0, t1, lo=-0.05, hi=0.5, n=8192):
    """Echo delay (s) of y behind x over [t0, t1] by GCC-PHAT; confidence = peak / median |r| over the lag range."""
    i0, i1 = int(t0 * FA), int(t1 * FA)
    w = hann(n)
    G = [0j] * n; frames = 0
    for s in range(i0, i1 - n + 1, n // 2):
        Y = fft([p * q for p, q in zip(y[s:s + n], w)])
        X = fft([p * q for p, q in zip(x[s:s + n], w)])
        for k in range(n):
            G[k] += Y[k] * X[k].conjugate()
        frames += 1
    if not frames:
        return DEFAULT_DELAY, 0.0
    kl, kh = int(BAND[0] * n / FA), int(BAND[1] * n / FA)
    P = []
    for k, g in enumerate(G):
        kk = k if k <= n // 2 else n - k
        P.append(g / abs(g) if kl <= kk <= kh and abs(g) > 1e-30 else 0j)
    r = fft(P, inverse=True)
    vals = [(r[l % n].real, l) for l in range(int(lo * FA), int(hi * FA) + 1)]
    pk, lag = max(vals)
    med = sorted(abs(v) for v, _ in vals)[len(vals) // 2] or 1e-30
    return lag / FA, pk / med


def coherent(y, x, t0, t1, n=512):
    """Welch over [t0, t1]: the part of y's band power linearly explained by x (fraction), mean coherence, frames."""
    i0, i1 = int(t0 * FA), int(t1 * FA)
    w = hann(n)
    kl, kh = int(math.ceil(BAND[0] * n / FA)), int(BAND[1] * n / FA)
    m = kh - kl + 1
    sxx = [0.0] * m; syy = [0.0] * m; sxy = [0j] * m; frames = 0
    for s in range(i0, i1 - n + 1, n // 2):
        Y = fft([p * q for p, q in zip(y[s:s + n], w)])
        X = fft([p * q for p, q in zip(x[s:s + n], w)])
        for j in range(m):
            a, b = Y[kl + j], X[kl + j]
            sxx[j] += (b * b.conjugate()).real; syy[j] += (a * a.conjugate()).real; sxy[j] += a * b.conjugate()
        frames += 1
    if not frames:
        return 0.0, 0.0, 0
    pc = sum(abs(c) ** 2 / p for c, p in zip(sxy, sxx) if p > 0)
    py = sum(syy)
    coh = sum(abs(c) ** 2 / (p * q) for c, p, q in zip(sxy, sxx, syy) if p > 0 and q > 0) / m
    return (pc / py if py > 0 else 0.0), coh, frames


def speech(y, src, D, h0, h1, frame=160):
    """Level over the source's active 20 ms frames and the envelope correlation, for near-end time [h0, h1] at p = h + D."""
    hs, he = h0 + 0.3, h1 - 0.3
    se = env(seg(src, hs, he), frame)
    ye = env(seg(y, hs + D, he + D), frame)
    k = min(len(se), len(ye))
    if k < 10:
        return -200.0, 0.0, 0
    se, ye = se[:k], ye[:k]
    pk = max(se)
    act = [i for i in range(k) if se[i] > pk * 1e-3]
    if not act:
        return -200.0, 0.0, 0
    lvl = db(sum(ye[i] for i in act) / len(act))
    return lvl, pearson([db(se[i]) for i in act], [db(ye[i]) for i in act]), len(act)


def dropouts(y, t0, t1, frame=160):
    e = [db(v) for v in env(seg(y, t0, t1), frame)]
    if not e:
        return 0
    med = sorted(e)[len(e) // 2]
    return sum(1 for v in e if v < med - 20)


def parse_probe(path):
    d = {}
    try:
        t = open(path, errors='replace').read()
    except OSError:
        return {'missing': True}
    m = re.search(r'callbacks (\d+) frames (\d+) expected (\d+) gaps (\d+) \(largest (\d+) frames at [\d.]+ s\) peak (-?[\d.]+) dBFS '
                  r'rms (-?[\d.]+) dBFS zeros ([\d.]+)% clipped (\d+) render-errors (\d+)', t)
    if m:
        d.update(callbacks=int(m[1]), frames=int(m[2]), expected=int(m[3]), gaps=int(m[4]), largest_gap=int(m[5]), peak_db=float(m[6]),
                 rms_db=float(m[7]), zeros_pct=float(m[8]), clipped=int(m[9]), render_errors=int(m[10]))
    m = re.search(r'timing far-start-in-rec-s (-?[\d.]+)', t)
    d['far_start'] = float(m[1]) if m else None
    m = re.search(r'overloads input (\d+) output (\d+) unit (\d+)', t)
    if m:
        d['overloads'] = [int(m[1]), int(m[2]), int(m[3])]
    rb = re.findall(r'readback bypass (-?\d+) agc (-?\d+) mute-output (-?\d+) ducking advanced (-?\d+) level (\d+) \((\w+|\?)\)', t)
    if rb:
        d['readback'] = dict(zip(('bypass', 'agc', 'mute', 'advanced', 'level', 'duck'), rb[-1]))
    m = re.search(r'^vpio-probe: init (-?\d+)', t, re.M)
    d['init'] = int(m[1]) if m else None
    m = re.search(r'vpio-probe: mode (\w+)', t)
    d['mode'] = m[1] if m else None
    m = re.search(r'tcc mic_status=(\d+)', t)
    d['mic_status'] = int(m[1]) if m else None
    m = re.search(r'mic_mode active=(\d+)', t)
    d['mic_mode'] = int(m[1]) if m else None
    m = re.search(r'unit latency ([\d.]+) s', t)
    d['unit_latency'] = float(m[1]) if m else None
    return d


def block(a):
    run, tag = a.run, a.tag
    out = {'tag': tag, 'other': bool(a.other), 'void': []}
    out['probe'] = pr = parse_probe(os.path.join(run, tag + '-probe.txt'))
    wav = os.path.join(run, tag + '.wav')
    if not os.path.isfile(wav):
        out['void'].append('no recording')
        return finish(run, tag, out)
    rate, rec = read_wav(wav)
    y = band(to_fa(rate, rec))
    out['seconds'] = len(y) / FA
    _, far8 = read_wav(os.path.join(a.assets, 'far8k.wav'))
    _, near8 = read_wav(os.path.join(a.assets, 'near8k.wav'))
    pad = [0.0] * (len(y) + 10 * FA)
    xf = (band(far8) + pad)[:len(pad)]; xn = (band(near8) + pad)[:len(pad)]
    if pr.get('zeros_pct', 0) >= 90 or power(y) < 1e-14:
        out['void'].append('all-zero or silent input (zeros %.1f%%): TCC or a muted path' % pr.get('zeros_pct', 100.0))
        return finish(run, tag, out)
    F0 = pr.get('far_start') if not a.other else None
    if F0 is None and not a.other:
        F0 = 0.0
        out['void'].append('no far-start time stamp (assumed 0)')
    hi = (F0 if F0 is not None else 0) + FAR['musicA'][0] - 0.1
    pc, rho = find_chirp(y, 0.0, min(hi, 7.8))
    out['chirp'] = {'p': pc, 'rho': rho}
    D = pc - NEAR['chirp'][0] if pc is not None and rho >= 0.2 else None
    if D is None:
        out['void'].append('chirp not found (rho %.2f): near-end windows void' % rho)
    out['D'] = D
    # C920
    cw = os.path.join(run, 'c920-%s.wav' % tag)
    c = None
    if os.path.isfile(cw):
        cr, cx = read_wav(cw)
        c = band(to_fa(cr, cx))
        cc, crho = find_chirp(c, 0.0, 20.0)
        out['c920'] = {'chirp': cc, 'rho': crho}
        K = cc - pc if cc is not None and pc is not None and crho >= 0.2 and D is not None else None
        out['c920']['K'] = K
    else:
        K = None
        out['c920'] = {'missing': True}
    def put_floor(f1):
        f0 = pc + NEAR['chirp'][1] - NEAR['chirp'][0] + 0.6
        out['floor_window'] = [f0, f1]
        out['floor'] = level(y, f0, f1) if f1 - f0 >= 0.8 else None
        if c is not None and K is not None and f1 - f0 >= 0.8:
            out['c920']['floor'] = level(c, f0 + K, f1 + K)
    if D is not None and not a.other:
        put_floor(F0 + FAR['musicA'][0] - 0.2)
    if not a.other:
        # echo delay: GCC-PHAT on music A against the far end as the probe stamped it
        xr = shifted(xf, int(round(F0 * FA)))
        ma0, ma1 = FAR['musicA']
        dl, conf = gcc_delay(y, xr, F0 + ma0 + 0.5, F0 + ma1 - 0.5)
        assumed = conf < 8.0
        out['delay'] = {'s': dl if not assumed else DEFAULT_DELAY, 'gcc': dl, 'conf': conf, 'assumed': assumed}
        F = F0 + out['delay']['s']
        out['F'] = F
        xa = shifted(xf, int(round(F * FA)))
        e0, e1 = F + ma0 + CONVERGE, F + ma1 - 0.3
        out['echo_window'] = [e0, e1]
        out['echo'] = level(y, e0, e1)
        frac, coh, frames = coherent(y, xa, e0, e1)
        out['echo_coh'] = out['echo'] + db(frac) if frac > 0 else -200.0
        out['echo_coh_floor'] = out['echo'] - db(frames) if frames else None   # the estimator's bias floor (1/frames)
        out['coh_mean'] = coh
        out['musicA_bins'] = [level(y, F + ma0 + s, F + ma0 + s + 1) for s in range(int(ma1 - ma0))]
        out['dropouts'] = dropouts(y, e0, e1)
        if c is not None and K is not None:
            out['c920']['music'] = level(c, e0 + K, e1 + K)
        if D is not None:
            mb0, mb1 = FAR['musicB']
            s20, s21 = NEAR['speech2']
            d0, d1 = max(F + mb0 + 0.3, s20 + D + 0.3), min(F + mb1 - 0.3, s21 + D - 0.3)
            if d1 - d0 >= 4:
                out['dt_window'] = [d0, d1]
                lv, r, nact = speech(y, xn, D, d0 - D, d1 - D)
                out['dt_speech'], out['dt_r'] = lv, r
                fr, _, _ = coherent(y, xa, d0, d1)
                out['dt_echo_coh'] = level(y, d0, d1) + db(fr) if fr > 0 else -200.0
            else:
                out['void'].append('double-talk window too short (D %.2f s)' % D)
    else:
        # other audio: music A's onset at the C920 (the probe itself played silence)
        out['delay'] = None
        if c is not None and out['c920'].get('chirp') is not None:
            cc = out['c920']['chirp']
            fl = sorted(env(seg(c, cc + 0.6, cc + 5.0), 80))
            floor = fl[len(fl) // 2] if fl else 0
            e = env(seg(c, cc + 3.0, cc + 16.0), 80)
            onset = None
            for i in range(len(e) - 10):
                if sum(e[i:i + 10]) / 10 > floor * 10 ** 1.5:
                    onset = cc + 3.0 + i * 0.01
                    break
            out['c920']['onset'] = onset
            if onset is not None and K is not None:
                po = onset - K
                put_floor(po - 0.3)
                o0, o1 = po + CONVERGE, po + FAR['musicA'][1] - FAR['musicA'][0] - 0.5
                out['echo_window'] = [o0, o1]
                out['echo'] = level(y, o0, o1)
                out['c920']['music'] = level(c, onset + CONVERGE, onset + FAR['musicA'][1] - FAR['musicA'][0] - 0.5)
            else:
                out['void'].append('no music onset at the C920')
    if D is not None:
        s10, s11 = NEAR['speech1']
        lv, r, nact = speech(y, xn, D, s10, s11)
        out['speech'], out['speech_r'], out['speech_frames'] = lv, r, nact
        n0, n1 = NEAR['noise']
        out['noise'] = level(y, n0 + D + 0.3, n1 + D - 0.3)
        lim = (F + FAR['musicB'][0]) if not a.other else None
        if lim is not None and n1 + D - 0.3 > lim:
            out['void'].append('noise window runs into music B (D %.2f s)' % D)
        if not a.other and s10 + D + 0.3 < F + FAR['musicA'][1] + 0.3:
            out['void'].append('speech 1 overlaps music A (D %.2f s)' % D)
        if c is not None and K is not None:
            out['c920']['speech'] = level(c, s10 + D + 0.3 + K, s11 + D - 0.3 + K)
            out['c920']['noise'] = level(c, n0 + D + 0.3 + K, n1 + D - 0.3 + K)
    return finish(run, tag, out)


def finish(run, tag, out):
    json.dump(out, open(os.path.join(run, tag + '-measure.json'), 'w'), indent=1)

    def f(k, d=None):
        v = (d if d is not None else out).get(k)
        return '%.1f' % v if isinstance(v, (int, float)) else '-'
    c = out.get('c920', {}); pr = out.get('probe', {}); dl = out.get('delay') or {}
    print('INFO vpio1 %s: chirp %s s (rho %s) D %s; echo delay %s s (gcc conf %s%s); floor %s, echo %s (music-coherent %s), speech %s '
          '(r %s), noise %s, DT speech %s / coherent echo %s dBFS; C920 music %s speech %s; probe zeros %s%% gaps %s clipped %s '
          'overloads %s%s' % (
              tag, f('p', out.get('chirp', {})), '%.2f' % out.get('chirp', {}).get('rho', 0), f('D'), '%.3f' % dl['s'] if dl else '-',
              f('conf', dl), ', assumed' if dl.get('assumed') else '', f('floor'), f('echo'), f('echo_coh'), f('speech'),
              '%.2f' % out['speech_r'] if 'speech_r' in out else '-', f('noise'), f('dt_speech'), f('dt_echo_coh'), f('music', c),
              f('speech', c), f('zeros_pct', pr), pr.get('gaps', '-'), pr.get('clipped', '-'), pr.get('overloads', '-'),
              ('; VOID: ' + '; '.join(out['void'])) if out['void'] else ''))
    return 0


def verdict(a):
    tags = a.tags or ['vpio', 'bypass', 'raw', 'other', 'vpio2']
    B = {}
    for t in tags:
        p = os.path.join(a.run, t + '-measure.json')
        if os.path.isfile(p):
            B[t] = json.load(open(p))
    lines = []
    out = lines.append

    def g(t, k, sub=None):
        d = B.get(t, {})
        if sub:
            d = d.get(sub) or {}
        v = d.get(k)
        return v if isinstance(v, (int, float)) and v > -199 else None
    V, Y, W = 'vpio', 'bypass', 'raw'
    missing = [t for t in (V, Y, W) if t not in B]
    hard = []
    for t in (V, Y, W):
        if t in B:
            pr = B[t].get('probe', {})
            if pr.get('zeros_pct', 0) >= 90 or any('all-zero' in v or v == 'no recording' for v in B[t]['void']):
                hard.append('%s: %s' % (t, '; '.join(B[t]['void']) or 'zeros %.0f%%' % pr.get('zeros_pct', 100)))
            elif pr.get('frames') and pr.get('expected') and pr['frames'] < 0.95 * pr['expected']:
                hard.append('%s: %d of %d frames' % (t, pr['frames'], pr['expected']))
    if missing or hard:
        out('VOID VPIO verdict: %s' % '; '.join(['missing block ' + t for t in missing] + hard))
        print('\n'.join(lines))
        return 0
    res = {}
    # echo
    ev, eb, er = g(V, 'echo'), g(Y, 'echo'), g(W, 'echo')
    fv, fb = g(V, 'floor'), g(Y, 'floor')
    cv, cb = g(V, 'echo_coh'), g(Y, 'echo_coh')
    if ev is None or eb is None:
        res['echo'] = 'VOID'; out('VOID vpio echo: no echo window (%s)' % '; '.join(B[V]['void'] + B[Y]['void']))
    elif fb is not None and eb - fb < 6:
        res['echo'] = 'VOID'; out('VOID vpio echo: bypass music echo only %.1f dB over its floor (the music does not reach the mic)' % (eb - fb))
    else:
        red = eb - ev
        redc = (cb - cv) if cv is not None and cb is not None else None
        lim = fb is not None and eb - fb < 18 and fv is not None and ev - fv < 3
        txt = ('%.1f dB under bypass (band 200-3500 Hz: %.1f -> %.1f dBFS, floor %s; music-coherent %s; vs raw %s)' % (
            red, eb, ev, '%.1f' % fv if fv is not None else '-', '%.1f dB' % redc if redc is not None else '-',
            '%.1f dB' % (er - ev) if er is not None else '-'))
        if red >= 15:
            res['echo'] = 'PASS'
        elif lim and redc is not None and redc >= 15:
            res['echo'] = 'PASS'; txt += ' (floor-limited: the coherent part decides)'
        elif lim:
            res['echo'] = 'VOID'; txt += ' (floor-limited and the coherent part short of 15 dB: inconclusive)'
        else:
            res['echo'] = 'FAIL'
        out('%s vpio echo: %s   [>= 15 dB]' % (res['echo'], txt))
    # noise
    nv, nb = g(V, 'noise'), g(Y, 'noise')
    if nv is None or nb is None:
        res['noise'] = 'VOID'; out('VOID vpio noise: no noise window')
    elif fb is not None and nb - fb < 6:
        res['noise'] = 'VOID'; out('VOID vpio noise: the host noise stands only %.1f dB over the floor at the tablet' % (nb - fb))
    else:
        res['noise'] = 'PASS' if nb - nv >= 6 else 'FAIL'
        out('%s vpio noise: %.1f dB under bypass (%.1f -> %.1f dBFS)   [>= 6 dB]' % (res['noise'], nb - nv, nb, nv))
    # speech
    sv, sb, rv = g(V, 'speech'), g(Y, 'speech'), g(V, 'speech_r')
    if sv is None or sb is None or rv is None:
        res['speech'] = 'VOID'; out('VOID vpio near-end speech: no speech window')
    elif fb is not None and sb - fb < 6:
        res['speech'] = 'VOID'; out('VOID vpio near-end speech: the host speech stands only %.1f dB over the floor at the tablet' % (sb - fb))
    else:
        rb = g(Y, 'speech_r') or 0
        shape = rv >= 0.7 or (rb < 0.7 and rv >= rb - 0.05)   # a near end already blurred at the tablet (bypass r < 0.7) is not VPIO's
        res['speech'] = 'PASS' if abs(sv - sb) <= 3 and shape else 'FAIL'
        out('%s vpio near-end speech: %+.1f dB vs bypass (%.1f -> %.1f dBFS), envelope r %.2f (bypass %.2f)   [within 3 dB, r >= 0.7%s]' % (
            res['speech'], sv - sb, sb, sv, rv, rb, '' if rb >= 0.7 else '; bypass itself under 0.7: r within 0.05 of it'))
    # C920: ducking of the probe's own music, and the near end's stability
    mv, mb, mr = g(V, 'music', 'c920'), g(Y, 'music', 'c920'), g(W, 'music', 'c920')
    cf = g(Y, 'floor', 'c920')
    if mv is None or mb is None or mr is None:
        res['duck'] = 'VOID'; out('VOID C920 ducking check: no C920 music window (chirp at the C920 not found?)')
    elif cf is not None and mb - cf < 10:
        res['duck'] = 'VOID'; out('VOID C920 ducking check: the tablet music stands only %.1f dB over the C920 floor' % (mb - cf))
    else:
        ok = abs(mv - mb) <= 1 and abs(mv - mr) <= 1
        res['duck'] = 'PASS' if ok else 'VOID'
        out('%s C920 not ducked: the tablet music at the C920 %+.1f dB vs bypass, %+.1f dB vs raw   [within 1 dB; else the echo '
            'result is ducking]' % (res['duck'], mv - mb, mv - mr))
        if not ok and res.get('echo') == 'PASS':
            res['echo'] = 'VOID'
    sp = [g(t, 'speech', 'c920') for t in (V, Y, W)]
    if None not in sp:
        out('INFO harness: the Mac speech at the C920 spread %.1f dB over vpio/bypass/raw (the near end was the same)' % (max(sp) - min(sp)))
    if eb is not None and er is not None:
        out('INFO harness: bypass vs raw music echo %+.1f dB (bypass unprocessed if near 0)' % (eb - er))
    for t in (V, Y, W):
        b = B[t]; pr = b.get('probe', {})
        out('INFO integrity %s: gaps %s, dropouts %s (20 ms windows > 20 dB under the median of music A), clipped %s, overloads %s, '
            'render errors %s, echo delay %s s (%s)' % (
                t, pr.get('gaps'), b.get('dropouts'), pr.get('clipped'), pr.get('overloads'), pr.get('render_errors'),
                '%.3f' % b['delay']['s'] if b.get('delay') else '-',
                ('assumed' if b['delay']['assumed'] else 'gcc %.0f' % b['delay']['conf']) if b.get('delay') else '-'))
    bv, bb = B[V].get('musicA_bins') or [], B[Y].get('musicA_bins') or []
    if bv and bb:
        out('INFO convergence: echo reduction per second of music A: ' + ' '.join('%.0f' % (q - p) for p, q in zip(bv, bb)))
    dv, db_ = g(V, 'dt_speech'), g(Y, 'dt_echo_coh')
    if dv is not None and sv is not None:
        out('INFO double talk: vpio speech %+.1f dB vs its speech-only level (r %.2f); music-coherent residual %s dB vs bypass' % (
            dv - sv, g(V, 'dt_r') or 0, '%.1f' % (g(V, 'dt_echo_coh') - db_) if g(V, 'dt_echo_coh') is not None and db_ is not None else '-'))
    if 'other' in B:
        O = B['other']
        om, oe = g('other', 'music', 'c920'), g('other', 'echo')
        out('INFO other audio (afplay music while VPIO runs with ducking %s): the C920 hears it %s dB vs bypass (ducking); the '
            'recording %s dB vs raw (cancelled if far below 0)%s' % (
                (O.get('probe', {}).get('readback') or {}).get('duck', '?'), '%+.1f' % (om - mb) if om is not None and mb is not None else '-',
                '%+.1f' % (oe - er) if oe is not None and er is not None else '-', ('; void: ' + '; '.join(O['void'])) if O['void'] else ''))
    if 'vpio2' in B:
        e2, n2, s2 = g('vpio2', 'echo'), g('vpio2', 'noise'), g('vpio2', 'speech')
        out('INFO vpio2 (AGC off): echo %s dB, noise %s dB, speech %s dB vs bypass' % (
            '%.1f' % (eb - e2) if e2 is not None and eb is not None else '-', '%.1f' % (nb - n2) if n2 is not None and nb is not None else '-',
            '%+.1f' % (s2 - sb) if s2 is not None and sb is not None else '-'))
    keys = ('echo', 'noise', 'speech', 'duck')
    if all(res.get(k) == 'PASS' for k in keys):
        out('PASS VPIO verdict: usable echo cancellation and noise suppression on this tablet')
    elif any(res.get(k) == 'FAIL' for k in keys):
        out('FAIL VPIO verdict: %s' % ', '.join('%s %s' % (k, res.get(k)) for k in keys))
    else:
        out('VOID VPIO verdict: %s' % ', '.join('%s %s' % (k, res.get(k)) for k in keys))
    print('\n'.join(lines))
    return 0


# ---------------------------------------------------------------- self-test (synthetic, host only)
def synth(assets, d, tag, echo_db, noise_db, speech_db, D, far_start, c920_music_db=0.0, zeros=False, other=False, delay=0.14):
    _, far8 = read_wav(os.path.join(assets, 'far8k.wav'))
    _, near8 = read_wav(os.path.join(assets, 'near8k.wav'))
    rnd = random.Random(sum(map(ord, tag)))
    n = PROBE_SECONDS * FA
    ns = NEAR['noise']
    nscale = [1.0] * len(near8)
    for i in range(int(ns[0] * FA), min(len(near8), int(ns[1] * FA))):
        nscale[i] = 10 ** (noise_db / 20)
    taps = [(0, 1.0), (23, 0.5), (61, -0.3), (140, 0.2), (233, 0.1)]
    fo = int(round((far_start + delay) * FA))
    y = [rnd.gauss(0, 10 ** (-80 / 20)) for _ in range(n)]
    ge = 0.25 * 10 ** (echo_db / 20); gs = 0.1 * 10 ** (speech_db / 20)
    for i in range(n):
        acc = 0.0
        for k, w in taps:
            j = i - fo - k
            if 0 <= j < len(far8):
                acc += w * far8[j]
        j = i - int(round(D * FA))
        nv = near8[j] * nscale[j] if 0 <= j < len(near8) else 0.0
        y[i] += ge * acc + gs * nv
    if zeros:
        y = [0.0] * n
    write_wav(os.path.join(d, tag + '.wav'), FA, y)
    K = 2.3
    nc = int((PROBE_SECONDS + 10) * FA)
    c = [rnd.gauss(0, 10 ** (-70 / 20)) for _ in range(nc)]
    gm = 0.3 * 10 ** (c920_music_db / 20)
    for i in range(nc):
        j = i - int(round((far_start + delay + K) * FA))
        m = far8[j] if 0 <= j < len(far8) else 0.0
        jn = i - int(round((D + K) * FA))
        nv = near8[jn] if 0 <= jn < len(near8) else 0.0
        c[i] += gm * m + 0.2 * nv
    write_wav(os.path.join(d, 'c920-%s.wav' % tag), FA, c)
    with open(os.path.join(d, tag + '-probe.txt'), 'w') as f:
        f.write('vpio-probe: mode %s seconds 66.0 rate 8000\nvpio-probe: tcc mic_status=3\n' % ('vpio' if not other else 'vpio'))
        if not other:
            f.write('vpio-probe: timing far-start-in-rec-s %.4f out-first-in-rec-s %.4f\n' % (far_start, far_start))
        f.write('vpio-probe: readback bypass 0 agc 1 mute-output 0 ducking advanced 0 level 10 (min) rc 0\n')
        f.write('vpio-probe: overloads input 0 output 0 unit 0\n')
        f.write('vpio-probe: callbacks 100 frames %d expected %d gaps 0 (largest 0 frames at 0.00 s) peak -6.0 dBFS rms -30.0 dBFS '
                'zeros %.1f%% clipped 0 render-errors 0 (last 0) oversize 0\n' % (n, n, 100.0 if zeros else 0.0))


def selftest(a):
    d = a.dir
    os.makedirs(d, exist_ok=True)
    assets = os.path.join(d, 'assets')
    if not os.path.isfile(os.path.join(assets, 'schedule.json')):
        stimuli(argparse.Namespace(dir=assets, rate=24000, voice=a.voice))
    # decimation: a 1 kHz sine passes, a 6 kHz sine is rejected
    s1 = [math.sin(2 * math.pi * 1000 * i / 24000) for i in range(24000)]
    s6 = [math.sin(2 * math.pi * 6000 * i / 24000) for i in range(24000)]
    p1, p6 = db(power(to_fa(24000, s1)[800:-800])), db(power(to_fa(24000, s6)[800:-800]))
    ok = abs(p1 + 3.01) < 0.3 and p6 < -45
    print('selftest decimation: 1 kHz %.2f dBFS, 6 kHz %.1f dBFS: %s' % (p1, p6, 'ok' if ok else 'BAD'))
    cases = {  # tag: echo, noise, speech, D, far_start, c920 music, zeros, other
        'vpio': (-25, -10, 0, 1.3, 0.05, 0, False, False), 'bypass': (0, 0, 0, 1.6, 0.04, 0, False, False),
        'raw': (0, 0, 0, 1.1, 0.06, 0, False, False), 'other': (0, 0, 0, 1.4, 1.0, 0, False, True),
        'vpio2': (-25, -10, 2, 1.5, 0.05, 0, False, False),
        'weak': (-8, -10, 0, 1.3, 0.05, 0, False, False), 'ducked': (-25, -10, 0, 1.3, 0.05, -6, False, False),
        'denied': (0, 0, 0, 1.3, 0.05, 0, True, False)}
    runs = {}
    for name in ('pass', 'weak', 'ducked', 'denied'):
        runs[name] = os.path.join(d, name)
        os.makedirs(runs[name], exist_ok=True)
    jobs = []
    for tag, c in cases.items():
        where = runs['pass'] if tag in ('vpio', 'bypass', 'raw', 'other', 'vpio2') else runs[tag]
        mt = 'vpio' if tag in ('weak', 'ducked', 'denied') else tag
        jobs.append((assets, where, mt, c))
    with concurrent.futures.ProcessPoolExecutor() as ex:
        list(ex.map(_selftest_one, jobs))
    for name in ('weak', 'ducked', 'denied'):
        for t in ('bypass', 'raw'):
            for suffix in ('-measure.json',):
                src = os.path.join(runs['pass'], t + suffix)
                open(os.path.join(runs[name], t + suffix), 'w').write(open(src).read())
    expect = {'pass': 'PASS VPIO verdict', 'weak': 'FAIL VPIO verdict', 'ducked': 'VOID VPIO verdict', 'denied': 'VOID VPIO verdict'}
    for name, want in expect.items():
        buf = subprocess.run([sys.executable, os.path.abspath(__file__), 'verdict', runs[name], 'vpio', 'bypass', 'raw', 'other', 'vpio2'],
                             capture_output=True, text=True).stdout
        open(os.path.join(runs[name], 'verdict.txt'), 'w').write(buf)
        got = want in buf
        ok = ok and got
        print('selftest %s: %s' % (name, 'ok' if got else 'BAD'))
        print('  ' + buf.strip().replace('\n', '\n  '))
    if 'pass' in runs:
        v = json.load(open(os.path.join(runs['pass'], 'vpio-measure.json')))
        b = json.load(open(os.path.join(runs['pass'], 'bypass-measure.json')))
        checks = [('echo', b['echo'] - v['echo'], 25, 2.5), ('echo_coh', b['echo_coh'] - v['echo_coh'], 25, 3),
                  ('noise', b['noise'] - v['noise'], 10, 2.5), ('speech', v['speech'] - b['speech'], 0, 1.5),
                  ('D vpio', v['D'], 1.3, 0.02), ('D bypass', b['D'], 1.6, 0.02), ('delay', b['delay']['s'], 0.14, 0.01)]
        for k, got, want, tol in checks:
            good = abs(got - want) <= tol
            ok = ok and good
            print('selftest %s: %.2f (want %.2f +- %.2f) %s' % (k, got, want, tol, 'ok' if good else 'BAD'))
    print('SELFTEST %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def _selftest_one(job):
    assets, where, tag, c = job
    echo, noise, sp, D, fs, cm, zeros, other = c
    synth(assets, where, tag, echo, noise, sp, D, fs, cm, zeros, other)
    ns = argparse.Namespace(assets=assets, run=where, tag=tag, other=other)
    with open(os.path.join(where, tag + '-measure.log'), 'w') as f:
        old = sys.stdout
        sys.stdout = f
        try:
            block(ns)
        finally:
            sys.stdout = old


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('stimuli'); s.add_argument('dir'); s.add_argument('--rate', type=int, default=24000)
    s.add_argument('--voice', default='Samantha')
    s = sub.add_parser('block'); s.add_argument('assets'); s.add_argument('run'); s.add_argument('tag')
    s.add_argument('--other', action='store_true')
    s = sub.add_parser('verdict'); s.add_argument('run'); s.add_argument('tags', nargs='*')
    s = sub.add_parser('selftest'); s.add_argument('dir'); s.add_argument('--voice', default='Samantha')
    a = ap.parse_args()
    return {'stimuli': stimuli, 'block': block, 'verdict': verdict, 'selftest': selftest}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
