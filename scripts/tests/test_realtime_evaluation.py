"""Regression cases for accepting acoustic interruptions from trace evidence."""

import copy
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "realtime_test", ROOT / "tools/realtime-debug/scripts/realtime_test.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class RealtimeEvaluationTests(unittest.TestCase):
    def setUp(self):
        self.info = {"summary": {"live": False, "gaps": 0, "truncated": False, "interrupted": False}}
        self.markers = [
            {"type": "interrupt_start", "device_time_us_estimate": 3_000_000},
            {"type": "interrupt_end", "device_time_us_estimate": 6_000_000},
        ]
        self.events = [
            {"kind": "pcm_playback", "meta": {"mono_us": 2_000_000}},
            {"kind": "pcm_uplink", "meta": {"mono_us": 4_000_000}},
            {"kind": "session_close", "meta": {"mono_us": 14_000_000}},
        ]
        self.timeline = [
            self.cloud(3_400_000, "input_audio_buffer.speech_started", item_id="near"),
            self.cloud(6_500_000, "conversation.item.input_audio_transcription.completed",
                       item_id="near", transcript="stop the story and answer three plus five"),
            self.cloud(7_000_000, "response.audio_transcript.done", response_id="answer", transcript="8"),
            self.cloud(7_100_000, "response.done", response={"id": "answer", "status": "completed"}),
        ]

    @staticmethod
    def cloud(time_us, event_type, **fields):
        return {"time_us": time_us, "cloud": {"type": event_type, **fields}}

    def evaluate(self):
        return RUNNER.evaluate(self.info, self.events, self.timeline, self.markers,
                               interruption=True, expected_input="three.*five", expected_reply="8")

    def test_split_near_speech_is_not_rejected_by_event_count(self):
        self.timeline.insert(1, self.cloud(4_000_000, "input_audio_buffer.speech_started", item_id="near2"))
        self.assertTrue(self.evaluate()["passed"])

    def test_stop_before_user_speech_fails_even_with_correct_answer(self):
        self.timeline.insert(0, {"time_us": 2_800_000, "type": "playback_reset", "detail": "keep_tail"})
        result = self.evaluate()
        self.assertFalse(result["passed"])
        self.assertIn("playback_cancel_before_near", result["failures"])

    def test_self_conversation_after_answer_fails(self):
        self.timeline.extend([
            self.cloud(8_000_000, "input_audio_buffer.speech_started", item_id="echo"),
            self.cloud(9_000_000, "conversation.item.input_audio_transcription.completed",
                       item_id="echo", transcript="three plus five is eight"),
            self.cloud(10_000_000, "response.audio_transcript.done", response_id="echo-answer", transcript="8"),
        ])
        result = self.evaluate()
        self.assertFalse(result["passed"])
        self.assertIn("speech_trigger_after_answer", result["failures"])

    def test_delayed_event_is_not_timely_interruption(self):
        self.timeline[0]["time_us"] = 6_000_000
        self.assertIn("speech_detection_too_late", self.evaluate()["failures"])

    def test_correct_answer_without_correct_near_transcript_fails(self):
        self.timeline[1]["cloud"]["transcript"] = "once upon a time in a forest"
        self.assertIn("near_transcript_mismatch", self.evaluate()["failures"])

    def test_incomplete_trace_cannot_pass(self):
        self.info["summary"]["live"] = True
        self.assertIn("incomplete_trace", self.evaluate()["failures"])

    def test_closed_session_cannot_replace_silent_observation(self):
        self.markers.append({'type': 'observation_end', 'device_time_us_estimate': 20_000_000})
        self.assertIn('session_closed_before_observation_end', self.evaluate()['failures'])

    def test_late_echo_event_does_not_count_as_near_speech(self):
        self.timeline[0]['cloud']['audio_start_ms'] = 100
        self.events.insert(0, {'kind': 'pcm_uplink', 'audio_length': 32000,
                               'meta': {'mono_us': 3_000_000, 'sample_rate': 16000,
                                        'channels': 1, 'bits_per_sample': 16}})
        self.assertIn('server_speech_began_before_near', self.evaluate()['failures'])

    def test_answer_cancelled_before_completion_fails(self):
        self.timeline[3]["cloud"]["response"]["status"] = "cancelled"
        self.assertIn("answer_not_completed", self.evaluate()["failures"])

    def test_delayed_near_event_is_latency_failure_not_a_new_echo_turn(self):
        self.timeline[0]['time_us'] = 7_800_000
        self.timeline[0]['cloud']['audio_start_ms'] = 2000
        self.timeline[1]['time_us'] = 8_000_000
        self.timeline[2]['time_us'] = 8_100_000
        self.timeline[3]['time_us'] = 8_200_000
        self.events.insert(0, {'kind': 'pcm_uplink', 'audio_length': 160000,
                              'meta': {'mono_us': 7_000_000, 'sample_rate': 16000,
                                       'channels': 1, 'bits_per_sample': 16}})
        result = self.evaluate()
        self.assertIn('speech_detection_too_late', result['failures'])
        self.assertNotIn('new_user_turn_after_near_tail', result['failures'])

    def test_missing_content_assertions_are_not_acceptance(self):
        result = RUNNER.evaluate(copy.deepcopy(self.info), self.events, self.timeline,
                                 self.markers, interruption=True)
        self.assertFalse(result["passed"])
        self.assertIn("missing_expected_content", result["failures"])

    def test_far_only_cannot_accept_device_talking_to_itself(self):
        result = RUNNER.evaluate(self.info, self.events, self.timeline, self.markers, far_only=True)
        self.assertIn('speech_trigger_during_far_only', result['failures'])


if __name__ == "__main__":
    unittest.main()
