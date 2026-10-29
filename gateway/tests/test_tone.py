#!/usr/bin/env python3
"""Tests for the 2026-10-27 tone pass-through (ears).

SenseVoice's STT reply carries emotion/event next to text. The device only
reads "text" and sends it back as the chat message, so the gateway remembers
the last STT reply and adds one tone line to the brain's context when the
chat text matches. The brain call is replaced by a fake that records what it
was given; nothing real is run.

Run: python3 -m unittest tests.test_tone   (from gateway/)
"""

import http.client
import json
import os
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # gateway/

import cc_brain  # noqa: E402
import stackchan_gateway as gateway  # noqa: E402


def stt_body(text, emotion="NEUTRAL", event="Speech"):
    return json.dumps(
        {"text": text, "lang": "zh", "emotion": emotion, "event": event, "speech_sec": 1.2},
        ensure_ascii=False,
    ).encode("utf-8")


class ToneUnitTest(unittest.TestCase):
    def make_app(self):
        config = gateway.GatewayConfig(
            host="127.0.0.1",
            port=1,
            upstream_url="http://127.0.0.1:9/",
            upstream_ca=None,
            device_token="secret-token",
            stt_token_sha256="a" * 64,
        )
        return gateway.GatewayApplication(config)

    def test_hint_names_emotion_and_event(self):
        hint = gateway.tone_hint("HAPPY", "Laughter")
        self.assertIn("开心", hint)
        self.assertIn("笑声", hint)

    def test_neutral_speech_adds_nothing(self):
        self.assertEqual(gateway.tone_hint("NEUTRAL", "Speech"), "")
        self.assertEqual(gateway.tone_hint("EMO_UNKNOWN", ""), "")
        self.assertEqual(gateway.tone_hint(None, None), "")

    def test_event_alone_is_enough(self):
        hint = gateway.tone_hint("NEUTRAL", "Cough")
        self.assertIn("咳嗽", hint)
        self.assertNotIn("语气", hint)

    def test_matching_text_within_window(self):
        app = self.make_app()
        app.remember_stt(stt_body("在吃饭。", "HAPPY"), now=100.0)
        self.assertIn("开心", app.tone_for("在吃饭。", now=130.0))

    def test_whitespace_differences_still_match(self):
        app = self.make_app()
        app.remember_stt(stt_body("在 吃饭。", "SAD"), now=100.0)
        self.assertIn("难过", app.tone_for(" 在吃饭。\n", now=101.0))

    def test_other_text_gets_nothing(self):
        app = self.make_app()
        app.remember_stt(stt_body("在吃饭。", "HAPPY"), now=100.0)
        self.assertEqual(app.tone_for("你好", now=101.0), "")

    def test_stale_result_gets_nothing(self):
        app = self.make_app()
        app.remember_stt(stt_body("在吃饭。", "HAPPY"), now=100.0)
        self.assertEqual(
            app.tone_for("在吃饭。", now=100.0 + gateway.TONE_WINDOW_SECONDS + 1), ""
        )

    def test_used_only_once(self):
        app = self.make_app()
        app.remember_stt(stt_body("哈哈", "HAPPY", "Laughter"), now=100.0)
        self.assertNotEqual(app.tone_for("哈哈", now=101.0), "")
        self.assertEqual(app.tone_for("哈哈", now=102.0), "")

    def test_empty_or_broken_stt_reply_is_ignored(self):
        app = self.make_app()
        app.remember_stt(stt_body("", "HAPPY", "no_speech"), now=100.0)
        app.remember_stt(b"not json", now=100.0)
        app.remember_stt(b'["a list"]', now=100.0)
        self.assertIsNone(app._last_stt)


class ToneThroughGatewayTest(unittest.TestCase):
    """STT reply -> chat request over HTTP, with the brain call faked."""

    def setUp(self):
        self.saved_env = dict(os.environ)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        os.environ["STACKCHAN_BRAIN"] = "cc"
        os.environ["STACKCHAN_ROBOT_LOG"] = str(Path(self.tmp.name) / "robot-log.jsonl")
        os.environ.pop("STACKCHAN_LOOK", None)
        self.calls = []
        self.saved_answer = cc_brain.answer

        def fake_answer(heard, system_text, history, extra_context, look=False):
            self.calls.append({"heard": heard, "extra_context": extra_context})
            return cc_brain.CCResult(reply="嗯嗯", session_id="s", latency_ms=1, cost_usd=None)

        cc_brain.answer = fake_answer
        config = gateway.GatewayConfig(
            host="127.0.0.1",
            port=1,
            upstream_url="http://127.0.0.1:9/",
            upstream_ca=None,
            device_token="secret-token",
            stt_token_sha256="a" * 64,
        )
        self.app = gateway.GatewayApplication(config)
        self.app.authorized_stt = lambda authorization: True
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), gateway.make_handler(self.app))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        cc_brain.answer = self.saved_answer
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        os.environ.clear()
        os.environ.update(self.saved_env)

    def post(self, path, body, headers):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        conn.request("POST", path, body=body, headers=headers)
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response.status, data

    def hear(self, stt_reply, status=200):
        self.app.forward_stt = lambda body, authorization, content_type: (
            status, "application/json", stt_reply,
        )
        return self.post(
            gateway.STT_PATH,
            b"--x\r\n\r\naudio\r\n--x--\r\n",
            {"Authorization": "Bearer t", "Content-Type": "multipart/form-data; boundary=x"},
        )

    def say(self, text):
        body = json.dumps(
            {"messages": [{"role": "system", "content": "约定"}, {"role": "user", "content": text}]},
            ensure_ascii=False,
        ).encode("utf-8")
        return self.post(
            gateway.CHAT_PATH,
            body,
            {"Authorization": "Bearer secret-token", "Content-Type": "application/json"},
        )

    def test_happy_laugh_reaches_the_brain(self):
        status, data = self.hear(stt_body("帮他做了中辣", "HAPPY", "Laughter"))
        self.assertEqual(status, 200)
        # The device still gets the STT reply unchanged (plus the trailing CR).
        self.assertEqual(data, stt_body("帮他做了中辣", "HAPPY", "Laughter") + b"\r")
        status, _ = self.say("帮他做了中辣")
        self.assertEqual(status, 200)
        self.assertEqual(self.calls[-1]["heard"], "帮他做了中辣")
        self.assertIn("开心", self.calls[-1]["extra_context"])
        self.assertIn("笑声", self.calls[-1]["extra_context"])

    def test_neutral_leaves_context_empty(self):
        self.hear(stt_body("在吃饭。"))
        self.say("在吃饭。")
        self.assertEqual(self.calls[-1]["extra_context"], "")

    def test_failed_stt_is_not_remembered(self):
        self.hear(stt_body("在吃饭。", "ANGRY"), status=500)
        self.say("在吃饭。")
        self.assertEqual(self.calls[-1]["extra_context"], "")


if __name__ == "__main__":
    unittest.main()
