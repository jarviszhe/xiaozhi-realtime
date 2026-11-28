"""Real two-board settings acceptance; simulates BOOT only on debug firmware.

Clears the classic gateway Wi-Fi with the existing long-press handler and
restores it through the real screen callbacks. Preserves retained binding and
the S3 credentials. No synthetic success/peer injection. No audio acceptance.
"""
import argparse
import json
from pathlib import Path
import re
import time

from phone_ui_capture import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout-test", action="store_true")
    parser.add_argument("--navigation-only", action="store_true",
                        help="Check repeated real UI navigation and empty scan without clearing Wi-Fi")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    s3, gateway = Console("COM3", args.output), Console("COM5", args.output)
    states, lines, checks = {}, [], []
    report = {"scope": "real_LVGL_and_BLE_LAN_provisioning_serial_button_simulation", "checks": checks,
              "rejected_status_frames": {"s3": 0, "gateway": 0}}
    last_poll = 0

    def check(name, passed, detail=""):
        checks.append({"check": name, "passed": bool(passed), "detail": detail, "at": time.time()})
        (args.output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf8")
        print(f"{'PASS' if passed else 'FAIL'} {name} {detail}", flush=True)
        if not passed:
            raise RuntimeError(name)

    def pump(seconds):
        nonlocal last_poll
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if time.monotonic() - last_poll > 1:
                s3.send("ui status")
                gateway.send("gateway status")
                last_poll = time.monotonic()
            for name, console in (("s3", s3), ("gateway", gateway)):
                batch = [console.line()]
                while len(batch) < 512 and (b"\n" in console.pending or console.port.in_waiting):
                    batch.append(console.line())
                for line in batch:
                    if not line:
                        continue
                    lines.append((name, line))
                    if any(fault in line for fault in ("stack overflow", "Guru Meditation", "assert failed", "panic_abort", "Malloc failed", "failed to allocate memory")):
                        raise RuntimeError(f"{name}: {line}")
                    if line.startswith("UI_STATUS "):
                        states["ui"] = dict(re.findall(r"(\w+)=([^ ]*)", line))
                    if line.startswith("GATEWAY {"):
                        try:
                            states[name] = json.loads(line[8:])
                        except json.JSONDecodeError:
                            # Keep the raw evidence, reject the damaged UART line,
                            # and request a fresh frame; never repair/infer state.
                            states.pop(name, None)
                            report["rejected_status_frames"][name] += 1
                            print(f"STATUS_FRAME_REJECTED source={name}", flush=True)
                            if report["rejected_status_frames"][name] > 3:
                                raise RuntimeError(f"repeated corrupt status frames: {name}")
                            console.send("gateway status")
                    if any(tag in line for tag in ("phone_ui: action", "stage=", "GATEWAY_EVENT", "GATEWAY_FOUND", "UI_CLICK")) and not line.startswith("UI_STATUS"):
                        print(f"{name}: {line}", flush=True)

    def wait(name, predicate, seconds=25):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            pump(0.3)
            if predicate():
                check(name, True, json.dumps(states.get("ui", {}), ensure_ascii=False))
                return
        check(name, False, json.dumps(states, ensure_ascii=False))

    def ui(**values):
        return all(states.get("ui", {}).get(key) == str(value) for key, value in values.items())

    def command(text, delay=0.6):
        states.pop("ui", None)
        s3.send(text)
        pump(delay)

    def screenshot(name):
        s3.screenshot(args.output / name)
        pump(0.4)

    def discover():
        command("ui settings")
        command("ui click search")
        wait("scan_started", lambda: ui(page=2, scan=1), 5)
        wait("scan_finished", lambda: ui(page=2, scan=0, count=1), 15)

    def connect():
        command("ui click connect 0")
        wait("physical_confirmation_requested", lambda: ui(page=4, operation=1, stage="waiting_button"), 25)
        check("BLE_confirmation_is_not_ready", not states["ui"]["fresh"] == "1")

    try:
        command("ui settings")
        wait("baseline_authenticated", lambda: ui(page=1, ready=1, fresh=1), 25)
        screenshot("06-settings.png")
        if args.navigation_only:
            report["scope"] = "real_LVGL_navigation_empty_scan_and_modal_controls"
            command("ui click rules")
            wait("rules_dialog_open", lambda: ui(page=6))
            start = len(lines)
            command("ui click search")
            check("modal_blocks_underlying_search", ui(page=6) and any(
                "UI_CLICK rejected_hidden_or_disabled" in line for _, line in lines[start:]))
            command("ui click close")
            for _ in range(10):
                for action in ("rules", "close", "records", "close", "home"):
                    command("ui click " + action, 0.15)
                command("ui settings", 0.15)
            wait("repeated_navigation_preserves_authenticated_link", lambda: ui(page=1, ready=1, fresh=1))
            command("ui controls")
            controls = [dict(re.findall(r"(\w+)=([^ ]*)", line)) for _, line in lines
                        if line.startswith("UI_CONTROL ")]
            check("settings_buttons_44px_and_inside_display", controls and all(
                int(c["height"]) >= 44 and 0 <= int(c["x"]) and 0 <= int(c["y"])
                and int(c["x"]) + int(c["width"]) <= 480
                and int(c["y"]) + int(c["height"]) <= 320 for c in controls))
            command("ui click search")
            wait("empty_scan_started", lambda: ui(page=2, scan=1), 5)
            wait("empty_scan_finishes_truthfully", lambda: ui(page=2, scan=0, count=0), 15)
            screenshot("07-empty.png")
            command("ui click rescan")
            wait("empty_retry_started", lambda: ui(page=2, scan=1), 5)
            wait("empty_retry_finishes", lambda: ui(page=2, scan=0, count=0), 15)
            command("ui click cancel")
            wait("empty_scan_preserves_link", lambda: ui(page=1, ready=1, fresh=1))
            screenshot("06-after-navigation.png")
            check("LVGL_stack_has_margin", int(states["ui"]["lvgl_stack_free"]) >= 2048)
            report["result"] = "PASS"
            return
        command("ui click rules")
        wait("rules_unavailable_is_explicit", lambda: ui(page=6))
        screenshot("rules-unavailable.png")
        command("ui click close")
        command("ui click search", 0.5)
        screenshot("07-scanning.png")
        command("ui click cancel")
        wait("scan_cancel_keeps_original_link", lambda: ui(page=1, ready=1, fresh=1))

        gateway.send("gateway button long")
        wait("simulated_long_clears_wifi", lambda: states.get("gateway", {}).get("wifi") is False, 15)
        wait("stale_link_never_shows_connected", lambda: ui(fresh=0, ready=0), 15)
        discover()
        screenshot("07-found.png")
        connect()
        screenshot("08-waiting-button.png")
        command("ui click cancel")
        wait("button_wait_cancelled", lambda: ui(page=1, operation=0), 10)
        wait("BLE_closed_after_cancel", lambda: states.get("gateway", {}).get("ble") is False, 10)

        if args.timeout_test:
            discover()
            connect()
            wait("button_timeout_is_failure", lambda: ui(page=5, operation=0,
                 error="button_confirmation_timeout") and not ui(fresh=1), 100)
            screenshot("confirmation-timeout.png")

        discover()
        connect()
        gateway.send("gateway button short")
        wait("normal_provision_authenticated_success", lambda: ui(page=5, ready=1, fresh=1, operation=0), 65)
        check("simulated_short_handler_observed", any("simulated_short_confirmed" in line for _, line in lines))
        screenshot("provision-success.png")
        command("ui click close")
        screenshot("06-connected.png")

        gateway.send("gateway button long")
        wait("recovery_wifi_cleared", lambda: states.get("gateway", {}).get("wifi") is False, 15)
        wait("recovery_old_link_lost", lambda: ui(fresh=0, ready=0), 15)
        command("ui click search")
        wait("recovery_scan_started", lambda: ui(page=2, scan=1), 5)
        wait("recovery_scan_idle", lambda: ui(page=2, scan=0), 15)
        before = len(lines)
        command("ui click reprovision")
        wait("authenticated_reprovision_without_button", lambda: ui(page=5, ready=1, fresh=1, operation=0), 65)
        check("no_button_confirmation_in_authenticated_recovery", not any("stage=waiting_button" in line for _, line in lines[before:]))
        screenshot("reprovision-success.png")
        command("ui click close")
        command("ui controls")
        check("all_reported_button_targets_at_least_44px", any(line.startswith("UI_CONTROL") for _, line in lines) and all(
            int(re.search(r"height=(\d+)", line)[1]) >= 44
            for _, line in lines if line.startswith("UI_CONTROL")))
        report["result"] = "PASS"
    except Exception as error:
        report["result"], report["error"] = "FAIL", str(error)
        raise
    finally:
        report["final_states"] = states
        report["finished_at"] = time.time()
        (args.output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf8")
        s3.close()
        gateway.close()


if __name__ == "__main__":
    main()
