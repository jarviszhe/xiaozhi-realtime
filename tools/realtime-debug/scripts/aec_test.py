"""Run controlled Waveshare AEC captures (Python 3, NumPy, Windows for near playback)."""

import argparse
import json
import math
import time
import urllib.parse
import urllib.request
import wave
from pathlib import Path

import numpy as np


def get_json(url):
    with urllib.request.urlopen(url, timeout=15) as response:
        return json.load(response)


def read_wave(path, rate=24000):
    with wave.open(str(path), "rb") as source:
        if source.getsampwidth() != 2:
            raise ValueError("Expected 16 bit PCM WAV")
        samples = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2")
        samples = samples.reshape(-1, source.getnchannels()).astype(float).mean(axis=1)
        source_rate = source.getframerate()
    if source_rate != rate:
        count = round(len(samples) * rate / source_rate)
        samples = np.interp(np.arange(count) * source_rate / rate, np.arange(len(samples)), samples)
    return samples


def write_wave(path, samples, rate):
    with wave.open(str(path), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(rate)
        target.writeframes(np.clip(samples, -32768, 32767).astype("<i2").tobytes())


def rms(samples):
    return float(np.sqrt(np.mean(samples * samples))) if len(samples) else 0.0


def correlation(a, b, max_lag=4000):
    count = min(len(a), len(b))
    a = a[:count] - np.mean(a[:count])
    b = b[:count] - np.mean(b[:count])
    size = 1 << (2 * count - 1).bit_length()
    result = np.fft.irfft(np.fft.rfft(a, size) * np.conj(np.fft.rfft(b, size)), size)
    lags = np.arange(-min(max_lag, count - 1), min(max_lag, count - 1) + 1)
    values = result[lags % size]
    index = int(np.argmax(np.abs(values)))
    norm = np.linalg.norm(a) * np.linalg.norm(b)
    return {"lag_ms": float(lags[index]) / 16, "correlation": float(values[index] / norm) if norm else 0}


def resource_delta(before, after):
    """CPU uses task runtime, while feed timing is explicitly elapsed wall time."""
    seconds = (after["time_us"] - before["time_us"]) / 1e6
    if seconds <= 0:
        raise ValueError("Resource snapshots must have increasing timestamps")
    result = {"window_seconds": seconds,
              "internal_free_before": before["internal_free"],
              "internal_free_after": after["internal_free"],
              "internal_min_since_boot": after["internal_min_since_boot"],
              "internal_largest_after": after["internal_largest"],
              "psram_free_before": before["psram_free"],
              "psram_free_after": after["psram_free"],
              "psram_min_since_boot": after["psram_min_since_boot"]}
    engine = {key: (after["engine"][key] - before["engine"][key]) % (1 << 32)
              for key in ("feed_frames", "feed_samples", "feed_wall_us", "feed_overruns",
                          "fetch_frames", "fetch_samples", "fetch_errors")}
    engine["feed_average_ms"] = (engine["feed_wall_us"] / engine["feed_frames"] / 1000
                                  if engine["feed_frames"] else None)
    engine["feed_max_ms_since_boot"] = after["engine"]["feed_max_us_since_boot"] / 1000
    engine["feed_audio_seconds_per_second"] = engine["feed_samples"] / 16000 / seconds
    engine["fetch_audio_seconds_per_second"] = engine["fetch_samples"] / 16000 / seconds
    engine["backlog_change_samples"] = engine["feed_samples"] - engine["fetch_samples"]
    result["engine"] = engine
    if not before.get("runtime_valid") or not after.get("runtime_valid"):
        result["cpu_valid"] = False
        return result
    modulus = 1 << after["runtime_bits"]
    elapsed = (after["runtime_total"] - before["runtime_total"]) % modulus
    if not elapsed or before["runtime_bits"] != after["runtime_bits"]:
        raise ValueError("Invalid runtime counter snapshots")
    start = {task["id"]: task for task in before["tasks"]}
    tasks = []
    for task in after["tasks"]:
        previous = start.get(task["id"])
        if previous is None or previous["name"] != task["name"]:
            continue
        runtime = (task["runtime"] - previous["runtime"]) % modulus
        tasks.append({"name": task["name"], "one_core_percent": 100 * runtime / elapsed,
                      "total_capacity_percent": 100 * runtime / elapsed / after["cores"],
                      "stack_min_free_bytes": task["stack_min_free_bytes"]})
    idle = [task for task in tasks if task["name"] in ("IDLE0", "IDLE1")]
    result["cpu_valid"] = len(idle) == after["cores"]
    result["cpu_busy_total_percent"] = (100 - sum(task["total_capacity_percent"] for task in idle)
                                          if result["cpu_valid"] else None)
    result["tasks"] = sorted(tasks, key=lambda task: -task["one_core_percent"])
    return result


def analyze(folder):
    tracks = {track: read_wave(folder / (track + ".wav"), 16000)
              for track in ("aec_mic", "aec_ref", "aec_out")}
    summary = {}
    for name, samples in tracks.items():
        # Separate startup history from the sustained recording.
        middle = samples[16000:-16000] if len(samples) > 48000 else samples
        summary[name] = {"rms": rms(samples), "middle_rms": rms(middle),
                         "peak": float(np.max(np.abs(samples))),
                         "clipped": int(np.count_nonzero(np.abs(samples) >= 32767)),
                         "seconds": len(samples) / 16000,
                         "rms_per_second": [rms(samples[i:i + 16000])
                                            for i in range(0, len(samples), 16000)]}
    for name in ("aec_mic", "aec_out"):
        summary[name + "_ref"] = correlation(tracks[name][16000:-8000],
                                              tracks["aec_ref"][16000:-8000])
    raw, out = summary["aec_mic"]["middle_rms"], summary["aec_out"]["middle_rms"]
    summary["middle_attenuation_db"] = 20 * math.log10(raw / out) if raw > 0 and out > 0 else None
    reference = tracks["aec_ref"]
    active = np.flatnonzero(np.abs(reference) > max(1, np.max(np.abs(reference)) * 0.01))
    if len(active):
        first = int(active[0])
        summary["reference_onset_seconds"] = first / 16000
        summary["onset_attenuation_db"] = {}
        for duration in (0.5, 1, 2):
            last = min(first + round(duration * 16000), len(tracks["aec_out"]))
            raw = rms(tracks["aec_mic"][first:last])
            out = rms(tracks["aec_out"][first:last])
            summary["onset_attenuation_db"][str(duration)] = (
                20 * math.log10(raw / out) if raw and out else None)
    return summary


def capture(args, pcm, delay, enabled, repetition):
    delay_label = "default" if delay is None else f"{delay:03d}"
    label = f"{args.label}-d{delay_label}-aec{enabled}-r{repetition}"
    folder = args.output / label
    folder.mkdir(parents=True, exist_ok=False)
    existing = {session["id"] for session in get_json(args.collector + "/api/sessions")}
    options = {"aec": enabled}
    if delay is not None:
        options["delay_ms"] = delay
    query = urllib.parse.urlencode(options)
    request = urllib.request.Request(args.device + "/api/test/pcm?" + query,
                                     data=pcm, headers={"Content-Type": "application/octet-stream"})
    started = time.time()
    near_trigger = {}
    if args.near:
        import threading
        import winsound
        stop_near = threading.Event()

        def play_near():
            # Upload time varies under idle Wi-Fi power save. Anchor the PC
            # voice to the device capture opening, rather than the HTTP request.
            try:
                deadline = time.monotonic() + 30
                while not stop_near.is_set() and time.monotonic() < deadline:
                    live = [session for session in get_json(args.collector + "/api/sessions")
                            if session["id"] not in existing and session["live"]
                            and session["mode"] == "manual"]
                    if len(live) == 1:
                        near_trigger.update(session=live[0]["id"], observed_at=time.time())
                        if not stop_near.wait(args.near_start):
                            winsound.PlaySound(str(args.near.resolve()),
                                               winsound.SND_FILENAME | winsound.SND_ASYNC)
                            near_trigger["played_at"] = time.time()
                        return
                    stop_near.wait(0.05)
                if not stop_near.is_set():
                    near_trigger["error"] = "No new manual session within 30 seconds"
            except Exception as error:
                near_trigger["error"] = str(error)

        near_thread = threading.Thread(target=play_near)
        near_thread.start()
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            reply = json.load(response)
    finally:
        if args.near:
            stop_near.set()
            near_thread.join()
            winsound.PlaySound(None, 0)
    if not reply.get("ok"):
        raise RuntimeError(reply)
    (folder / "device-reply.json").write_text(json.dumps(reply, indent=2), encoding="utf-8")
    if args.near:
        (folder / "near-trigger.json").write_text(json.dumps(near_trigger, indent=2), encoding="utf-8")
        if "error" in near_trigger or "played_at" not in near_trigger:
            raise RuntimeError(f"Near playback was not triggered: {near_trigger}")
    deadline = time.time() + 12
    sessions = []
    while time.time() < deadline:
        sessions = [session for session in get_json(args.collector + "/api/sessions")
                    if session["id"] not in existing and not session["live"]]
        if sessions:
            break
        time.sleep(0.25)
    if len(sessions) != 1:
        raise RuntimeError(f"Expected one completed capture, found {sessions}")
    session = sessions[0]
    if args.near and near_trigger.get("session") != session["id"]:
        raise RuntimeError("Near speech was anchored to a different capture")
    url = args.collector + "/api/sessions/" + session["id"]
    report = get_json(url + "/aec-report")
    info = get_json(url)
    for track in ("aec_mic", "aec_ref", "aec_out", "playback"):
        if len(info.get(track, [])) != 1:
            raise RuntimeError(f"Expected one segment for {track}: {info}")
        with urllib.request.urlopen(url + f"/audio/{track}/0.wav", timeout=20) as audio:
            (folder / (track + ".wav")).write_bytes(audio.read())
    summary = {"label": label, "delay_ms": delay, "aec": enabled, "session": session,
               "collector_report": report, "elapsed": time.time() - started,
               "source": str(args.source.resolve()), "playback_peak": args.peak,
               "source_seconds": args.seconds,
               "near": str(args.near.resolve()) if args.near else None,
               "near_start": args.near_start if args.near else None,
               "near_trigger": near_trigger if args.near else None,
               "analysis": analyze(folder)}
    if reply.get("stats_before") and reply.get("stats_after"):
        summary["resources"] = resource_delta(reply["stats_before"], reply["stats_after"])
        before = reply["stats_before"]
        summary["reference_source"] = ("codec_dac" if before.get("codec_reference")
                                       else "software_pcm")
        summary["input_gain_db"] = before.get("input_gain_db")
        summary["output_volume"] = before.get("output_volume")
    (folder / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if not report["complete"] or report["gaps"] or session["interrupted"]:
        raise RuntimeError(f"Invalid capture saved in {folder}")
    print(json.dumps({"label": label, "session": session["id"],
                      "attenuation_db": report.get("overall_attenuation_db"),
                      "onset_db": summary["analysis"].get("onset_attenuation_db"),
                      "resources": summary.get("resources")}), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", default="http://192.168.5.15")
    parser.add_argument("--collector", default="http://127.0.0.1:7320")
    parser.add_argument("--delays", default="0,10,20,30,40,80",
                        help="Comma-separated delays in ms, or default to test the firmware default")
    parser.add_argument("--aec", choices=(0, 1), type=int, default=1)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--label", default="speech")
    parser.add_argument("--peak", type=float, default=0.04)
    parser.add_argument("--seconds", type=float, default=16)
    parser.add_argument("--near", type=Path)
    parser.add_argument("--near-start", type=float, default=4,
                        help="Seconds after observing the new device trace opening")
    args = parser.parse_args()
    if not 0 <= args.peak <= 0.5 or not 2 <= args.seconds <= 20:
        parser.error("peak must be 0..0.5 and seconds must be 2..20")
    try:
        delays = [None if value.strip() == "default" else int(value)
                  for value in args.delays.split(",")]
    except ValueError:
        parser.error("delays must be 0..250 ms or default")
    if any(value is not None and not 0 <= value <= 250 for value in delays):
        parser.error("delays must be 0..250 ms or default")
    if args.repeats < 1 or args.near_start < 0:
        parser.error("repeats must be positive and near-start must not be negative")
    args.output.mkdir(parents=True, exist_ok=True)
    source = read_wave(args.source)
    if len(source) == 0 or np.max(np.abs(source)) == 0:
        parser.error("source must contain audio")
    source = source * (args.peak * 32767 / np.max(np.abs(source)))
    # Repeat a known source, preserving silence at both ends for noise measurements.
    body_count = int((args.seconds - 1.5) * 24000)
    samples = np.concatenate((np.zeros(12000), np.resize(source, body_count), np.zeros(24000)))
    write_wave(args.output / (args.label + "-source.wav"), samples, 24000)
    pcm = np.clip(samples, -32768, 32767).astype("<i2").tobytes()
    results = []
    for repetition in range(1, args.repeats + 1):
        # Reverse the repeat order so a warm filter cannot always favor the last delay.
        for delay in delays if repetition % 2 else reversed(delays):
            results.append(capture(args, pcm, delay, args.aec, repetition))
            (args.output / (args.label + "-results.json")).write_text(
                json.dumps(results, indent=2), encoding="utf-8")
            time.sleep(0.5)


if __name__ == "__main__":
    main()
