#!/usr/bin/env python3
"""Tests for cc_brain.py, robot_log_hook.py and the cc branch of
stackchan_gateway.py.

The fake `claude` executable is a shell script; STACKCHAN_CLAUDE_BIN points
at it, so the real claude CLI is never invoked. Run on the Mac:

    python3 test_cc_brain.py

NOTE (Muse, drafting only): I cannot run these tests from my side, so I am
not claiming they pass. The gateway HTTP tests spin up a real
ThreadingHTTPServer on 127.0.0.1 and POST real chat payloads at it.
"""

import contextlib
import http.client
import io
import json
import os
import stat
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from http.server import ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent  # gateway/ (tests live in gateway/tests/)
sys.path.insert(0, str(HERE))

import cc_brain  # noqa: E402
import robot_log_hook  # noqa: E402
import stackchan_gateway as gateway  # noqa: E402

STUB_SCRIPT = r"""#!/bin/bash
# Fake `claude` for tests. Records argv + stdin, then behaves per mode.
#   STACKCHAN_STUB_MODE: ok | exit1 | badjson | iserror | sleep5
set -u
echo "$@" > "$STACKCHAN_STUB_RECORD_DIR/argv.txt"
cat > "$STACKCHAN_STUB_RECORD_DIR/stdin.txt"
case "${STACKCHAN_STUB_MODE:-ok}" in
  ok)      echo '{"type":"result","is_error":false,"result":"知道了","total_cost_usd":0.002,"session_id":"stub-session-1","usage":{"input_tokens":2,"cache_creation_input_tokens":0,"cache_read_input_tokens":176645,"output_tokens":20}}' ;;
  exit1)   exit 1 ;;
  badjson)  echo 'this is not json' ;;
  iserror)  echo '{"type":"result","is_error":true,"result":""}' ;;
  sleep5)   sleep 5; echo '{"type":"result","is_error":false,"result":"晚了"}' ;;
esac
"""


def fresh_env(root: Path, mode: str = "ok") -> dict:
    record = root / "record"
    record.mkdir(exist_ok=True)
    stub = root / "claude"
    stub.write_text(STUB_SCRIPT, encoding="utf-8")
    stub.chmod(stub.stat().st_mode | stat.S_IEXEC)
    projects = root / "projects" / "-Users-you"
    projects.mkdir(parents=True, exist_ok=True)
    return {
        "STACKCHAN_CLAUDE_BIN": str(stub),
        "STACKCHAN_CLAUDE_PROJECTS_DIR": str(projects),
        "STACKCHAN_ROBOT_LOG": str(root / "robot-log.jsonl"),
        "STACKCHAN_STUB_RECORD_DIR": str(record),
        "STACKCHAN_STUB_MODE": mode,
        "STACKCHAN_CC_TIMEOUT": "10",
        "STACKCHAN_CC_MAX_AGE_HOURS": "72",
        "STACKCHAN_BRAIN": "cc",
        "STACKCHAN_WINDOW_CURSOR": str(root / ".window-cursor"),
    }


class EnvCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.saved_env = dict(os.environ)
        os.environ.update(fresh_env(self.root))
        self.projects = Path(os.environ["STACKCHAN_CLAUDE_PROJECTS_DIR"])
        self.record = Path(os.environ["STACKCHAN_STUB_RECORD_DIR"])

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved_env)

    def touch_session(self, age_seconds: float = 0.0) -> Path:
        path = self.projects / "abc123def456.jsonl"
        path.write_text('{"type":"user","entrypoint":"cli"}\n', encoding="utf-8")
        if age_seconds:
            stamp = time.time() - age_seconds
            os.utime(path, (stamp, stamp))
        return path

    def last_log_row(self) -> dict:
        lines = Path(os.environ["STACKCHAN_ROBOT_LOG"]).read_text(encoding="utf-8").splitlines()
        return json.loads(lines[-1])


def make_app() -> gateway.GatewayApplication:
    config = gateway.GatewayConfig(
        host="127.0.0.1",
        port=1,
        upstream_url="http://127.0.0.1:9/",  # http: no CA file needed
        upstream_ca=None,
        device_token="secret-token",
        stt_token_sha256="a" * 64,
    )
    service = gateway.WeatherService(
        fetch_json=lambda url, timeout: (_ for _ in ()).throw(
            AssertionError("test must not hit the network")
        )
    )
    return gateway.GatewayApplication(config, weather_service=service)


def serve(app):
    server = ThreadingHTTPServer(("127.0.0.1", 0), gateway.make_handler(app))
    thread = threading.Thread(
        target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True
    )
    thread.start()
    return server, server.server_address[1]


def post_chat(port: int, messages, token: str = "secret-token"):
    body = json.dumps({"messages": messages}, ensure_ascii=False).encode("utf-8")
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    conn.request(
        "POST",
        "/v1/chat/completions",
        body=body,
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"},
    )
    response = conn.getresponse()
    data = response.read()
    conn.close()
    return response.status, json.loads(data.decode("utf-8"))


def chat_messages(*user_texts, system="系统约定：开头用 emoji 表示情绪。"):
    messages = [{"role": "system", "content": system}]
    for index, text in enumerate(user_texts):
        messages.append({"role": "user" if index % 2 == 0 else "assistant", "content": text})
    return messages


class CCBrainUnitTest(EnvCase):
    def test_answer_ok(self):
        self.touch_session()
        result = cc_brain.answer(
            "你好呀", "系统约定", [("user", "早"), ("assistant", "早呀")], ""
        )
        self.assertEqual(result.reply, "知道了")
        self.assertEqual(result.session_id, "stub-session-1")
        self.assertEqual(result.cost_usd, 0.002)
        self.assertGreaterEqual(result.latency_ms, 0)

    def test_argv_and_stdin(self):
        self.touch_session()
        cc_brain.answer("明天天气怎么样", "系统约定", [], "")
        argv = (self.record / "argv.txt").read_text(encoding="utf-8")
        stdin = (self.record / "stdin.txt").read_text(encoding="utf-8")
        for flag in ("--fork-session", "--no-session-persistence", "disableAllHooks"):
            self.assertIn(flag, argv)
        # The question body must travel on stdin, never in argv.
        self.assertIn("明天天气怎么样", stdin)
        self.assertNotIn("明天天气怎么样", argv)
        # device_system goes into --append-system-prompt (in argv, as one arg).
        self.assertIn("系统约定", argv)

    def test_robot_rules_answer_in_the_questions_language(self):
        rules = cc_brain.robot_rules("系统约定")
        # 2026-10-28: was "用中文回答" — an English question abroad
        # still got a Chinese answer. Now: the question's language wins,
        # Chinese stays the default, the rest of the sentence is kept.
        self.assertIn("用主人提问用的语言回答", rules)
        self.assertIn("默认中文", rules)
        self.assertIn("有人用英文问就用英文答", rules)
        self.assertIn("一两句话，一般 50 个字以内", rules)
        self.assertNotIn("用中文回答，一两句话", rules)

    def test_no_session_dir_empty(self):
        with self.assertRaises(cc_brain.CCNoSession):
            cc_brain.answer("hi", "sys", [], "")
        self.assertFalse((self.record / "argv.txt").exists())

    def test_no_session_too_old(self):
        self.touch_session(age_seconds=73 * 3600)
        with self.assertRaises(cc_brain.CCNoSession):
            cc_brain.answer("hi", "sys", [], "")

    def test_timeout_kills_stub(self):
        os.environ["STACKCHAN_STUB_MODE"] = "sleep5"
        os.environ["STACKCHAN_CC_TIMEOUT"] = "1"
        self.touch_session()
        started = time.monotonic()
        with self.assertRaises(cc_brain.CCTimeout):
            cc_brain.answer("hi", "sys", [], "")
        # Must return promptly-ish after the 1s budget, not after sleep 5.
        self.assertLess(time.monotonic() - started, 4.0)

    def test_headless_session_newer_than_window_is_skipped(self):
        window = self.touch_session(age_seconds=600)
        headless = self.projects / "reminder.jsonl"
        headless.write_text('{"type":"user","entrypoint":"sdk-cli"}\n')
        self.assertEqual(cc_brain.find_window_session(), window.stem)

    def test_only_headless_sessions_means_none(self):
        (self.projects / "reminder.jsonl").write_text('{"entrypoint":"sdk-cli"}\n')
        (self.projects / "noentry.jsonl").write_text("{}\n")
        self.assertIsNone(cc_brain.find_window_session())

    def test_find_window_session_picks_newest(self):
        old = self.projects / "old.jsonl"
        old.write_text('{"entrypoint":"cli"}\n')
        stamp = time.time() - 3600
        os.utime(old, (stamp, stamp))
        new = self.touch_session()
        self.assertEqual(cc_brain.find_window_session(), new.stem)


class GatewayCCTest(EnvCase):
    def _fallback_app(self):
        app = make_app()
        app.forward = lambda body, auth: (
            200,
            "application/json",
            b'{"id":"upstream","choices":[{"message":{"role":"assistant","content":"fallback reply"}}]}',
        )
        return app

    def test_cc_success_shape_and_log(self):
        self.touch_session()
        app = make_app()
        server, port = serve(app)
        self.addCleanup(server.shutdown)
        status, payload = post_chat(port, chat_messages("早", "早呀", "今天忙吗"))
        self.assertEqual(status, 200)
        self.assertEqual(payload["object"], "chat.completion")
        self.assertEqual(payload["choices"][0]["message"]["role"], "assistant")
        self.assertEqual(payload["choices"][0]["message"]["content"], "知道了")
        self.assertEqual(payload["choices"][0]["finish_reason"], "stop")
        self.assertIn("usage", payload)
        row = self.last_log_row()
        self.assertEqual(row["brain"], "cc")
        self.assertEqual(row["reply"], "知道了")
        self.assertEqual(row["heard"], "今天忙吗")
        self.assertIsNone(row["error"])
        self.assertEqual(row["usage"]["cache_read_input_tokens"], 176645)

    def _fallback_case(self, mode, expected_error):
        os.environ["STACKCHAN_STUB_MODE"] = mode
        if mode == "sleep5":
            os.environ["STACKCHAN_CC_TIMEOUT"] = "1"
        self.touch_session()
        app = self._fallback_app()
        server, port = serve(app)
        self.addCleanup(server.shutdown)
        status, payload = post_chat(port, chat_messages("在吗"))
        self.assertEqual(status, 200)
        self.assertEqual(payload["id"], "upstream")  # came from the old path
        row = self.last_log_row()
        self.assertEqual(row["brain"], "api-fallback")
        self.assertEqual(row["error"], expected_error)

    def test_fallback_on_exit1(self):
        self._fallback_case("exit1", "CCBadOutput")

    def test_fallback_on_badjson(self):
        self._fallback_case("badjson", "CCBadOutput")

    def test_fallback_on_is_error(self):
        self._fallback_case("iserror", "CCBadOutput")

    def test_fallback_on_timeout(self):
        self._fallback_case("sleep5", "CCTimeout")

    def test_fallback_when_no_session(self):
        # projects dir empty: stub is never even started, old path is used.
        app = self._fallback_app()
        server, port = serve(app)
        self.addCleanup(server.shutdown)
        status, payload = post_chat(port, chat_messages("在吗"))
        self.assertEqual(status, 200)
        self.assertEqual(payload["id"], "upstream")
        self.assertFalse((self.record / "argv.txt").exists())
        row = self.last_log_row()
        self.assertEqual(row["brain"], "api-fallback")
        self.assertEqual(row["error"], "CCNoSession")

    def test_fallback_when_session_stale(self):
        self.touch_session(age_seconds=73 * 3600)
        app = self._fallback_app()
        server, port = serve(app)
        self.addCleanup(server.shutdown)
        status, _ = post_chat(port, chat_messages("在吗"))
        self.assertEqual(status, 200)
        self.assertEqual(self.last_log_row()["error"], "CCNoSession")

    def test_weather_context_reaches_stdin(self):
        self.touch_session()
        app = make_app()
        app.weather_service.context_for = lambda query: (
            "[WEATHER_TOOL_RESULT]\nstatus=success\n测试天气",
            gateway.Location(query=query, name=query, latitude=0.0, longitude=0.0),
        )
        server, port = serve(app)
        self.addCleanup(server.shutdown)
        status, _ = post_chat(port, chat_messages("今天天气怎么样"))
        self.assertEqual(status, 200)
        stdin = (self.record / "stdin.txt").read_text(encoding="utf-8")
        self.assertIn("[WEATHER_TOOL_RESULT]", stdin)

    def test_api_mode_never_touches_stub(self):
        os.environ["STACKCHAN_BRAIN"] = "api"
        self.touch_session()
        app = self._fallback_app()
        server, port = serve(app)
        self.addCleanup(server.shutdown)
        status, payload = post_chat(port, chat_messages("在吗"))
        self.assertEqual(status, 200)
        self.assertEqual(payload["id"], "upstream")
        # The stub binary was never started: no argv/stdin recordings at all.
        self.assertFalse((self.record / "argv.txt").exists())
        self.assertFalse((self.record / "stdin.txt").exists())
        # api mode keeps the old log behavior: no robot-log row from cc code.
        self.assertFalse(Path(os.environ["STACKCHAN_ROBOT_LOG"]).exists())

    def test_no_user_message_is_400(self):
        app = make_app()
        server, port = serve(app)
        self.addCleanup(server.shutdown)
        status, _ = post_chat(port, [{"role": "system", "content": "x"}])
        self.assertEqual(status, 400)


class HookTest(EnvCase):
    def _write_log(self, rows):
        path = Path(os.environ["STACKCHAN_ROBOT_LOG"])
        with path.open("a", encoding="utf-8") as handle:
            for row in rows:
                handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    def _transcript(self, entrypoint):
        path = self.root / ("session-%s.jsonl" % entrypoint)
        path.write_text(
            '{"type":"queue-operation"}\n{"type":"user","entrypoint":"%s"}\n' % entrypoint,
            encoding="utf-8",
        )
        return path

    def _run_hook(self, entrypoint="cli", stdin_text=None):
        # The hook reads Claude Code's hook JSON from stdin; by default we are
        # an interactive window ("cli") so the existing tests keep their meaning.
        if stdin_text is None:
            stdin_text = json.dumps({"transcript_path": str(self._transcript(entrypoint))})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), mock.patch("sys.stdin", io.StringIO(stdin_text)):
            code = robot_log_hook.main()
        return code, buf.getvalue()

    def _row(self, heard, reply, brain="cc"):
        return {
            "ts": "2026-10-04T07:10:00+08:00",
            "heard": heard,
            "reply": reply,
            "brain": brain,
            "session_id": "abc",
            "latency_ms": 1200,
            "cost_usd": 0.001,
            "error": None,
        }

    def test_prints_new_rows_then_quiet(self):
        self._write_log([self._row("你好", "你好呀"), self._row("天气", "还行", "api-fallback")])
        code, out = self._run_hook()
        self.assertEqual(code, 0)
        self.assertIn("[StackChan]", out)
        self.assertIn("2 轮", out)
        self.assertIn("fallback", out)  # api-fallback rounds are marked
        code, out = self._run_hook()
        self.assertEqual(code, 0)
        self.assertEqual(out, "")  # second run: nothing new, nothing printed

    def test_truncated_log_reads_from_start(self):
        self._write_log([self._row("a", "b")])
        self._run_hook()  # cursor now at end
        # Simulate rotation/truncation: shorter file.
        # The replacement must really be shorter, or a size-based check can't see it.
        Path(os.environ["STACKCHAN_ROBOT_LOG"]).write_text('{"heard":"c"}\n', encoding="utf-8")
        code, out = self._run_hook()
        self.assertEqual(code, 0)
        self.assertIn("c", out)

    def test_missing_log_is_quiet_and_zero(self):
        code, out = self._run_hook()
        self.assertEqual(code, 0)
        self.assertEqual(out, "")

    # ★2026-10-09 headless runs (muse-to-code bridge, reminders) fired this
    # global hook and walked off with the robot rows.
    def test_headless_session_leaves_rows_for_window(self):
        self._write_log([self._row("你好", "你好呀")])
        code, out = self._run_hook(entrypoint="sdk-cli")
        self.assertEqual(code, 0)
        self.assertEqual(out, "")
        self.assertFalse(Path(os.environ["STACKCHAN_WINDOW_CURSOR"]).exists())
        code, out = self._run_hook(entrypoint="cli")  # the owner's window still gets them
        self.assertIn("你好呀", out)

    def test_unknown_session_is_skipped(self):
        self._write_log([self._row("你好", "你好呀")])
        os.environ.pop("CLAUDE_CODE_ENTRYPOINT", None)
        for stdin_text in ("", "not json", json.dumps({"transcript_path": str(self.root / "missing.jsonl")})):
            code, out = self._run_hook(stdin_text=stdin_text)
            self.assertEqual((code, out), (0, ""))
        self.assertFalse(Path(os.environ["STACKCHAN_WINDOW_CURSOR"]).exists())

    def test_env_entrypoint_is_the_fallback(self):
        self._write_log([self._row("你好", "你好呀")])
        os.environ["CLAUDE_CODE_ENTRYPOINT"] = "sdk-cli"
        self.assertEqual(self._run_hook(stdin_text="{}")[1], "")
        os.environ["CLAUDE_CODE_ENTRYPOINT"] = "cli"
        self.assertIn("你好呀", self._run_hook(stdin_text="{}")[1])

    def test_transcript_beats_env(self):
        self._write_log([self._row("你好", "你好呀")])
        os.environ["CLAUDE_CODE_ENTRYPOINT"] = "cli"
        self.assertEqual(self._run_hook(entrypoint="sdk-cli")[1], "")

    def test_max_20_listed(self):
        self._write_log([self._row("h%d" % i, "r%d" % i) for i in range(25)])
        code, out = self._run_hook()
        self.assertEqual(code, 0)
        self.assertIn("25 轮", out)
        self.assertIn("更早还有 5 轮未列出", out)
        self.assertNotIn("h0", out)
        self.assertIn("h24", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
