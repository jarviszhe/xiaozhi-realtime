"""Run board Realtime checks using local WAV speech and the existing trace collector.

Requires Windows speaker playback, enabled device debug capture, and a connected
board. Starts interruption only after the collector observes actual playback.
"""

import argparse
import json
from pathlib import Path
import time
import urllib.error
import urllib.request


def get_json(url):
    with urllib.request.urlopen(url, timeout=10) as response:
        return json.load(response)


def action(device, name):
    request = urllib.request.Request(device + "/api/test/chat?action=" + name, data=b"", method="POST")
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def wait_for(description, predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.2)
    raise TimeoutError(description)


def play(path):
    import winsound
    winsound.PlaySound(str(path.resolve()), winsound.SND_FILENAME)


def select_mode(device, mode):
    for _ in range(4):
        state = get_json(device + "/api/test/stats")
        if state["mode"] == mode and state["state"] == "idle":
            return
        action(device, "cycle")
        time.sleep(0.3)
    raise RuntimeError("Requested mode unavailable: " + mode)


def collect(collector, session, output):
    base = collector + "/api/sessions/" + session
    info = get_json(base)
    (output / "session.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    events, after = [], ""
    while True:
        page = get_json(base + "/events?limit=1000&after=" + after)
        if not page:
            break
        events.extend(page)
        after = page[-1]["meta"]["seq"]
    timeline = []
    for event in events:
        meta = event["meta"]
        if event["kind"] == "local_event":
            timeline.append({"time_us": meta.get("mono_us"), "type": meta.get("event_type"),
                             "detail": meta.get("detail")})
        elif event["kind"] == "ws_rx" and event.get("payload_length", 0) < 8000:
            try:
                value = get_json(base + "/events/payload/" + meta["seq"])
                timeline.append({"time_us": meta.get("mono_us"), "cloud": value})
            except (ValueError, urllib.error.HTTPError):
                pass
    (output / "events.json").write_text(json.dumps(events, indent=2), encoding="utf-8")
    (output / "timeline.json").write_text(json.dumps(timeline, indent=2, ensure_ascii=False), encoding="utf-8")
    for track in ("uplink", "playback", "aec_mic", "aec_ref", "aec_out"):
        for index, _ in enumerate(info.get(track) or []):
            with urllib.request.urlopen(base + f"/audio/{track}/{index}.wav", timeout=30) as response:
                (output / f"{track}-{index}.wav").write_bytes(response.read())
    return info, events, timeline


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="http://192.168.5.15")
    parser.add_argument("--collector", default="http://127.0.0.1:7320")
    parser.add_argument("--mode", choices=("manual", "auto", "realtime"), default="realtime")
    parser.add_argument("--question", type=Path, required=True)
    parser.add_argument("--interrupt", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--interrupt-delay", type=float, default=1)
    args = parser.parse_args()
    if args.interrupt and args.mode != "realtime":
        parser.error("Interruption scenario requires realtime mode")
    args.output.mkdir(parents=True, exist_ok=True)
    action(args.device, "stop")
    wait_for("idle", lambda: get_json(args.device + "/api/test/stats")["state"] == "idle")
    select_mode(args.device, args.mode)
    known = {s["id"] for s in get_json(args.collector + "/api/sessions")}
    resources = []
    markers = []
    session = None

    def timed_play(label, path):
        before = time.monotonic()
        clock = get_json(args.device + "/api/test/stats")
        after = time.monotonic()
        midpoint = (before + after) / 2
        for phase in ("start", "end"):
            marker = {"type": label + "_" + phase,
                      "device_time_us_estimate": clock["time_us"] + int((time.monotonic() - midpoint) * 1e6),
                      "clock_uncertainty_us": int((after - before) * 5e5)}
            markers.append(marker)
            if phase == "start":
                play(path)

    try:
        action(args.device, "click")
        wait_for("listening", lambda: get_json(args.device + "/api/test/stats")["state"] == "listening")
        session = wait_for("new trace", lambda: next((s["id"] for s in get_json(
            args.collector + "/api/sessions") if s["id"] not in known and s["live"]), None))
        resources.append(get_json(args.device + "/api/test/stats"))
        timed_play("question", args.question)
        if args.mode == "manual":
            action(args.device, "click")
        base = args.collector + "/api/sessions/" + session
        wait_for("actual speaker playback", lambda: get_json(base).get("playback"), timeout=45)
        resources.append(get_json(args.device + "/api/test/stats"))
        if args.interrupt:
            time.sleep(args.interrupt_delay)
            print("Observed device playback; playing near speech", flush=True)
            timed_play("interrupt", args.interrupt)
            time.sleep(12)
        else:
            wait_for("reply completed", lambda: get_json(args.device + "/api/test/stats")["state"]
                     == ("idle" if args.mode == "manual" else "listening"), timeout=90)
        resources.append(get_json(args.device + "/api/test/stats"))
    finally:
        action(args.device, "stop")
        if session:
            wait_for("trace sender drained and session closed", lambda: next((s for s in get_json(
                args.collector + "/api/sessions") if s["id"] == session and not s["live"]), None), timeout=30)
        (args.output / "resources.json").write_text(json.dumps(resources, indent=2), encoding="utf-8")
        (args.output / "markers.json").write_text(json.dumps(markers, indent=2), encoding="utf-8")
        if session:
            info, events, timeline = collect(args.collector, session, args.output)
            summary = info["summary"]
            if summary["live"] or summary["gaps"] or summary["truncated"] or summary["interrupted"]:
                raise RuntimeError("Incomplete trace; retained for diagnosis")
            if args.interrupt:
                starts = [t for t in timeline if t.get("cloud", {}).get("type") == "input_audio_buffer.speech_started"]
                if len(starts) != 2:
                    raise RuntimeError(f"Expected two user speech turns, received {len(starts)}")
                cut = starts[1]["time_us"]
                near_start = next(m["device_time_us_estimate"] for m in markers if m["type"] == "interrupt_start")
                print(json.dumps({"interruption_detection_ms": (cut - near_start) / 1000}), flush=True)
                uploads = [e["meta"]["mono_us"] for e in events if e["kind"] == "pcm_uplink"]
                if not any(t > cut for t in uploads):
                    raise RuntimeError("No microphone upload after interruption")
            print(json.dumps({"session": session, "mode": args.mode, "events": len(events),
                              "timeline": len(timeline), "output": str(args.output.resolve())}), flush=True)


if __name__ == "__main__":
    main()
