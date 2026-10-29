#!/usr/bin/env python3
"""Tests for the 2026-10-28 fallback reply logging.

When the cc brain fails, chat_via_cc falls back to the API upstream. The
robot-log row used to be written before the forward, with reply "", so the
log never recorded what the fallback model actually said. Now the row is
written after forward() returns: on a parseable 200 the reply is the
upstream content; otherwise the reply stays "" and the error gains the
upstream status. The brain call is faked to raise; the forward is faked
per test; nothing real is run.

Run: python3 -m unittest tests.test_fallback_log   (from gateway/)
"""

import http.client
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # gateway/

import cc_brain  # noqa: E402
import stackchan_gateway as gateway  # noqa: E402

UPSTREAM_OK = json.dumps(
    {
        "id": "upstream-1",
        "object": "chat.completion",
        "choices": [
            {"index": 0, "message": {"role": "assistant", "content": "备用脑的回答"}, "finish_reason": "stop"}
        ],
    },
    ensure_ascii=False,
).encode("utf-8")


class FallbackLogTest(unittest.TestCase):
    def setUp(self):
        self.saved_env = dict(os.environ)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        os.environ["STACKCHAN_BRAIN"] = "cc"
        self.log_path = Path(self.tmp.name) / "robot-log.jsonl"
        os.environ["STACKCHAN_ROBOT_LOG"] = str(self.log_path)
        os.environ.pop("STACKCHAN_LOOK", None)
        self.saved_answer = cc_brain.answer

        def failing_answer(heard, system_text, history, extra_context, look=False):
            raise cc_brain.CCBadOutput("stub brain failure")

        cc_brain.answer = failing_answer
        config = gateway.GatewayConfig(
            host="127.0.0.1",
            port=1,
            upstream_url="http://127.0.0.1:9/",
            upstream_ca=None,
            device_token="secret-token",
            stt_token_sha256="a" * 64,
        )
        self.app = gateway.GatewayApplication(config)
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

    def say(self, text, upstream):
        self.app.forward = lambda body, authorization: upstream
        body = json.dumps(
            {"messages": [{"role": "system", "content": "约定"}, {"role": "user", "content": text}]},
            ensure_ascii=False,
        ).encode("utf-8")
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        conn.request(
            "POST",
            gateway.CHAT_PATH,
            body=body,
            headers={"Authorization": "Bearer secret-token", "Content-Type": "application/json"},
        )
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response.status, data

    def last_log_row(self):
        lines = self.log_path.read_text(encoding="utf-8").strip().splitlines()
        return json.loads(lines[-1])

    def test_fallback_reply_is_logged(self):
        status, data = self.say("在吗", (200, "application/json", UPSTREAM_OK))
        self.assertEqual(status, 200)
        # The device still gets the upstream body byte-for-byte.
        self.assertEqual(data, UPSTREAM_OK)
        row = self.last_log_row()
        self.assertEqual(row["brain"], "api-fallback")
        self.assertEqual(row["heard"], "在吗")
        self.assertEqual(row["reply"], "备用脑的回答")
        self.assertEqual(row["error"], "CCBadOutput")

    def test_fallback_upstream_non200(self):
        status, data = self.say("在吗", (502, "text/html", b"Bad Gateway"))
        self.assertEqual(status, 502)
        self.assertEqual(data, b"Bad Gateway")
        row = self.last_log_row()
        self.assertEqual(row["reply"], "")
        self.assertEqual(row["error"], "CCBadOutput;upstream=502")

    def test_fallback_upstream_non_json_body(self):
        status, data = self.say("在吗", (200, "application/json", b"<html>not json</html>"))
        self.assertEqual(status, 200)
        self.assertEqual(data, b"<html>not json</html>")
        row = self.last_log_row()
        self.assertEqual(row["reply"], "")
        self.assertEqual(row["error"], "CCBadOutput;upstream=200")

    def test_fallback_upstream_json_without_choices(self):
        body = b'{"id": "x", "object": "chat.completion"}'
        status, data = self.say("在吗", (200, "application/json", body))
        self.assertEqual(status, 200)
        self.assertEqual(data, body)
        row = self.last_log_row()
        self.assertEqual(row["reply"], "")
        self.assertEqual(row["error"], "CCBadOutput;upstream=200")

    def test_fallback_upstream_unreachable_still_logged(self):
        # Both brains down (10-26 outage): forward() raises instead of returning.
        def unreachable(body, authorization):
            raise urllib.error.URLError("connection refused")

        self.app.forward = unreachable
        body = json.dumps({"messages": [{"role": "user", "content": "在吗"}]}).encode("utf-8")
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        conn.request(
            "POST", gateway.CHAT_PATH, body=body,
            headers={"Authorization": "Bearer secret-token", "Content-Type": "application/json"},
        )
        response = conn.getresponse()
        response.read()
        conn.close()
        self.assertEqual(response.status, 502)
        row = self.last_log_row()
        self.assertEqual(row["heard"], "在吗")
        self.assertEqual(row["reply"], "")
        self.assertEqual(row["error"], "CCBadOutput;upstream=URLError")


if __name__ == "__main__":
    unittest.main()
