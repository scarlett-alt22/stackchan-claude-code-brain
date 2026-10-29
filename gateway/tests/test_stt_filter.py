#!/usr/bin/env python3
"""Tests for the 2026-10-28 STT hallucination filter.

On silence/noise the recognizer sometimes returns subtitle-credit
boilerplate (seen live: "字幕志愿者 杨茜茜"). The gateway now blanks the
text of a matching transcript before relaying it to the device (the
device treats empty text as "didn't catch that") and keeps it out of the
tone memory. Ordinary speech that merely mentions 谢谢 or 订阅 must pass
through untouched. The STT upstream is faked; nothing real is run.

Run: python3 -m unittest tests.test_stt_filter   (from gateway/)
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

import stackchan_gateway as gateway  # noqa: E402


def stt_body(text, emotion="NEUTRAL", event="Speech"):
    return json.dumps(
        {"text": text, "lang": "zh", "emotion": emotion, "event": event, "speech_sec": 1.2},
        ensure_ascii=False,
    ).encode("utf-8")


class HallucinationPatternTest(unittest.TestCase):
    def test_subtitle_volunteer_credit(self):
        self.assertTrue(gateway.stt_hallucination("字幕志愿者 杨茜茜"))
        self.assertTrue(gateway.stt_hallucination("字幕志愿者杨茜茜"))

    def test_subtitle_by_provider(self):
        self.assertTrue(gateway.stt_hallucination("字幕由爱听写提供"))
        self.assertTrue(gateway.stt_hallucination("本字幕由某某工作室提供。"))

    def test_like_subscribe_begging(self):
        self.assertTrue(gateway.stt_hallucination("请不吝点赞订阅转发打赏"))
        self.assertTrue(gateway.stt_hallucination("请不吝点赞、订阅、转发、打赏"))

    def test_channel_credit(self):
        self.assertTrue(gateway.stt_hallucination("明镜与点点栏目"))

    def test_thanks_for_watching_only_when_whole(self):
        self.assertTrue(gateway.stt_hallucination("谢谢观看"))
        self.assertTrue(gateway.stt_hallucination("谢谢观看！"))
        self.assertFalse(gateway.stt_hallucination("谢谢观看今天的节目"))

    def test_normal_speech_with_thanks_or_subscribe_passes(self):
        self.assertFalse(gateway.stt_hallucination("小机器人谢谢你帮我订阅这个"))
        self.assertFalse(gateway.stt_hallucination("谢谢"))
        self.assertFalse(gateway.stt_hallucination("我想订阅明天的天气提醒"))

    def test_plain_speech_and_empty_pass(self):
        self.assertFalse(gateway.stt_hallucination("今天天气不错"))
        self.assertFalse(gateway.stt_hallucination(""))
        self.assertFalse(gateway.stt_hallucination("。"))


class DropHallucinationTest(unittest.TestCase):
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

    def test_matching_body_text_blanked_other_fields_kept(self):
        app = self.make_app()
        with self.assertLogs("stackchan-gateway", level="INFO") as logs:
            out = app.drop_stt_hallucination(stt_body("字幕志愿者 杨茜茜", "HAPPY"))
        result = json.loads(out.decode("utf-8"))
        self.assertEqual(result["text"], "")
        self.assertEqual(result["emotion"], "HAPPY")
        self.assertEqual(result["lang"], "zh")
        # Exactly one log line, and the dropped text is not in it.
        joined = "\n".join(logs.output)
        self.assertIn("stt hallucination dropped", joined)
        self.assertNotIn("杨茜茜", joined)

    def test_normal_body_returned_byte_for_byte(self):
        app = self.make_app()
        body = stt_body("小机器人谢谢你帮我订阅这个")
        self.assertEqual(app.drop_stt_hallucination(body), body)

    def test_non_json_and_non_dict_bodies_unchanged(self):
        app = self.make_app()
        self.assertEqual(app.drop_stt_hallucination(b"not json"), b"not json")
        self.assertEqual(app.drop_stt_hallucination(b'["a list"]'), b'["a list"]')
        self.assertEqual(app.drop_stt_hallucination(b'{"lang": "zh"}'), b'{"lang": "zh"}')


class HallucinationThroughGatewayTest(unittest.TestCase):
    """STT replies over HTTP: what the device gets, and what is remembered."""

    def setUp(self):
        self.saved_env = dict(os.environ)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        os.environ["STACKCHAN_ROBOT_LOG"] = str(Path(self.tmp.name) / "robot-log.jsonl")
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
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        os.environ.clear()
        os.environ.update(self.saved_env)

    def hear(self, stt_reply, status=200):
        self.app.forward_stt = lambda body, authorization, content_type: (
            status, "application/json", stt_reply,
        )
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        conn.request(
            "POST",
            gateway.STT_PATH,
            body=b"--x\r\n\r\naudio\r\n--x--\r\n",
            headers={"Authorization": "Bearer t", "Content-Type": "multipart/form-data; boundary=x"},
        )
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response.status, data

    def test_hallucination_blanked_and_not_remembered(self):
        status, data = self.hear(stt_body("字幕志愿者 杨茜茜", "HAPPY", "Laughter"))
        self.assertEqual(status, 200)
        result = json.loads(data.rstrip(b"\r").decode("utf-8"))
        self.assertEqual(result["text"], "")
        # The tone memory must not be fed a dropped transcript.
        self.assertIsNone(self.app._last_stt)

    def test_normal_speech_relayed_unchanged_and_remembered(self):
        body = stt_body("小机器人谢谢你帮我订阅这个", "HAPPY")
        status, data = self.hear(body)
        self.assertEqual(status, 200)
        self.assertEqual(data, body + b"\r")
        self.assertIsNotNone(self.app._last_stt)
        self.assertEqual(self.app._last_stt[0], "小机器人谢谢你帮我订阅这个")


if __name__ == "__main__":
    unittest.main()
