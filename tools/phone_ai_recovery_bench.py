"""Interrupt a real Android/HFP phone AI call without clearing stored settings.

Requires phone_ai_monitor.py. Wi-Fi is restored in finally; a new call must
complete on the same boot. This is fault recovery, not normal audio acceptance.
"""
import argparse
import json
from pathlib import Path
import time
from types import SimpleNamespace

import phone_ai_bench as bench
from phone_ai_validation import read_records


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    events = []
    began = time.time()
    error = None
    original_auto = None
    owns_call = False
    wifi_disabled = False

    def event(kind, **values):
        row = dict(time=time.time(), kind=kind, **values)
        events.append(row)
        with (args.output / "events.jsonl").open("a", encoding="utf8") as stream:
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")
        print(json.dumps(row, ensure_ascii=False), flush=True)

    def wait_ui(label, predicate, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ui = bench.last_ui()
            if predicate(ui):
                event(label, ui=ui)
                return ui
            time.sleep(1)
        raise TimeoutError(label + ": " + ui)

    try:
        bench.require_android()
        phone = bench.phone_status()
        if not phone.get("fresh") or phone.get("phase") not in (0, 4):
            raise RuntimeError("another call is active or status is stale")
        original_auto = bool(phone["flags"] & 1)
        bench.set_auto_answer(True, 8)
        owns_call = True
        bench.action("calltest", "incoming")
        event("incoming")
        wait_ui("listening", lambda ui: all(token in ui for token in
                (" ai=3 ", " ready=1 ", " fresh=1 ", " hfp=3 ")), 35)
        bench.action("hfptest", "start", "--ez", "call_media", "true",
                     "--ei", "seconds", "120", "--ei", "rate", "16000",
                     "--ei", "frequency", "0", "--es", "session", args.output.name,
                     "--ei", "volume_percent", "80")
        time.sleep(3)
        ui = bench.last_ui()
        if any(token not in ui for token in (" ai=3 ", " ready=1 ", " fresh=1 ", " hfp=3 ")):
            raise RuntimeError("call failed before fault injection: " + ui)
        unexpected = [row for row in read_records(args.log) if row["time"] >= began and any(
            marker in row["line"] for marker in ("control closed", "control receive failed",
            "PHONE_AI worker_stopped", "PHONE_AI failure=", "PHONE_AI state=failed"))]
        if unexpected:
            event("unexpected_before_injection", records=unexpected)
            raise RuntimeError("uncontrolled call failure before fault injection")
        event("healthy_before_injection", ui=ui)
        event("inject_gateway_wifi_off")
        wifi_disabled = True
        bench.command("wifi off", "COM5")
        wait_ui("media_stopped", lambda ui: " ai=6 " in ui and any(
                token in ui.split() for token in ("ai_error=gateway_control",
                "ai_error=gateway_media_timeout")), 25)
        bench.screenshot(args.log, args.output / "network-failure.png")
        # Leave the real failure visible and save the phone's independent state.
        for service in ("telecom", "audio"):
            (args.output / ("fault-android-" + service + ".txt")).write_text(
                bench.adb("shell", "dumpsys", service), encoding="utf8")
        bench.action("calltest", "end")
        bench.action("hfptest", "stop")
        event("ended_call_while_offline")
        # Keep the runtime policy enabled: this proves the classic gateway
        # blocks ATA when its bound assistant is unreachable, rather than
        # merely relying on an AUTO_OFF command that cannot cross the outage.
        blocked_since = time.time()
        blocked_at = time.monotonic()
        bench.action("calltest", "incoming")
        event("incoming_while_gateway_offline")
        incoming_seen = False
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            phone = bench.phone_status("COM5")
            if phone["phase"] == 3 or phone["call_active"]:
                raise RuntimeError("unavailable assistant call was automatically answered")
            incoming_seen = incoming_seen or phone["phase"] == 1
            if incoming_seen and time.monotonic() - blocked_at >= 6:
                if phone["phase"] != 1 or not phone["flags"] & 1 or phone["flags"] & 2:
                    raise RuntimeError("offline incoming call/policy/countdown state not proven")
                break
            time.sleep(.5)
        else:
            raise TimeoutError("incoming call while assistant unavailable was not observed")
        suppressed = [row for row in read_records(args.log.with_name("serial-COM5.jsonl"))
                      if row["time"] >= blocked_since and
                      "AUTO_ANSWER suppressed reason=assistant_link_unavailable" in row["line"] and
                      f"call_id={phone['call_id']}" in row["line"].split()]
        if not suppressed:
            raise RuntimeError("missing classic gateway automatic-answer suppression evidence")
        event("offline_auto_answer_suppressed", phone=phone, records=suppressed)
        bench.action("calltest", "end")
        event("ended_blocked_incoming_call")
        bench.command("wifi on", "COM5")
        wifi_disabled = False
        event("restore_gateway_wifi")
        wait_ui("ready_after_reconnect", lambda ui: all(s in ui for s in
                (" ready=1 ", " fresh=1 ", " hfp=3 ", " ai=1 ")) and
                "ai_error=none" in ui.split(), 75)
        deadline = time.monotonic() + 8
        while True:
            phone = bench.phone_status()
            if phone.get("fresh") and phone["phase"] in (0, 4) and phone["flags"] & 1:
                break
            if time.monotonic() > deadline:
                raise RuntimeError("phone not idle/ended or automatic answer not restored")
            time.sleep(.3)
        event("auto_answer_recovered", call_id=phone["call_id"])
        # No board reset between the injected fault and the following call.
        bench.run(SimpleNamespace(output=args.output / (args.output.name + "-next-call"), log=args.log,
                                  seconds=40, screenshot=False))
        event("next_call_completed")
    except (Exception, SystemExit) as exc:
        error = str(exc)
        event("failure", error=error)
    finally:
        for name, restore in (
            ("wifi_on", lambda: bench.command("wifi on", "COM5") if wifi_disabled else None),
            ("end_call", lambda: bench.action("calltest", "end") if owns_call else None),
            ("stop_media", lambda: bench.action("hfptest", "stop") if owns_call else None),
        ):
            try:
                restore()
            except Exception as exc:
                error = error or str(exc)
                event("restore_failed", action=name, error=str(exc))
        if original_auto is not None:
            try:
                bench.set_auto_answer(original_auto, 8)
            except Exception as exc:
                error = error or str(exc)
                event("restore_failed", action="automatic_answer", error=str(exc))
        faults = []
        for port in ("COM3", "COM5"):
            for row in read_records(args.log.with_name("serial-" + port + ".jsonl")):
                if row["time"] >= began and any(marker in row["line"] for marker in (
                        "Guru Meditation", "panic", "assert failed", "CORRUPT HEAP",
                        "ESP-ROM:", "rst:0x", "Malloc failed", "CODEC_GUARD failed")):
                    faults.append(dict(port=port, **row))
        result = dict(scope="Android_HFP_gateway_network_fault_recovery_and_offline_incoming_same_boot",
                      result="FAIL" if error or faults else "PASS", error=error,
                      faults=faults, start=began, end=time.time(), events=events,
                      carrier_call="NOT_TESTED", acoustic_acceptance="NOT_VERIFIED")
        (args.output / "result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf8")
        print(json.dumps({k: v for k, v in result.items() if k != "events"}), flush=True)
    return result["result"] != "PASS"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--adb", type=Path, required=True)
    parser.add_argument("--phone", required=True)
    parser.add_argument("--monitor", default="http://127.0.0.1:7330")
    args = parser.parse_args()
    if not args.log.is_file() or not args.log.with_name("serial-COM5.jsonl").is_file():
        parser.error("existing COM3 and COM5 monitor logs are required")
    bench.ADB, bench.SERIAL, bench.MONITOR = args.adb, args.phone, args.monitor
    raise SystemExit(run(args))
