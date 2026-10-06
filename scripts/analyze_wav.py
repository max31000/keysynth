#!/usr/bin/env python3
"""Analyze a rendered WAV (docs/TESTING.md).

Prints peak / RMS / DC per channel, fundamental estimate, spectral centroid and an envelope summary.
Optional: --spectrogram out.png, --start/--end to analyze a time window, --json for machine-readable output.

    python scripts/analyze_wav.py renders/out.wav
    python scripts/analyze_wav.py renders/out.wav --start 0.2 --end 0.9 --spectrogram renders/out.png

Dependencies (numpy, scipy, matplotlib) are installed with pip on first use if missing.
"""
import argparse
import importlib
import json
import subprocess
import sys


def ensure(pkgs):
    missing = []
    for mod, pip_name in pkgs:
        try:
            importlib.import_module(mod)
        except ImportError:
            missing.append(pip_name)
    if missing:
        print(f"installing {' '.join(missing)} ...", file=sys.stderr)
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--quiet", *missing])


ensure([("numpy", "numpy"), ("scipy", "scipy"), ("matplotlib", "matplotlib")])

import numpy as np  # noqa: E402
from scipy.io import wavfile  # noqa: E402
from scipy import signal  # noqa: E402


def db(x):
    return 20.0 * np.log10(max(float(x), 1e-12))


def load(path):
    sr, data = wavfile.read(path)
    if data.dtype.kind == "i":
        data = data.astype(np.float64) / float(np.iinfo(data.dtype).max)
    elif data.dtype.kind == "u":
        data = (data.astype(np.float64) - 128.0) / 128.0
    else:
        data = data.astype(np.float64)
    if data.ndim == 1:
        data = data[:, None]
    return sr, data


def fundamental(x, sr, fmin=20.0, fmax=5000.0):
    """Autocorrelation pitch estimate with parabolic interpolation, refined by the spectral peak near it."""
    x = x - np.mean(x)
    if np.max(np.abs(x)) < 1e-6:
        return None
    n = min(len(x), int(sr * 1.0))
    seg = x[:n] * np.hanning(n)
    spec = np.fft.rfft(seg, 2 * n)
    ac = np.fft.irfft(np.abs(spec) ** 2)[:n]
    ac /= ac[0] + 1e-12
    lo, hi = int(sr / fmax), min(int(sr / fmin), n - 2)
    if hi <= lo + 2:
        return None
    region = ac[lo:hi]
    # first peak above 0.5 of the global max in range (avoids octave errors on harmonic-rich signals)
    peaks, _ = signal.find_peaks(region)
    if len(peaks) == 0:
        return None
    best = region[peaks].max()
    cand = [p for p in peaks if region[p] >= 0.85 * best]
    p = cand[0] + lo
    a, b, c = ac[p - 1], ac[p], ac[p + 1]
    denom = a - 2 * b + c
    shift = 0.5 * (a - c) / denom if denom != 0 else 0.0
    return sr / (p + shift)


def centroid(x, sr):
    f, pxx = signal.welch(x, sr, nperseg=min(4096, len(x)))
    return float(np.sum(f * pxx) / (np.sum(pxx) + 1e-20))


def envelope(x, sr, hop_s=0.01):
    hop = max(1, int(sr * hop_s))
    frames = len(x) // hop
    if frames == 0:
        return np.array([]), np.array([])
    rms = np.sqrt(np.mean(x[: frames * hop].reshape(frames, hop) ** 2, axis=1))
    t = np.arange(frames) * hop_s
    return t, rms


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--start", type=float, default=0.0, help="analysis window start (s) for pitch/centroid")
    ap.add_argument("--end", type=float, default=None, help="analysis window end (s)")
    ap.add_argument("--spectrogram", metavar="PNG", help="write a spectrogram + envelope plot")
    ap.add_argument("--json", action="store_true", help="print results as JSON")
    args = ap.parse_args()

    sr, data = load(args.wav)
    mono = data.mean(axis=1)
    res = {"file": args.wav, "sample_rate": int(sr), "channels": int(data.shape[1]),
           "duration_s": round(len(mono) / sr, 4), "channels_stats": []}
    for ch in range(data.shape[1]):
        x = data[:, ch]
        res["channels_stats"].append({
            "peak_dbfs": round(db(np.max(np.abs(x))), 2),
            "rms_dbfs": round(db(np.sqrt(np.mean(x ** 2))), 2),
            "dc": float(np.mean(x)),
            "nan_or_inf": int(np.sum(~np.isfinite(x))),
        })
    a = int(args.start * sr)
    b = int(args.end * sr) if args.end else len(mono)
    win = mono[a:b]
    f0 = fundamental(win, sr) if len(win) > 64 else None
    res["fundamental_hz"] = round(f0, 2) if f0 else None
    if f0:
        midi = 69 + 12 * np.log2(f0 / 440.0)
        names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        n = int(round(midi))
        res["fundamental_note"] = f"{names[n % 12]}{n // 12 - 1} ({(midi - n) * 100:+.1f} cents)"
    res["spectral_centroid_hz"] = round(centroid(win, sr), 1) if len(win) > 64 else None

    t, env = envelope(mono, sr)
    if len(env):
        pk = int(np.argmax(env))
        env_db = 20 * np.log10(np.maximum(env, 1e-9))
        peak_db = env_db[pk]
        above = np.where(env_db > peak_db - 60)[0]
        res["envelope"] = {
            "peak_time_s": round(float(t[pk]), 3),
            "peak_rms_dbfs": round(float(peak_db), 2),
            "attack_to_peak_s": round(float(t[pk] - t[above[0]]), 3) if len(above) else None,
            "last_above_-60dB_rel_s": round(float(t[above[-1]]), 3) if len(above) else None,
            "tail_last_100ms_rms_dbfs": round(db(np.sqrt(np.mean(mono[-int(0.1 * sr):] ** 2))), 2),
            "rms_dbfs_per_250ms": [round(float(v), 1) for v in
                                   20 * np.log10(np.maximum(envelope(mono, sr, 0.25)[1], 1e-9))],
        }

    if args.spectrogram:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), sharex=True,
                                       gridspec_kw={"height_ratios": [3, 1]})
        f, tt, sxx = signal.spectrogram(mono, sr, nperseg=2048, noverlap=1536)
        ax1.pcolormesh(tt, f, 10 * np.log10(sxx + 1e-14), shading="auto", vmin=-140, vmax=-20, cmap="magma")
        ax1.set_ylim(0, min(sr / 2, 12000))
        ax1.set_ylabel("Hz")
        ax1.set_title(args.wav)
        ax2.plot(t, 20 * np.log10(np.maximum(env, 1e-9)))
        ax2.set_ylim(-100, 0)
        ax2.set_ylabel("RMS dBFS")
        ax2.set_xlabel("s")
        fig.tight_layout()
        fig.savefig(args.spectrogram, dpi=110)
        res["spectrogram"] = args.spectrogram

    if args.json:
        print(json.dumps(res, indent=2))
        return
    print(f"{res['file']}: {res['duration_s']} s, {sr} Hz, {res['channels']} ch")
    for i, c in enumerate(res["channels_stats"]):
        print(f"  ch{i}: peak {c['peak_dbfs']} dBFS, RMS {c['rms_dbfs']} dBFS, DC {c['dc']:.2e}, "
              f"non-finite {c['nan_or_inf']}")
    print(f"  fundamental: {res['fundamental_hz']} Hz {res.get('fundamental_note', '')}"
          f"  (window {args.start}-{args.end or res['duration_s']} s)")
    print(f"  spectral centroid: {res['spectral_centroid_hz']} Hz")
    if "envelope" in res:
        e = res["envelope"]
        print(f"  envelope: peak {e['peak_rms_dbfs']} dBFS at {e['peak_time_s']} s, attack {e['attack_to_peak_s']} s, "
              f"> -60 dB(rel) until {e['last_above_-60dB_rel_s']} s, last 100 ms {e['tail_last_100ms_rms_dbfs']} dBFS")
        print(f"  RMS per 250 ms: {e['rms_dbfs_per_250ms']}")
    if args.spectrogram:
        print(f"  spectrogram: {args.spectrogram}")


if __name__ == "__main__":
    main()
