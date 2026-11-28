"""Evidence checks for one Android Telecom / HFP / Aliyun phone session.

Use host timestamps to isolate the call in an append-only serial log. These
checks establish digital session continuity, not acoustic or carrier acceptance.
"""

import json
import re
from pathlib import Path


def assess_session(records, start, hangup, turns, cleanup_end=None, gateway_records=()):
    lines = [r["line"] for r in records if start <= r["time"] < hangup]
    lifecycle = [r["line"] for r in records
                 if start <= r["time"] < (cleanup_end if cleanup_end is not None else hangup)]
    errors = []
    ready = [line for line in lines if "PHONE_AI route=active cloud=ready" in line]
    sessions = sum("Cloud event type=session.created " in line for line in lines)
    inputs = sum("type=conversation.item.input_audio_transcription.completed " in line
                 for line in lines)
    replies = sum("Cloud event type=response.done " in line for line in lines)
    if len(ready) != 1 or sessions != 1:
        errors.append(f"expected one cloud session, ready={len(ready)} created={sessions}")
    if turns < 1 or inputs < turns or replies < turns:
        errors.append(f"unfinished turns: sent={turns} transcribed={inputs} replies={replies}")
    for line in lines:
        if any(marker in line for marker in (
            "PHONE_AI worker_stopped", "control receive failed", "control closed",
            "PHONE_AI failure=", "panic", "assert failed", "Malloc failed",
            "SSL receive failed", "Failed to allocate memory",
        )):
            errors.append(line)
        failed = re.search(r"PHONE_AI cloud_uplink_packets=\d+ failed=(\d+)", line)
        if failed and int(failed[1]):
            errors.append(line)
    for line in lifecycle:
        if any(marker in line for marker in (
            "CORRUPT HEAP", "panic", "assert failed", "Guru Meditation", "ESP-ROM:",
            "Malloc failed", "Failed to allocate memory",
            "PcmRateConverter: Conversion failed", "Input resampler recreate failed",
        )) and line not in errors:
            errors.append(line)
    # The gateway may reboot and reconnect between S3 polling samples. Its
    # independent log is required evidence, even when the S3 session recovers.
    for record in gateway_records:
        if not start <= record["time"] < (cleanup_end if cleanup_end is not None else hangup):
            continue
        line = record["line"]
        if any(marker in line for marker in (
            "Guru Meditation", "panic", "assert failed", "CORRUPT HEAP",
            "ESP-ROM:", "rst:0x", "Malloc failed", "Failed to allocate memory",
            "CODEC_GUARD failed",
        )):
            errors.append("gateway: " + line)
    return {"digital_session": "FAIL" if errors else "PASS", "errors": errors,
            "cloud_sessions": sessions, "ready_count": len(ready), "turns_sent": turns,
            "inputs_completed": inputs, "replies_completed": replies,
            "acoustic_acceptance": "NOT_VERIFIED", "carrier_call": "NOT_TESTED"}


def read_records(path):
    return [json.loads(line) for line in Path(path).read_text(encoding="utf8").splitlines()
            if line.strip()]
