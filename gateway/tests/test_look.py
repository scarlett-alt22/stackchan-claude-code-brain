#!/usr/bin/env python3
"""Tests for the 2026-10-05 "look" feature.

My reply ends in [[look]] -> the device photographs straight ahead and posts
the JPEG to /v1/photos with X-StackChan-Look: 1 -> the gateway forks the
window again with the photo attached (stream-json input) and returns the
answer in the photo response.

The fake `claude` below answers text questions with $STACKCHAN_STUB_TEXT_REPLY
and image questions (stream-json input) with $STACKCHAN_STUB_IMAGE_REPLY, the
latter as stream-json output like the real CLI. The real CLI is never run.

Run: python3 -m unittest tests.test_look   (from gateway/)
"""

import base64
import http.client
import json
import os
import stat
import sys
import tempfile
import threading
import time
import unittest
from datetime import datetime, timedelta
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # gateway/

import cc_brain  # noqa: E402
import stackchan_gateway as gateway  # noqa: E402

STUB = r"""#!/bin/bash
set -u
N=$(printf %04d "$(ls "$STACKCHAN_STUB_RECORD_DIR" | wc -l | tr -d ' ')")
echo "$@" > "$STACKCHAN_STUB_RECORD_DIR/argv-$N.txt"
cat > "$STACKCHAN_STUB_RECORD_DIR/stdin-$N.txt"
case " $* " in
  *" --input-format stream-json "*)
    echo '{"type":"system","subtype":"init","tools":[],"session_id":"stub-look"}'
    echo '{"type":"assistant","message":{"content":[{"type":"text","text":"ignored"}]}}'
    printf '{"type":"result","is_error":false,"result":"%s","session_id":"stub-look","usage":{"input_tokens":3,"cache_creation_input_tokens":900,"cache_read_input_tokens":195000,"output_tokens":30}}\n' "$STACKCHAN_STUB_IMAGE_REPLY"
    ;;
  *)
    printf '{"type":"result","is_error":false,"result":"%s","session_id":"stub-text","usage":{"input_tokens":2,"cache_creation_input_tokens":0,"cache_read_input_tokens":195000,"output_tokens":10}}\n' "$STACKCHAN_STUB_TEXT_REPLY"
    ;;
esac
"""

JPEG = b"\xff\xd8" + b"\x11" * 200 + b"\xff\xd9"


class LookCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.record = root / "record"
        self.record.mkdir()
        stub = root / "claude"
        stub.write_text(STUB, encoding="utf-8")
        stub.chmod(stub.stat().st_mode | stat.S_IEXEC)
        projects = root / "projects"
        projects.mkdir()
        (projects / "window1.jsonl").write_text('{"entrypoint":"cli"}\n', encoding="utf-8")
        self.log_path = root / "robot-log.jsonl"
        self.photo_dir = root / "photos"
        self.saved_env = dict(os.environ)
        os.environ.update(
            {
                "STACKCHAN_CLAUDE_BIN": str(stub),
                "STACKCHAN_CLAUDE_PROJECTS_DIR": str(projects),
                "STACKCHAN_ROBOT_LOG": str(self.log_path),
                "STACKCHAN_STUB_RECORD_DIR": str(self.record),
                "STACKCHAN_STUB_TEXT_REPLY": "好，我看看。[[look]]",
                "STACKCHAN_STUB_IMAGE_REPLY": "看到了，是一艘白色帆船。",
                "STACKCHAN_CC_TIMEOUT": "10",
                "STACKCHAN_BRAIN": "cc",
                "STACKCHAN_LOOK": "1",
            }
        )
        config = gateway.GatewayConfig(
            host="127.0.0.1",
            port=0,
            upstream_url="http://127.0.0.1:9/",
            upstream_ca=None,
            device_token="secret-token",
            stt_token_sha256="a" * 64,
            photo_dir=self.photo_dir,
        )
        self.app = gateway.GatewayApplication(config)
        self.app.forward = lambda body, auth: (
            200, "application/json", b'{"id":"upstream","choices":[]}'
        )
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), gateway.make_handler(self.app))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.port = self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        os.environ.clear()
        os.environ.update(self.saved_env)

    # --- helpers -------------------------------------------------------
    def request(self, path, body, headers):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        conn.request("POST", path, body=body, headers=headers)
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response.status, json.loads(data.decode("utf-8"))

    def chat(self, text):
        messages = [
            {"role": "system", "content": "设备约定：开头用 emoji。"},
            {"role": "user", "content": text},
        ]
        body = json.dumps({"messages": messages}, ensure_ascii=False).encode("utf-8")
        status, payload = self.request(
            "/v1/chat/completions",
            body,
            {"Authorization": "Bearer secret-token", "Content-Type": "application/json"},
        )
        self.assertEqual(status, 200)
        return payload["choices"][0]["message"]["content"]

    def photo(self, look=True, token="secret-token"):
        headers = {"Authorization": "Bearer " + token, "Content-Type": "image/jpeg"}
        if look:
            headers["X-StackChan-Look"] = "1"
        return self.request("/v1/photos", JPEG, headers)

    def log_rows(self):
        return [json.loads(line) for line in self.log_path.read_text(encoding="utf-8").splitlines()]

    def recorded(self, prefix):
        files = sorted(self.record.glob(prefix + "-*.txt"))
        return [f.read_text(encoding="utf-8") for f in files]

    # --- rules -----------------------------------------------------------
    def test_rules_mention_look_only_when_enabled(self):
        self.assertIn("[[look]]", cc_brain.robot_rules("x", look=True))
        self.assertNotIn("[[look]]", cc_brain.robot_rules("x", look=False))

    def test_tag_helpers(self):
        self.assertTrue(cc_brain.has_look_tag("我看看 [[look]]"))
        self.assertTrue(cc_brain.has_look_tag("[[ LOOK x=0 ]]"))
        self.assertFalse(cc_brain.has_look_tag("look 一下"))
        self.assertEqual(cc_brain.strip_look_tags("我看看。[[look]]"), "我看看。")

    # --- round one --------------------------------------------------------
    def test_switch_on_device_gets_tag_and_rules_include_it(self):
        reply = self.chat("你看前面有一艘船")
        self.assertEqual(reply, "好，我看看。[[look]]")
        argv = self.recorded("argv")[-1]
        self.assertIn("[[look]]", argv)  # the rule rides in --append-system-prompt
        self.assertIn("--output-format json", argv)  # text path unchanged

    def test_switch_off_device_never_sees_tag(self):
        os.environ["STACKCHAN_LOOK"] = "0"
        reply = self.chat("你看前面有一艘船")
        self.assertEqual(reply, "好，我看看。")
        self.assertNotIn("[[look]]", self.recorded("argv")[-1])
        # robot-log keeps what I actually said
        self.assertEqual(self.log_rows()[-1]["reply"], "好，我看看。[[look]]")

    def test_switch_off_tag_only_reply_is_not_spoken_raw(self):
        os.environ["STACKCHAN_LOOK"] = "0"
        os.environ["STACKCHAN_STUB_TEXT_REPLY"] = "[[look]]"
        self.assertEqual(self.chat("看"), "嗯。")

    # --- round two --------------------------------------------------------
    def test_full_look_round_trip(self):
        self.chat("你看前面有一艘船")
        status, payload = self.photo()
        self.assertEqual(status, 200)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["reply"], "看到了，是一艘白色帆船。")
        self.assertNotIn("look_error", payload)
        # photo stored as usual
        self.assertTrue((self.photo_dir / payload["path"]).is_file())
        # the second fork got stream-json input with the image and the question
        argv = self.recorded("argv")[-1]
        self.assertIn("--input-format stream-json", argv)
        self.assertIn("--output-format stream-json", argv)
        self.assertIn("--fork-session", argv)
        message = json.loads(self.recorded("stdin")[-1])
        blocks = message["message"]["content"]
        self.assertEqual(blocks[0]["type"], "image")
        self.assertEqual(base64.b64decode(blocks[0]["source"]["data"]), JPEG)
        self.assertEqual(blocks[0]["source"]["media_type"], "image/jpeg")
        self.assertIn("你看前面有一艘船", blocks[1]["text"])
        self.assertIn("这一轮不要再输出 [[look]]", blocks[1]["text"])
        # the image round does not offer [[look]] again
        self.assertNotIn("机器人能替你看", argv)
        # logged as cc-look with the image answer
        last = self.log_rows()[-1]
        self.assertEqual(last["brain"], "cc-look")
        self.assertEqual(last["heard"], "你看前面有一艘船")
        self.assertEqual(last["reply"], "看到了，是一艘白色帆船。")
        # the device emoji contract from the chat request is reused
        self.assertIn("设备约定：开头用 emoji。", argv)

    def test_second_tag_in_answer_is_dropped(self):
        self.chat("你看前面有一艘船")
        os.environ["STACKCHAN_STUB_IMAGE_REPLY"] = "有点远，再看一次？[[look]]"
        _status, payload = self.photo()
        self.assertEqual(payload["reply"], "有点远，再看一次？")

    def test_photo_without_header_has_no_reply(self):
        self.chat("你看前面有一艘船")
        _status, payload = self.photo(look=False)
        self.assertNotIn("reply", payload)
        self.assertEqual(len(self.recorded("argv")), 1)  # no second fork

    def test_look_header_without_request(self):
        _status, payload = self.photo()
        self.assertEqual(payload["reply"], "")
        self.assertEqual(payload["look_error"], "no_look_request")
        self.assertTrue((self.photo_dir / payload["path"]).is_file())

    def test_look_header_with_switch_off(self):
        self.chat("你看前面有一艘船")
        os.environ["STACKCHAN_LOOK"] = "0"
        _status, payload = self.photo()
        self.assertEqual(payload["look_error"], "look_disabled")

    def test_expired_request(self):
        old = (datetime.now().astimezone() - timedelta(seconds=600)).isoformat()
        row = {"ts": old, "heard": "看看", "reply": "我看看[[look]]", "brain": "cc"}
        self.log_path.write_text(json.dumps(row, ensure_ascii=False) + "\n", encoding="utf-8")
        _status, payload = self.photo()
        self.assertEqual(payload["look_error"], "look_request_expired")

    def test_unauthorized_photo_never_forks(self):
        self.chat("你看前面有一艘船")
        status, _payload = self.photo(token="wrong")
        self.assertEqual(status, 401)
        self.assertEqual(len(self.recorded("argv")), 1)


if __name__ == "__main__":
    unittest.main()
