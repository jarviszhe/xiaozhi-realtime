"""Download finalized SD evidence through the device's explicitly enabled debug API.

Uses one persistent connection and bounded reads. Never formats or mutates the
card; audio/telephone transcripts belong in ignored artifacts, not in commits.
"""
import argparse
import array
import hashlib
import http.client
import json
import math
from pathlib import Path
import re
import sys
import wave


def wav_metrics(path):
    squares = count = peak = 0
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, 16000):
            raise ValueError("unexpected recording PCM format")
        while data := source.readframes(16000):
            samples = array.array("h", data)
            if sys.byteorder != "little":
                samples.byteswap()
            count += len(samples)
            squares += sum(x * x for x in samples)
            peak = max(peak, max(map(abs, samples), default=0))
        declared = source.getnframes()
    if count != declared or path.stat().st_size != 44 + count * 2:
        raise ValueError("recording length/header mismatch")
    return {"samples": count, "seconds": count / 16000,
            "rms": math.sqrt(squares / count) if count else 0, "peak": peak}


def inspect(host, record_id, output):
    if not re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{8}", record_id):
        raise ValueError("invalid recording ID")
    output.mkdir(parents=True, exist_ok=False)
    connection = http.client.HTTPConnection(host, timeout=15)
    report = {"id": record_id, "files": {}, "errors": [],
              "scope": "SD files; packet delivery, acoustic and carrier acceptance are separate"}
    try:
        for name in ("meta.json", "live-transcript.jsonl", "transcript.jsonl",
                     "caller.wav", "assistant.wav"):
            temporary = output / (name + ".part")
            offset = 0
            digest = hashlib.sha256()
            total = None
            try:
                with temporary.open("wb") as target:
                    while total is None or offset < total:
                        connection.request("GET", "/api/test/record-file?id=" + record_id +
                                           "&file=" + name + "&offset=" + str(offset))
                        response = connection.getresponse()
                        # One response is bounded by the firmware to 8192 bytes.
                        data = response.read(8193)
                        if response.status != 200 or len(data) > 8192:
                            raise ValueError("download status=" + str(response.status))
                        size = int(response.getheader("X-File-Size"))
                        if size < 0 or size > 2 ** 32 or (total is not None and total != size):
                            raise ValueError("file size changed or exceeds bound")
                        total = size
                        if offset + len(data) > total or (offset < total and not data):
                            raise ValueError("invalid/short download")
                        target.write(data)
                        digest.update(data)
                        offset += len(data)
                path = output / name
                temporary.replace(path)
                info = {"bytes": offset, "sha256": digest.hexdigest()}
                if name.endswith(".wav"):
                    info.update(wav_metrics(path))
                elif name == "meta.json":
                    report["metadata"] = json.loads(path.read_text(encoding="utf8"))
                else:
                    rows = [json.loads(line) for line in path.read_text(encoding="utf8").splitlines()]
                    info["sentences"] = len(rows)
                    info["speakers"] = sorted({row.get("speaker", "") for row in rows})
                report["files"][name] = info
                print(name, offset, flush=True)
            except Exception as error:
                # Avoid printing response bodies, headers or configuration secrets.
                report["errors"].append({"file": name, "error": str(error)})
                print(name, "FAILED", type(error).__name__, flush=True)
                connection.close()
                connection = http.client.HTTPConnection(host, timeout=15)
    finally:
        connection.close()
        (output / "inspection.json").write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                                encoding="utf8")
    return bool(report["errors"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Device LAN address, optionally host:port")
    parser.add_argument("--id", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    raise SystemExit(inspect(args.host, args.id, args.output))
