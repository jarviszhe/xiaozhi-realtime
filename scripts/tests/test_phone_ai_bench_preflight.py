"""Disconnected Android and stale status must not alter an unrelated call."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import phone_ai_bench as bench
import phone_ai_recovery_bench as recovery


class PhoneAiBenchPreflightTests(unittest.TestCase):
    def test_ui_status_waits_for_own_response_amid_screenshot_rows(self):
        with mock.patch.object(bench.time, "time", return_value=10), \
             mock.patch.object(bench.time, "sleep"), \
             mock.patch.object(bench, "command") as command, \
             mock.patch.object(bench, "api", side_effect=[
                 [{"time": 9, "line": "UI_STATUS stale"},
                  {"time": 11, "line": "UI_PIXELS 0 abc"}],
                 [{"time": 11, "line": "UI_STATUS fresh=1 "},
                  {"time": 12, "line": "UI_PIXELS 192 abc"}]]) as api:
            self.assertEqual(bench.last_ui(), "UI_STATUS fresh=1 ")
        command.assert_called_once_with("ui status")
        self.assertEqual(api.call_count, 2)

    def args(self, root):
        log = root / "serial-COM3.jsonl"
        log.write_text("", encoding="utf8")
        log.with_name("serial-COM5.jsonl").write_text("", encoding="utf8")
        return SimpleNamespace(output=root / "test", log=log, seconds=40, screenshot=False)

    def test_normal_bench_without_android_makes_no_device_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            with mock.patch.object(bench, "adb", side_effect=RuntimeError("device offline")), \
                 mock.patch.object(bench, "command") as command, \
                 mock.patch.object(bench, "action") as action, \
                 mock.patch.object(bench, "phone_status") as phone_status, \
                 contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit):
                    bench.run(args)
            command.assert_not_called()
            action.assert_not_called()
            phone_status.assert_not_called()
            result = json.loads((args.output / "result.json").read_text(encoding="utf8"))
            self.assertEqual(result["result"], "FAIL")
            self.assertIn("device offline", result["bench_error"])

    def test_recovery_bench_without_android_makes_no_device_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            with mock.patch.object(bench, "adb", side_effect=RuntimeError("device offline")), \
                 mock.patch.object(bench, "command") as command, \
                 mock.patch.object(bench, "action") as action, \
                 mock.patch.object(bench, "phone_status") as phone_status, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(recovery.run(args))
            command.assert_not_called()
            action.assert_not_called()
            phone_status.assert_not_called()
            result = json.loads((args.output / "result.json").read_text(encoding="utf8"))
            self.assertEqual(result["result"], "FAIL")

    def test_policy_restore_rejects_stale_matching_flags(self):
        with mock.patch.object(bench, "command") as command, \
             mock.patch.object(bench, "phone_status", side_effect=[
                 {"fresh": False, "flags": 0},
                 {"fresh": True, "flags": 1},
                 {"fresh": True, "flags": 0}]) as status, \
             mock.patch.object(bench.time, "sleep"):
            result = bench.set_auto_answer(False)
        self.assertTrue(result["fresh"])
        self.assertEqual(status.call_count, 3)
        command.assert_called_once_with("phone auto off")

    def test_existing_call_is_not_ended_or_reconfigured_by_either_bench(self):
        for runner in (bench.run, recovery.run):
            with self.subTest(runner=runner.__module__), tempfile.TemporaryDirectory() as directory:
                args = self.args(Path(directory))
                with mock.patch.object(bench, "adb", return_value="PHONE_AI_ADB_READY\n"), \
                     mock.patch.object(bench, "command") as command, \
                     mock.patch.object(bench, "action") as action, \
                     mock.patch.object(bench, "phone_status", return_value={"fresh": True, "phase": 2}), \
                     mock.patch.object(bench, "last_ui", return_value="UI_STATUS ready=1 fresh=1 hfp=3 ai=3 "), \
                     contextlib.redirect_stdout(io.StringIO()):
                    if runner is bench.run:
                        with self.assertRaises(SystemExit):
                            runner(args)
                    else:
                        self.assertTrue(runner(args))
                action.assert_not_called()
                self.assertFalse(any(call.args[0].startswith("phone auto ")
                                     for call in command.call_args_list))

    def test_policy_restore_times_out_when_only_stale_state_is_available(self):
        with mock.patch.object(bench, "command"), \
             mock.patch.object(bench, "phone_status", return_value={"fresh": False, "flags": 0}), \
             mock.patch.object(bench.time, "monotonic", side_effect=range(20)), \
             mock.patch.object(bench.time, "sleep"):
            with self.assertRaisesRegex(TimeoutError, "fresh phone status"):
                bench.set_auto_answer(False, 2)

    def test_gateway_status_reads_its_own_port_and_ignores_older_console_rows(self):
        with mock.patch.object(bench, "command", return_value={"time": 10}) as command, \
             mock.patch.object(bench, "api", return_value=[
                 {"time": 9, "line": 'PHONE {"phase":3}'},
                 {"time": 11, "line": 'PHONE {"phase":1}'}]) as api:
            self.assertEqual(bench.phone_status("COM5"), {"phase": 1})
        command.assert_called_once_with("phone status", "COM5")
        api.assert_called_once_with("tail", {"count": 60, "port": "COM5"})

    def test_failure_collection_runs_duration_but_cannot_report_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            args.seconds = 20
            args.collect_after_control_failure = True
            state = {"clock": 0, "ui": 0, "ended": False}

            def sleep(seconds):
                state["clock"] += seconds

            def command(text, port="COM3"):
                if text == "ui click hangup":
                    state["ended"] = True
                return {"time": state["clock"]}

            def ui():
                state["ui"] += 1
                if state["ended"]:
                    return "UI_STATUS ready=1 fresh=1 hfp=3 ai=1 "
                return ("UI_STATUS ready=1 fresh=1 hfp=3 ai=3 " if state["ui"] <= 3 else
                        "UI_STATUS ready=1 fresh=0 hfp=0 ai=3 ")

            with mock.patch.object(bench, "require_android"), \
                 mock.patch.object(bench, "adb", return_value=""), \
                 mock.patch.object(bench, "command", side_effect=command), \
                 mock.patch.object(bench, "action"), \
                 mock.patch.object(bench, "transcripts", return_value=[]), \
                 mock.patch.object(bench, "phone_status", return_value={"fresh": True, "phase": 4, "flags": 1}), \
                 mock.patch.object(bench, "last_ui", side_effect=ui), \
                 mock.patch.object(bench, "assess_session", return_value={"digital_session": "PASS"}), \
                 mock.patch.object(bench.time, "monotonic", side_effect=lambda: state["clock"]), \
                 mock.patch.object(bench.time, "sleep", side_effect=sleep), \
                 contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit):
                    bench.run(args)
            result = json.loads((args.output / "result.json").read_text(encoding="utf8"))
            self.assertEqual(result["result"], "FAIL")
            self.assertIn("control status unavailable", result["bench_error"])
            load = next(e for e in result["events"] if e["kind"] == "load_end")
            self.assertGreaterEqual(load["elapsed"], args.seconds)


if __name__ == "__main__":
    unittest.main()
