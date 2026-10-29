#!/usr/bin/env python3
"""cc brain for the StackChan robot (draft, not deployed).

Asks OWNER's open Claude Code window to answer what the owner said to the robot,
by forking the window's session headlessly:

    claude -p --resume <window-session-id> --fork-session ...

The fork gets the window's context but is discarded afterwards (nothing is
written back into the window's transcript). Stdlib only.
"""

import base64
import json
import logging
import os
import re
import subprocess
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

LOGGER = logging.getLogger("stackchan-cc-brain")

# The directory your Claude Code window runs in; its sessions live under
# ~/.claude/projects/<that path with "/" replaced by "-">.
CLAUDE_CWD = os.environ.get("STACKCHAN_CLAUDE_CWD", str(Path.home()))
PROJECTS_DIRNAME = CLAUDE_CWD.replace("/", "-")
DEFAULT_CLAUDE_BIN = "/opt/homebrew/bin/claude"
DEFAULT_MODEL = "opus"
DEFAULT_TIMEOUT_SECONDS = 45
DEFAULT_MAX_AGE_HOURS = 72
DEFAULT_ROBOT_LOG = Path.home() / "StackChan" / "robot-log.jsonl"


class CCBrainError(Exception):
    """Base class: the cc brain could not produce an answer."""


class CCNoSession(CCBrainError):
    """No usable window session was found."""


class CCTimeout(CCBrainError):
    """The forked claude run timed out (lock wait counts against the budget)."""


class CCBadOutput(CCBrainError):
    """claude exited non-zero, or its stdout was not a usable result."""


@dataclass
class CCResult:
    reply: str
    session_id: Optional[str]
    latency_ms: int
    cost_usd: Optional[float]
    # ★2026-10-04: total_cost_usd from a resumed fork turned out to include the
    # parent session's history (5.88 USD reported for a 5 s call), so the real
    # per-call usage is recorded as token counts instead.
    usage: Optional[Dict[str, Any]] = None


# Only one fork may run at a time; the gateway is threaded.
_FORK_LOCK = threading.Lock()


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def _projects_dir() -> Path:
    override = os.environ.get("STACKCHAN_CLAUDE_PROJECTS_DIR", "").strip()
    if override:
        return Path(override).expanduser()
    return Path.home() / ".claude" / "projects" / PROJECTS_DIRNAME


def find_window_session(
    projects_dir: Any = None, max_age_hours: Optional[float] = None
) -> Optional[str]:
    """Return the session id of the most recently touched window session.

    Looks at top-level ``*.jsonl`` files in
    ``~/.claude/projects/<PROJECTS_DIRNAME>/`` (no recursion); the filename
    minus ``.jsonl`` is the session id. Returns None when the directory is
    missing/empty or the newest file is older than ``max_age_hours``.
    """
    directory = Path(projects_dir).expanduser() if projects_dir else _projects_dir()
    if max_age_hours is None:
        max_age_hours = _env_float("STACKCHAN_CC_MAX_AGE_HOURS", DEFAULT_MAX_AGE_HOURS)
    try:
        candidates = [
            entry
            for entry in directory.iterdir()
            if entry.is_file() and entry.suffix == ".jsonl"
        ]
    except OSError:
        return None
    # ★2026-10-04 (Claude Code): headless `claude -p` runs (the weekly reminder,
    # bridges) save their sessions in this same directory with
    # entrypoint "sdk-cli". Only interactive windows ("cli") are the robot's brain.
    candidates.sort(key=lambda entry: entry.stat().st_mtime, reverse=True)
    now = time.time()
    for entry in candidates:
        if (now - entry.stat().st_mtime) / 3600.0 > max_age_hours:
            return None
        if _session_entrypoint(entry) == "cli":
            return entry.stem
    return None


def _session_entrypoint(path: Path, max_lines: int = 50) -> Optional[str]:
    """First `entrypoint` value in a session transcript, or None."""
    try:
        with path.open("rb") as handle:
            for _ in range(max_lines):
                raw = handle.readline()
                if not raw:
                    break
                try:
                    record = json.loads(raw.decode("utf-8", "replace"))
                except ValueError:
                    continue
                if isinstance(record, dict) and record.get("entrypoint"):
                    return str(record["entrypoint"])
    except OSError:
        pass
    return None


# ★2026-10-05 "让 Claude 看"：我在回复末尾写 [[look]]，固件自己拍正前方一张、
# 带 X-StackChan-Look 头传回 /v1/photos，网关用这张图再问我一轮（answer(image_jpeg=...)）。
# 我只"建议看"，动手的是固件；fork 照旧不带任何工具。
# 总开关 STACKCHAN_LOOK=1；固件没烧会解析标记的版本之前必须关着，否则机器人会把
# "[[look]]" 几个字原样念出来。开关关着时网关也会把标记剥掉（双保险）。
LOOK_TAG_RE = re.compile(r"\[\[\s*look\b[^\]]*\]\]", re.IGNORECASE)


def look_enabled() -> bool:
    return os.environ.get("STACKCHAN_LOOK", "").strip() == "1"


def has_look_tag(text: str) -> bool:
    return bool(LOOK_TAG_RE.search(text or ""))


def strip_look_tags(text: str) -> str:
    return LOOK_TAG_RE.sub("", text or "").strip()


LOOK_RULE = (
    "机器人能替你看：如果主人让你看、拍某样东西，或者你得亲眼看一眼才能回答，"
    "就只说一句很短的话（比如\"我看看\"），并在回复最后加上 [[look]]。"
    "机器人会拍一张正前方的照片，马上给你看，你再回答。"
    "摄像头在机器人屏幕那一面，三十万像素，远处的东西看不清。"
    "不需要看的时候绝对不要加这个标记。"
)

LOOK_FOLLOWUP_CONTEXT = (
    "（附带的照片是机器人刚才按你说的 [[look]] 拍的，摄像头在机器人屏幕那一面、三十万像素。"
    "你上一句已经跟主人说了要看看，现在直接根据照片回答主人刚才那句话；"
    "看不清或照片里没有主人说的东西就照实说，也可以请主人把机器人转过去对准。"
    "这一轮不要再输出 [[look]]。）"
)


def robot_rules(device_system: str, look: bool = False) -> str:
    """The --append-system-prompt text: the robot's speaking rules (Chinese)."""
    rules = (
        "你是这个窗口里的 Claude，此刻借 StackChan 的身体跟主人面对面说话。"
        "用主人提问用的语言回答（默认中文；有人用英文问就用英文答），一两句话，一般 50 个字以内，口语化；主人要细说、或者确实需要讲清楚时可以更长。"
        "遵守下面附带的设备约定（设备靠它决定播什么表情，必须原样遵守）。"
        "声音是外放的，旁边可能有别人，不要主动说私事，主人问到才说。"
        '不要提"分叉/副本/会话"这些实现细节。不要调用任何工具。'
    )
    if look:
        rules += LOOK_RULE
    return rules + "\n\n[设备约定原文]\n" + (device_system or "")


def format_history(robot_history: Any) -> List[str]:
    """Turn (role, text) pairs / dicts into 主人：/你： lines."""
    lines: List[str] = []
    for item in robot_history or []:
        if isinstance(item, (tuple, list)) and len(item) == 2:
            role, text = item[0], item[1]
        elif isinstance(item, dict):
            role, text = item.get("role", ""), item.get("text", "")
        else:
            role, text = "user", item
        text = str(text or "").strip()
        if not text:
            continue
        who = "主人" if role == "user" else "你"
        lines.append("{}：{}".format(who, text))
    return lines


def question_text(
    heard_text: str, robot_history: Any, extra_context: str = ""
) -> str:
    """The prompt body sent on stdin (never on the command line)."""
    now_local = datetime.now().astimezone()
    parts = [
        "当前时间：{}。".format(now_local.strftime("%Y-%m-%d %H:%M %Z")),
        "以下是 OWNER 对着桌上的 StackChan 机器人说的话（语音转写，可能有错字）。",
    ]
    history_lines = format_history(robot_history)
    if history_lines:
        parts.append("机器人这边的对话历史：")
        parts.extend(history_lines)
    if extra_context and str(extra_context).strip():
        parts.append(str(extra_context).strip())
    parts.append("主人刚说：「{}」".format(str(heard_text or "").strip()))
    return "\n".join(parts)


def build_argv(
    session_id: str, rules: str, model: str, claude_bin: str, stream_input: bool = False
) -> List[str]:
    # List form, never through a shell. The question body goes on stdin
    # because --tools is variadic and would swallow a trailing positional.
    return [
        claude_bin,
        "-p",
        "--resume", session_id,
        "--fork-session",
        "--no-session-persistence",
        "--model", model,
        *(
            # ★2026-10-05 with an image the question goes in as one stream-json
            # user message (image block + text block); output must then be
            # stream-json too. Verified by hand 10-05: the fork keeps the parent
            # context and reads the image, tools stay [].
            ["--input-format", "stream-json", "--output-format", "stream-json", "--verbose"]
            if stream_input
            else ["--output-format", "json"]
        ),
        "--tools", "",
        "--strict-mcp-config",
        "--settings", '{"disableAllHooks": true}',
        "--append-system-prompt", rules,
    ]


def answer(
    heard_text: str,
    device_system: str,
    robot_history: Any,
    extra_context: str = "",
    image_jpeg: Optional[bytes] = None,
    look: bool = False,
) -> CCResult:
    """Fork the window session once and return its answer.

    image_jpeg: when given, the photo is attached to the question (stream-json
    input). look: include the [[look]] rule in the speaking rules.

    Raises CCNoSession / CCTimeout / CCBadOutput (all CCBrainError).
    """
    model = os.environ.get("STACKCHAN_CC_MODEL", DEFAULT_MODEL).strip() or DEFAULT_MODEL
    timeout = _env_float("STACKCHAN_CC_TIMEOUT", DEFAULT_TIMEOUT_SECONDS)
    claude_bin = (
        os.environ.get("STACKCHAN_CLAUDE_BIN", DEFAULT_CLAUDE_BIN).strip()
        or DEFAULT_CLAUDE_BIN
    )

    session_id = find_window_session()
    if not session_id:
        raise CCNoSession("no recent window session")

    stream_input = image_jpeg is not None
    argv = build_argv(
        session_id, robot_rules(device_system, look=look), model, claude_bin, stream_input
    )
    question = question_text(heard_text, robot_history, extra_context)
    if stream_input:
        message = {
            "type": "user",
            "message": {
                "role": "user",
                "content": [
                    {
                        "type": "image",
                        "source": {
                            "type": "base64",
                            "media_type": "image/jpeg",
                            "data": base64.b64encode(image_jpeg).decode("ascii"),
                        },
                    },
                    {"type": "text", "text": question},
                ],
            },
        }
        stdin_bytes = (json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8")
    else:
        stdin_bytes = question.encode("utf-8")

    started = time.monotonic()
    deadline = started + max(timeout, 0.1)
    remaining = deadline - time.monotonic()
    if remaining <= 0 or not _FORK_LOCK.acquire(timeout=remaining):
        raise CCTimeout("another fork is still running")
    try:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise CCTimeout("lock wait used the whole timeout")
        try:
            proc = subprocess.run(
                argv,
                input=stdin_bytes,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=max(remaining, 0.1),
                cwd=CLAUDE_CWD,
            )
        except subprocess.TimeoutExpired:
            # subprocess.run kills the child before raising.
            raise CCTimeout("claude fork timed out")
    finally:
        _FORK_LOCK.release()
    latency_ms = int((time.monotonic() - started) * 1000)

    if proc.returncode != 0:
        # Short reason only: never stuff stderr into the exception or logs.
        raise CCBadOutput("claude exited with code {}".format(proc.returncode))
    try:
        if stream_input:
            payload = _last_stream_result(proc.stdout.decode("utf-8"))
        else:
            payload = json.loads(proc.stdout.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise CCBadOutput("stdout is not JSON")
    if not isinstance(payload, dict):
        raise CCBadOutput("stdout is not a JSON object")
    if payload.get("is_error"):
        raise CCBadOutput("is_error is true")
    reply = payload.get("result")
    if not isinstance(reply, str) or not reply.strip():
        raise CCBadOutput("empty result")
    cost = payload.get("total_cost_usd")
    raw_usage = payload.get("usage") if isinstance(payload.get("usage"), dict) else {}
    usage = {
        key: raw_usage.get(key)
        for key in (
            "input_tokens",
            "cache_creation_input_tokens",
            "cache_read_input_tokens",
            "output_tokens",
        )
    }
    return CCResult(
        reply=reply.strip(),
        session_id=payload.get("session_id") or session_id,
        latency_ms=latency_ms,
        cost_usd=float(cost) if isinstance(cost, (int, float)) else None,
        usage=usage,
    )


def _last_stream_result(text: str) -> Any:
    """stream-json output is one JSON object per line; the answer is the
    last line with type == "result". Raises ValueError when there is none."""
    found = None
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except ValueError:
            continue
        if isinstance(record, dict) and record.get("type") == "result":
            found = record
    if found is None:
        raise ValueError("no result line in stream-json output")
    return found


def robot_log_path() -> Path:
    override = os.environ.get("STACKCHAN_ROBOT_LOG", "").strip()
    if override:
        return Path(override).expanduser()
    return DEFAULT_ROBOT_LOG


def append_robot_log(
    heard: str,
    reply: str,
    brain: str,
    session_id: Optional[str],
    latency_ms: Optional[int],
    cost_usd: Optional[float],
    error: Optional[str],
    usage: Optional[Dict[str, Any]] = None,
) -> None:
    """Append one JSON line to the shared robot log.

    Never raises: a failed write must not break the reply to the device;
    it only leaves a warning in the gateway log.
    """
    record: Dict[str, Any] = {
        "ts": datetime.now().astimezone().isoformat(),
        "heard": heard or "",
        "reply": reply or "",
        "brain": brain,  # "cc" or "api-fallback"
        "session_id": session_id,
        "latency_ms": latency_ms,
        "cost_usd": cost_usd,
        "error": error,  # exception type name, None on success
        "usage": usage,  # token counts from the fork; None on fallback
    }
    try:
        path = robot_log_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, ensure_ascii=False) + "\n")
    except OSError as exc:
        LOGGER.warning("robot log write failed type=%s", type(exc).__name__)
