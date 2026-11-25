"""Repeated real-board startup/discovery checks; no speech acceptance."""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--s3-port', default='COM3')
parser.add_argument('--gateway-port', default='COM5')
parser.add_argument('--s3-resets', type=int, default=3, choices=range(1, 11))
parser.add_argument('--reprovision-test', action='store_true',
                    help='Simulate gateway long press, clear Wi-Fi, and recover using retained binding')
args = parser.parse_args()
out = args.out
out.mkdir(parents=True, exist_ok=False)
ports = {}
report = {"scope": "s3_mdns_psram_startup_and_retained_binding_control", "checks": []}
seen = {"s3": [], "gateway": []}
pending = {"s3": b"", "gateway": b""}
states = {}


def open_ports():
    for name, number in (("s3", args.s3_port), ("gateway", args.gateway_port)):
        s = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=2)
        s.dtr = False
        s.rts = False
        s.port = number
        s.open()
        ports[name] = s


def close_ports():
    for s in ports.values():
        s.close()
    ports.clear()
    for name in pending:
        pending[name] = b""


def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for name, s in ports.items():
            data = s.read(s.in_waiting or 1)
            if not data:
                continue
            with (out / (name + ".log")).open("ab") as log:
                log.write(data)
            pending[name] += data
            while b"\n" in pending[name]:
                raw, pending[name] = pending[name].split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                seen[name].append(line)
                if line.startswith("GATEWAY {"):
                    states[name] = json.loads(line[8:])
                if any(x in line for x in ("assert failed", "panic", "task_wdt:", "Malloc failed",
                                           "failed to allocate memory", "Cannot allocate memory",
                                           "handler already registered", "mDNS startup failed")):
                    raise RuntimeError(name + ": " + line)
                if "mDNS" in line or "authenticated control ready" in line:
                    print(name + ": " + line, flush=True)


def send(name, command):
    with (out / "commands.jsonl").open("a", encoding="utf-8") as log:
        log.write(json.dumps({"at": time.time(), "device": name, "command": command}) + "\n")
    ports[name].write((command + "\n").encode("ascii"))


def snapshot():
    states.clear()
    for name in ports:
        send(name, "gateway status")
    pump(1)
    return dict(states)


def ready(label):
    end = time.monotonic() + 70
    while time.monotonic() < end:
        state = snapshot()
        if len(state) == 2 and all(d["ready"] and d["remote_fresh"]
                                  and d["stage"] == "ready" for d in state.values()):
            if not state["s3"].get("mdns") or state["s3"].get("mdns_failures") != 0:
                raise RuntimeError("mDNS status not healthy: " + label)
            report["checks"].append({"name": label, "result": "PASS", "status": state})
            print("PASS " + label + " internal_free=" + str(state["s3"]["internal_free"])
                  + " largest=" + str(state["s3"]["internal_largest"]), flush=True)
            return state
        pump(1)
    raise RuntimeError("ready timed out: " + label)


try:
    open_ports()
    ready("initial_boot")
    for index, name in enumerate(["s3"] * args.s3_resets + ["gateway"]):
        marker = len(seen["s3"])
        close_ports()
        reset_started = time.monotonic()
        reset = subprocess.run([sys.executable, "-m", "esptool", "--port",
                                args.s3_port if name == "s3" else args.gateway_port, "run"],
                               capture_output=True, timeout=30)
        (out / f"{index}-{name}-reset.log").write_bytes(reset.stdout + reset.stderr)
        if reset.returncode:
            raise RuntimeError("reset failed: " + name)
        open_ports()
        state = ready(f"{index}_{name}_warm_reset")
        elapsed_ms = (time.monotonic() - reset_started) * 1000
        if state[name]["uptime_ms"] > elapsed_ms + 3000:
            raise RuntimeError("reset not proven: " + name)
        if name == "s3" and not any("mDNS task will be created from SPIRAM" in line
                                     for line in seen["s3"][marker:]):
            raise RuntimeError("PSRAM task allocation log missing after S3 reset")
    if args.reprovision_test:
        send("gateway", "gateway button long")
        pump(3)
        state = snapshot()
        if state["gateway"]["wifi"] or not state["gateway"]["bound"]:
            raise RuntimeError("gateway Wi-Fi clear not proven")
        send("s3", "gateway reprovision")
        ready("ble_reprovision_with_mdns_psram")
    previous = snapshot()
    for index in range(6):
        pump(10)
        state = ready(f"idle_heartbeat_{index}")
        if any(state[name]["uptime_ms"] <= previous[name]["uptime_ms"] for name in ports):
            raise RuntimeError("unexpected reboot during idle observation")
        previous = state
    report["result"] = "PASS"
except Exception as exc:
    report["result"] = "FAIL"
    report["error"] = str(exc)
    print("FAIL " + str(exc), flush=True)
finally:
    close_ports()
    (out / "summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
sys.exit(report["result"] != "PASS")
