"""Reject a recovered call as a continuous-session success."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from phone_ai_validation import assess_session


class PhoneAiValidationTests(unittest.TestCase):
    def records(self):
        return [{"time": 10 + i, "line": line} for i, line in enumerate([
            "Cloud event type=session.created bytes=1",
            "PHONE_AI route=active cloud=ready epoch=3",
            "Cloud event type=conversation.item.input_audio_transcription.completed bytes=1",
            "Cloud event type=response.done bytes=1",
        ])]

    def test_complete_call_does_not_claim_acoustic_or_carrier_acceptance(self):
        result = assess_session(self.records(), 10, 20, 1)
        self.assertEqual(result["digital_session"], "PASS")
        self.assertEqual(result["acoustic_acceptance"], "NOT_VERIFIED")
        self.assertEqual(result["carrier_call"], "NOT_TESTED")

    def test_reconnected_session_fails_even_when_later_reply_completes(self):
        rows = self.records() + [
            {"time": 14, "line": "PHONE_AI worker_stopped reason=audio_path_failed"},
            {"time": 15, "line": "Cloud event type=session.created bytes=1"},
            {"time": 16, "line": "PHONE_AI route=active cloud=ready epoch=5"},
        ]
        self.assertEqual(assess_session(rows, 10, 20, 1)["digital_session"], "FAIL")

    def test_last_question_without_finished_reply_fails(self):
        self.assertEqual(assess_session(self.records()[:-1], 10, 20, 1)
                         ["digital_session"], "FAIL")

    def test_old_failure_and_expected_hangup_are_outside_call_window(self):
        rows = self.records() + [
            {"time": 9, "line": "control receive failed expected_type=6"},
            {"time": 20, "line": "PHONE_AI worker_stopped reason=call_ended"},
        ]
        self.assertEqual(assess_session(rows, 10, 20, 1)["digital_session"], "PASS")

    def test_control_loss_and_upload_failure_fail(self):
        for line in ["control closed role=s3", "PHONE_AI failure=media_changed",
                     "PHONE_AI cloud_uplink_packets=250 failed=1 epoch=3"]:
            with self.subTest(line=line):
                rows = self.records() + [{"time": 14, "line": line}]
                self.assertEqual(assess_session(rows, 10, 20, 1)["digital_session"], "FAIL")

    def test_heap_corruption_and_reboot_after_hangup_fail_lifecycle(self):
        for line in ["CORRUPT HEAP: Bad tail", "ESP-ROM:esp32s3-20210327"]:
            with self.subTest(line=line):
                rows = self.records() + [{"time": 21, "line": line}]
                self.assertEqual(assess_session(rows, 10, 20, 1, 30)
                                 ["digital_session"], "FAIL")

    def test_expected_worker_cleanup_after_hangup_is_allowed(self):
        rows = self.records() + [
            {"time": 21, "line": "PHONE_AI worker_stopped reason=call_ended"},
        ]
        self.assertEqual(assess_session(rows, 10, 20, 1, 30)["digital_session"], "PASS")

    def test_gateway_crash_fails_even_when_s3_session_completes(self):
        peer = [{"time": 17, "line": "Guru Meditation Error: StoreProhibited"}]
        result = assess_session(self.records(), 10, 20, 1, 30, peer)
        self.assertEqual(result["digital_session"], "FAIL")
        self.assertTrue(result["errors"][0].startswith("gateway: "))

    def test_gateway_old_boot_and_crash_after_cleanup_are_outside_window(self):
        peer = [{"time": 9, "line": "rst:0xc"},
                {"time": 31, "line": "Guru Meditation Error: StoreProhibited"}]
        self.assertEqual(assess_session(self.records(), 10, 20, 1, 30, peer)
                         ["digital_session"], "PASS")
