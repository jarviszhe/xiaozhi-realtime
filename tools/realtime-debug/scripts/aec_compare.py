"""Compare double-talk captures with independent acoustic near/far baselines.

Correlation and projection gain are diagnostic estimates, not ERLE or perceptual
speech scores. The room, PC volume, sources and device volume must stay fixed.
"""

import argparse
import json
import math
from pathlib import Path

import numpy as np

from aec_test import read_wave, write_wave

RATE = 16000


def match(samples, template, max_lag_seconds):
    count = min(len(samples), len(template))
    if count < RATE:
        raise ValueError("Recordings must contain at least one second")
    a = samples[:count] - np.mean(samples[:count])
    b = template[:count] - np.mean(template[:count])
    size = 1 << (2 * count - 1).bit_length()
    values = np.fft.irfft(np.fft.rfft(a, size) * np.conj(np.fft.rfft(b, size)), size)
    maximum = min(round(max_lag_seconds * RATE), count - 1)
    lags = np.arange(-maximum, maximum + 1)
    lag = int(lags[np.argmax(np.abs(values[lags % size]))])
    norm = np.linalg.norm(a) * np.linalg.norm(b)
    if norm == 0:
        raise ValueError("Cannot compare silent recordings")
    return lag, float(values[lag % size] / norm)


def compare(folder, near, far):
    result = {"folder": str(folder.resolve())}
    for track in ("aec_mic", "aec_out"):
        samples = read_wave(folder / (track + ".wav"), RATE)
        lag, correlation = match(samples, near, 3)
        shifted = np.interp(np.arange(len(samples)) - lag, np.arange(len(near)), near,
                            left=0, right=0)
        # Use near speech energy rather than the silent leading/trailing regions.
        first = max(0, lag)
        last = min(len(samples), len(near) + lag)
        gain = float(np.dot(samples[first:last], shifted[first:last]) /
                     np.dot(shifted[first:last], shifted[first:last]))
        far_lag, far_correlation = match(samples, far, 0.25)
        result[track] = {"near_correlation": correlation, "near_projection_gain": gain,
                         "near_lag_samples": lag, "far_correlation": far_correlation,
                         "far_lag_samples": far_lag}
    mic_gain = result["aec_mic"]["near_projection_gain"]
    out_gain = result["aec_out"]["near_projection_gain"]
    result["near_gain_change_db"] = (20 * math.log10(abs(out_gain / mic_gain))
                                     if mic_gain and out_gain else None)
    return result


def export_pair(result, destination):
    folder = Path(result["folder"])
    mic = read_wave(folder / "aec_mic.wav", RATE)
    out = read_wave(folder / "aec_out.wav", RATE)
    # AFE startup/reset can discard a short prefix. Align by the independent
    # near-only recording, applying only a constant shift and common gain.
    shift = result["aec_mic"]["near_lag_samples"] - result["aec_out"]["near_lag_samples"]
    aligned = np.interp(np.arange(len(mic)) - shift, np.arange(len(out)), out,
                        left=0, right=0)
    peak = max(float(np.max(np.abs(mic))), float(np.max(np.abs(aligned))))
    gain = 0.9 * 32767 / peak if peak else 1.0
    destination.mkdir(parents=True, exist_ok=True)
    write_wave(destination / "double-talk-before.wav", mic * gain, RATE)
    write_wave(destination / "double-talk-after.wav", aligned * gain, RATE)
    comparison = np.concatenate((mic, np.zeros(RATE), aligned)) * gain
    write_wave(destination / "double-talk-before-then-after.wav", comparison, RATE)
    manifest = {"selected": result, "common_gain": gain,
                "output_shift_samples": shift, "sample_rate": RATE,
                "processing": "constant alignment and identical gain only; original tracks retained",
                "order": "whole raw capture, one second silence, whole AEC output"}
    (destination / "listening-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--near-only", required=True, type=Path,
                        help="Microphone WAV from PC-only acoustic playback")
    parser.add_argument("--far-only", required=True, type=Path,
                        help="Microphone WAV from device-only acoustic playback")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    near = read_wave(args.near_only, RATE)
    far = read_wave(args.far_only, RATE)
    results = [compare(folder, near, far) for folder in args.captures]
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "comparison.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    best = max(results, key=lambda result: result["aec_out"]["near_correlation"])
    export_pair(best, args.output)
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
