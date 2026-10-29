#!/usr/bin/env python3
"""Tests for the POST /v1/photos route and photo rows in robot_log_hook.

Run: python3 test_photos.py

Covers: valid JPEG -> 200, file on disk with identical bytes, one photo row
in robot-log.jsonl; unauthenticated -> 401; wrong Content-Type -> 415;
body > 1MB -> 413; non-JPEG / truncated JPEG -> 400 and nothing saved;
same-second uploads -> no overwrite ("-2" suffix); hook lists photo paths
and still renders chat rows.
"""

import io
import json
import os
import sys
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request
from contextlib import redirect_stdout
from datetime import datetime
from http.server import ThreadingHTTPServer
from pathlib import Path
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # gateway/

import stackchan_gateway as gw
import robot_log_hook as hook

JPEG = b"\xff\xd8" + b"\x00" * 100 + b"\xff\xd9"
NOT_JPEG = b"this is not a jpeg"
TRUNCATED_JPEG = b"\xff\xd8" + b"\x00" * 100  # head ok, tail missing
AUTH = {"Authorization": "Bearer test-secret", "Content-Type": "image/jpeg"}


def make_config(photo_dir: Path) -> gw.GatewayConfig:
    return gw.GatewayConfig(
        host="127.0.0.1",
        port=0,
        upstream_url="http://127.0.0.1:1/v1/chat/completions",
        upstream_ca=None,
        device_token="test-secret",
        device_token_sha256="",
        upstream_timeout=5,
        stt_upstream_url="http://127.0.0.1:1/v1/audio/transcriptions",
        stt_token_sha256="0" * 64,
        stt_timeout=5,
        photo_dir=photo_dir,
    )


class PhotoRouteTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photo-test-"))
        self.photo_dir = self.tmp / "photos"
        self.app = gw.GatewayApplication(make_config(self.photo_dir))
        self.log_path = self.tmp / "robot-log.jsonl"
        self._old_log_env = os.environ.get("STACKCHAN_ROBOT_LOG")
        os.environ["STACKCHAN_ROBOT_LOG"] = str(self.log_path)
        self.server = ThreadingHTTPServer(
            ("127.0.0.1", 0), gw.make_handler(self.app)
        )
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True
        )
        self.thread.start()
        self.url = "http://127.0.0.1:{}/v1/photos".format(
            self.server.server_port
        )

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        if self._old_log_env is None:
            os.environ.pop("STACKCHAN_ROBOT_LOG", None)
        else:
            os.environ["STACKCHAN_ROBOT_LOG"] = self._old_log_env

    def post(self, body: bytes, headers: dict):
        request = urllib.request.Request(
            self.url, data=body, headers=headers, method="POST"
        )
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read().decode("utf-8"))

    def saved_jpegs(self):
        return list(self.photo_dir.rglob("*.jpg"))

    def test_valid_jpeg(self):
        status, payload = self.post(JPEG, dict(AUTH))
        self.assertEqual(status, 200)
        self.assertTrue(payload.get("ok"))
        rel = payload["path"]
        saved = self.photo_dir / rel
        self.assertTrue(saved.is_file(), "photo file not on disk")
        self.assertEqual(saved.read_bytes(), JPEG)
        rows = [
            json.loads(line)
            for line in self.log_path.read_text(encoding="utf-8").splitlines()
        ]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["type"], "photo")
        self.assertEqual(rows[0]["path"], rel)
        self.assertEqual(rows[0]["bytes"], len(JPEG))
        self.assertIn("ts", rows[0])

    def test_unauthorized(self):
        status, _ = self.post(JPEG, {"Content-Type": "image/jpeg"})
        self.assertEqual(status, 401)
        status, _ = self.post(
            JPEG,
            {"Authorization": "Bearer wrong", "Content-Type": "image/jpeg"},
        )
        self.assertEqual(status, 401)
        self.assertEqual(self.saved_jpegs(), [])

    def test_wrong_content_type(self):
        headers = dict(AUTH)
        headers["Content-Type"] = "application/octet-stream"
        status, _ = self.post(JPEG, headers)
        self.assertEqual(status, 415)
        self.assertEqual(self.saved_jpegs(), [])

    def test_too_large(self):
        body = b"\xff\xd8" + b"\x00" * gw.MAX_PHOTO_BYTES + b"\xff\xd9"
        # The server answers 413 without reading the body and closes. On macOS
        # the socket buffer is small enough that the client is still sending
        # and sees a broken pipe instead of the 413; both mean "rejected".
        try:
            status, _ = self.post(body, dict(AUTH))
            self.assertEqual(status, 413)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except urllib.error.URLError as exc:
            self.assertIsInstance(exc.reason, (BrokenPipeError, ConnectionResetError))
        self.assertEqual(self.saved_jpegs(), [])

    def test_not_jpeg(self):
        status, payload = self.post(NOT_JPEG, dict(AUTH))
        self.assertEqual(status, 400)
        self.assertEqual(self.saved_jpegs(), [])

    def test_truncated_jpeg(self):
        status, _ = self.post(TRUNCATED_JPEG, dict(AUTH))
        self.assertEqual(status, 400)
        self.assertEqual(self.saved_jpegs(), [])

    def test_same_second_no_overwrite(self):
        moment = datetime(2026, 10, 4, 7, 30, 0, tzinfo=ZoneInfo("Asia/Tokyo"))
        rel1, _ = self.app.save_photo(JPEG, now=moment)
        rel2, _ = self.app.save_photo(JPEG, now=moment)
        self.assertNotEqual(rel1, rel2)
        self.assertTrue(rel1.endswith("073000.jpg"))
        self.assertTrue(rel2.endswith("073000-2.jpg"))
        self.assertEqual((self.photo_dir / rel1).read_bytes(), JPEG)
        self.assertEqual((self.photo_dir / rel2).read_bytes(), JPEG)


class HookPhotoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="hook-test-"))
        self._old = {
            key: os.environ.get(key)
            for key in (
                "STACKCHAN_ROBOT_LOG",
                "STACKCHAN_WINDOW_CURSOR",
                "STACKCHAN_PHOTO_DIR",
            )
        }
        os.environ["STACKCHAN_ROBOT_LOG"] = str(self.tmp / "robot-log.jsonl")
        os.environ["STACKCHAN_WINDOW_CURSOR"] = str(self.tmp / "cursor")
        os.environ["STACKCHAN_PHOTO_DIR"] = str(self.tmp / "photos")

    def tearDown(self):
        for key, value in self._old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value

    def run_hook(self, rows):
        log = Path(os.environ["STACKCHAN_ROBOT_LOG"])
        log.write_text(
            "\n".join(json.dumps(row, ensure_ascii=False) for row in rows) + "\n",
            encoding="utf-8",
        )
        transcript = self.tmp / "window.jsonl"
        transcript.write_text('{"type":"user","entrypoint":"cli"}\n', encoding="utf-8")
        stdin = io.StringIO(json.dumps({"transcript_path": str(transcript)}))
        buffer = io.StringIO()
        with redirect_stdout(buffer), mock.patch("sys.stdin", stdin):
            code = hook.main()
        return code, buffer.getvalue()

    def test_hook_lists_photos_and_chats(self):
        photo_dir = Path(os.environ["STACKCHAN_PHOTO_DIR"])
        rows = [
            {
                "type": "photo",
                "ts": "2026-10-04T07:30:00+08:00",
                "path": "2026-10-04/073000.jpg",
                "bytes": 123,
            },
            {
                "type": "photo",
                "ts": "2026-10-04T07:31:00+08:00",
                "path": "2026-10-04/073100.jpg",
                "bytes": 124,
            },
            {
                "heard": "你好",
                "reply": "你好呀",
                "ts": "2026-10-04T07:32:00+08:00",
                "brain": "cc",
            },
        ]
        code, out = self.run_hook(rows)
        self.assertEqual(code, 0)
        self.assertIn("[StackChan] 拍了 2 张照片：", out)
        self.assertIn(str(photo_dir / "2026-10-04/073000.jpg"), out)
        self.assertIn(str(photo_dir / "2026-10-04/073100.jpg"), out)
        # chat rows are rendered as before
        self.assertIn("主人：「你好」", out)

    def test_hook_photo_list_cap(self):
        rows = [
            {
                "type": "photo",
                "ts": "2026-10-04T07:30:00+08:00",
                "path": "2026-10-04/0730{:02d}.jpg".format(i),
                "bytes": 100 + i,
            }
            for i in range(12)
        ]
        code, out = self.run_hook(rows)
        self.assertEqual(code, 0)
        self.assertIn("[StackChan] 拍了 12 张照片：", out)
        self.assertIn("更早还有 2 张未列出。", out)
        self.assertNotIn("073000.jpg", out)  # oldest two are cut


if __name__ == "__main__":
    unittest.main(verbosity=2)
