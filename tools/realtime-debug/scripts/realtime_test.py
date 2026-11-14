"""Run board Realtime checks using local WAV speech and the existing trace collector.

Requires Windows speaker playback, enabled device debug capture, and a connected
board. Starts interruption only after the collector observes actual playback.
"""

import argparse
import json
import re
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


def speech_audio_time(events, audio_start_ms):
    """Map a server sample position to local submission time, when captured."""
    if audio_start_ms is None:
        return None
    offset_ms = 0
    for event in events:
        if event['kind'] != 'pcm_uplink':
            continue
        meta = event['meta']
        duration_ms = event.get('audio_length', 0) * 1000 / (
            meta['sample_rate'] * meta['channels'] * meta['bits_per_sample'] / 8)
        if offset_ms <= audio_start_ms < offset_ms + duration_ms:
            return int(meta['mono_us']) - int((offset_ms + duration_ms - audio_start_ms) * 1000)
        offset_ms += duration_ms
    return None


def evaluate(info, events, timeline, markers, *, interruption=False, expected_input=None,
             expected_reply=None, max_detection_ms=1500, far_only=False):
    """Evaluate saved evidence; speech-start counts alone are never acceptance."""
    failures, metrics = [], {}
    summary = info['summary']
    if any(summary.get(k) for k in ('live', 'gaps', 'truncated', 'interrupted')):
        failures.append('incomplete_trace')
    by_name = {m['type']: m for m in markers}
    if 'observation_end' in by_name:
        last_time = max((int(e['meta']['mono_us']) for e in events), default=0)
        if last_time < by_name['observation_end']['device_time_us_estimate'] - 500000:
            failures.append('session_closed_before_observation_end')
    playback = [e for e in events if e['kind'] == 'pcm_playback']
    if not playback:
        failures.append('no_device_playback')
    clouds = [(t['time_us'], t['cloud']) for t in timeline if 'cloud' in t]
    if not interruption:
        if far_only and playback:
            first_play = min(int(e['meta']['mono_us']) for e in playback)
            if any(t > first_play and c.get('type') == 'input_audio_buffer.speech_started'
                   for t, c in clouds):
                failures.append('speech_trigger_during_far_only')
        return {'passed': not failures, 'failures': failures, 'metrics': metrics}
    if not all(k in by_name for k in ('interrupt_start', 'interrupt_end')):
        return {'passed': False, 'failures': failures + ['missing_near_markers'], 'metrics': metrics}
    start = by_name['interrupt_start']['device_time_us_estimate']
    end = by_name['interrupt_end']['device_time_us_estimate']
    uncertainty = max(by_name[k].get('clock_uncertainty_us', 0)
                      for k in ('interrupt_start', 'interrupt_end'))
    first_play = min((int(e['meta']['mono_us']) for e in playback), default=start)
    speech = [(t, c) for t, c in clouds
              if c.get('type') == 'input_audio_buffer.speech_started' and t > first_play]
    if any(t < start - uncertainty for t, _ in speech):
        failures.append('speech_trigger_before_near')
    if any(first_play < t['time_us'] < start - uncertainty and
           t.get('type') in ('playback_reset', 'local_vad_interrupt') for t in timeline):
        failures.append('playback_cancel_before_near')
    near_events = [(t, c) for t, c in speech if t >= start - uncertainty]
    if not near_events:
        failures.append('near_not_detected')
    else:
        detected = near_events[0][0]
        metrics['speech_event_delay_ms'] = (detected - start) / 1000
        if detected > start + max_detection_ms * 1000 + uncertainty:
            failures.append('speech_detection_too_late')
        if not any(e['kind'] == 'pcm_uplink' and int(e['meta']['mono_us']) > detected
                   for e in events):
            failures.append('no_upload_after_interruption')
        # audio_start_ms is a server input-sample position, not receipt time.
        # Map it through locally submitted PCM to expose buffered event delivery.
        mapped = speech_audio_time(events, near_events[0][1].get('audio_start_ms'))
        if mapped is not None:
            metrics['server_audio_start_device_us_estimate'] = mapped
            metrics['speech_event_after_audio_ms'] = (detected - mapped) / 1000
            # Allow the server's 300 ms prefix and timestamp uncertainty.
            if mapped < start - 600000 - uncertainty:
                failures.append('server_speech_began_before_near')
    # Several speech starts can belong to one real utterance. Match their items
    # against the transcript instead of asserting exactly two starts per test.
    users = [(t, c) for t, c in clouds if t >= start - uncertainty and
             c.get('type') == 'conversation.item.input_audio_transcription.completed']
    user_text = ''.join(c.get('transcript', '') for _, c in users)
    metrics['near_transcript'] = user_text
    if not expected_input or not expected_reply:
        failures.append('missing_expected_content')
    if expected_input and not re.search(expected_input, user_text):
        failures.append('near_transcript_mismatch')
    replies = [(t, c) for t, c in clouds if t >= start and
               c.get('type') == 'response.audio_transcript.done']
    matched_replies = [(t, c) for t, c in replies
                       if expected_reply and re.search(expected_reply, c.get('transcript', ''))]
    if not matched_replies:
        failures.append('expected_reply_missing')
    else:
        reply_time, reply = matched_replies[0]
        metrics['reply_transcript'] = reply.get('transcript', '')
        # A new speech turn after the intended answer with no new PC speech is
        # not a legitimate split of the interruption.
        if any(t > max(reply_time, end + uncertainty) for t, _ in speech):
            failures.append('speech_trigger_after_answer')
        completed = [t for t, c in clouds if c.get('type') == 'response.done' and
                     c.get('response', {}).get('id') == reply.get('response_id') and
                     c.get('response', {}).get('status') == 'completed']
        if not completed:
            failures.append('answer_not_completed')
        last_time = max((int(e['meta']['mono_us']) for e in events), default=0)
        metrics['post_answer_observation_ms'] = (last_time - max(reply_time, end)) / 1000
        if metrics['post_answer_observation_ms'] < 5000:
            failures.append('insufficient_post_answer_observation')
    # A silent PC after end+tail must not create unrelated user transcripts,
    # even when the model happens to produce the expected answer as well.
    # Delayed delivery of the real near utterance is a latency failure, not
    # evidence of a separate acoustic turn after the PC has stopped speaking.
    speech_times = {}
    for t, c in speech:
        mapped = speech_audio_time(events, c.get('audio_start_ms'))
        speech_times[c.get('item_id')] = mapped if mapped is not None else t
    for _, c in users:
        if speech_times.get(c.get('item_id'), 0) > end + 1500000 + uncertainty:
            failures.append('new_user_turn_after_near_tail')
            break
    metrics['near_speech_events'] = len(near_events)
    return {'passed': not failures, 'failures': list(dict.fromkeys(failures)), 'metrics': metrics}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="http://192.168.5.15")
    parser.add_argument("--collector", default="http://127.0.0.1:7320")
    parser.add_argument("--mode", choices=("manual", "auto", "realtime"), default="realtime")
    parser.add_argument("--question", type=Path, required=True)
    parser.add_argument("--interrupt", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--interrupt-delay", type=float, default=1)
    parser.add_argument("--observe-seconds", type=float, default=20)
    parser.add_argument("--expect-input", help="Regex required to accept an interruption transcript")
    parser.add_argument("--expect-reply", help="Regex required to accept the new answer")
    parser.add_argument("--max-detection-ms", type=float, default=1500)
    parser.add_argument("--aec-policy", choices=('playback', 'continuous'))
    parser.add_argument("--aec-tracks", action='store_true', help='Record M/R/output as well as conversation tracks')
    parser.add_argument("--input-gain", type=int, choices=(18, 24, 30), help='Temporary codec PGA gain in dB')
    parser.add_argument("--output-volume", type=int, help='Speaker volume 1..100; restored after the test')
    parser.add_argument("--far-only", action='store_true', help='Observe device speech without PC interruption')
    args = parser.parse_args()
    if args.interrupt and args.mode != "realtime":
        parser.error("Interruption scenario requires realtime mode")
    if args.far_only and (args.mode != 'realtime' or args.interrupt):
        parser.error('far-only requires realtime without --interrupt')
    if args.output_volume is not None and not 1 <= args.output_volume <= 100:
        parser.error('output-volume must be 1..100')
    args.output.mkdir(parents=True, exist_ok=True)
    original_volume = None
    resources = []
    markers = []
    session = None
    errors = []
    result = {'passed': False, 'failures': [], 'errors': errors}

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
        action(args.device, "stop")
        wait_for("idle", lambda: get_json(args.device + "/api/test/stats")["state"] == "idle")
        select_mode(args.device, args.mode)
        action(args.device, 'trace-aec' if args.aec_tracks else 'trace-basic')
        if args.input_gain is not None:
            action(args.device, 'gain-' + str(args.input_gain))
            wait_for('Input gain applied', lambda: get_json(
                args.device + '/api/test/stats').get('input_gain_db') == args.input_gain)
        if args.aec_policy:
            action(args.device, 'aec-' + args.aec_policy)
            wait_for('AEC test policy applied', lambda: get_json(
                args.device + '/api/test/stats').get('aec_policy') == args.aec_policy)
        if args.output_volume is not None:
            original_volume = get_json(args.device + '/api/test/stats')['output_volume']
            action(args.device, 'volume-' + str(args.output_volume))
            wait_for('Output volume applied', lambda: get_json(
                args.device + '/api/test/stats').get('output_volume') == args.output_volume)
        known = {s["id"] for s in get_json(args.collector + "/api/sessions")}
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
            time.sleep(args.observe_seconds)
        elif args.far_only:
            time.sleep(args.observe_seconds)
        else:
            wait_for("reply completed", lambda: get_json(args.device + "/api/test/stats")["state"]
                     == ("idle" if args.mode == "manual" else "listening"), timeout=90)
        resources.append(get_json(args.device + "/api/test/stats"))
        markers.append({'type': 'observation_end',
                        'device_time_us_estimate': resources[-1]['time_us']})
    except Exception as exc:
        errors.append({'stage': 'run', 'type': type(exc).__name__, 'message': str(exc)})
    finally:
        try:
            action(args.device, "stop")
            if session:
                wait_for("trace sender drained and session closed", lambda: next((s for s in get_json(
                    args.collector + "/api/sessions") if s["id"] == session and not s["live"]), None), timeout=30)
        except Exception as exc:
            errors.append({'stage': 'close', 'type': type(exc).__name__, 'message': str(exc)})
        if original_volume is not None:
            try:
                wait_for('idle before volume restore', lambda: get_json(
                    args.device + '/api/test/stats')['state'] == 'idle')
                action(args.device, 'volume-' + str(original_volume))
                wait_for('Original volume restored', lambda: get_json(
                    args.device + '/api/test/stats').get('output_volume') == original_volume)
            except Exception as exc:
                errors.append({'stage': 'restore', 'type': type(exc).__name__, 'message': str(exc)})
        (args.output / "resources.json").write_text(json.dumps(resources, indent=2), encoding="utf-8")
        (args.output / "markers.json").write_text(json.dumps(markers, indent=2), encoding="utf-8")
        try:
            if session:
                info, events, timeline = collect(args.collector, session, args.output)
                result.update(evaluate(info, events, timeline, markers, interruption=bool(args.interrupt),
                                       expected_input=args.expect_input, expected_reply=args.expect_reply,
                                       max_detection_ms=args.max_detection_ms, far_only=args.far_only))
            else:
                result['failures'].append('no_trace_session')
        except Exception as exc:
            errors.append({'stage': 'export', 'type': type(exc).__name__, 'message': str(exc)})
        result.update(session=session, mode=args.mode, aec_policy=args.aec_policy, input_gain_db=args.input_gain,
                      output_volume=args.output_volume, original_volume=original_volume,
                      expected_input=args.expect_input, expected_reply=args.expect_reply,
                      max_detection_ms=args.max_detection_ms,
                      output=str(args.output.resolve()))
        result['passed'] = result['passed'] and not errors
        (args.output / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False), flush=True)
    if not result['passed']:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
