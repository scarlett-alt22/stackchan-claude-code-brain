#!/usr/bin/env python3
"""UserPromptSubmit hook: tell the window what happened on the robot.

Reads lines appended to robot-log.jsonl since the last run (the cursor is a
byte offset stored in .window-cursor; if the log shrank, read from the start).
Prints at most the 20 most recent rounds as plain text -- plain stdout on
UserPromptSubmit is added to Claude's context (official hooks reference).

Any exception is swallowed and the exit code is always 0: this hook must
never block the owner from sending a message in the window.

★2026-10-09 only interactive windows may take the rows. The hook is global, so
headless `claude -p` runs (muse-to-code bridge, reminders) fired it too and
advanced the cursor -- whatever the owner said to the robot went into a session the owner
never sees. Now: transcript entrypoint (the same signal cc_brain uses to pick
the robot's brain), else $CLAUDE_CODE_ENTRYPOINT; anything but "cli", or
unknown, exits without touching the cursor (the rows wait for a real window).
"""

import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import List, Optional, Tuple
from zoneinfo import ZoneInfo

DEFAULT_LOG = Path.home() / "StackChan" / "robot-log.jsonl"
DEFAULT_CURSOR = Path.home() / "StackChan" / ".window-cursor"
# ★2026-10-04 photo upload (code-to-muse 20261004-071145-stackchan-camera).
# Must match the gateway's DEFAULT_PHOTO_DIR; STACKCHAN_PHOTO_DIR overrides both.
DEFAULT_PHOTO_DIR = Path.home() / "StackChan" / "photos"
PHOTO_MAX_LISTED = 10
MAX_LISTED = 20
DISPLAY_LIMIT = 40


def _paths() -> Tuple[Path, Path]:
    log = os.environ.get("STACKCHAN_ROBOT_LOG", "").strip()
    cursor = os.environ.get("STACKCHAN_WINDOW_CURSOR", "").strip()
    return (
        Path(log).expanduser() if log else DEFAULT_LOG,
        Path(cursor).expanduser() if cursor else DEFAULT_CURSOR,
    )


def _photo_dir() -> Path:
    override = os.environ.get("STACKCHAN_PHOTO_DIR", "").strip()
    return Path(override).expanduser() if override else DEFAULT_PHOTO_DIR


def _short(text: object) -> str:
    value = str(text or "").strip().replace("\n", " ")
    if len(value) <= DISPLAY_LIMIT:
        return value
    return value[:DISPLAY_LIMIT] + "…"


def _when(timestamp: object) -> str:
    try:
        moment = datetime.fromisoformat(str(timestamp))
    except (TypeError, ValueError):
        return "??:??"
    return moment.astimezone().strftime("%m-%d %H:%M")


def read_new_rows(log_path: Path, cursor_path: Path) -> List[dict]:
    """Return new log rows and advance the cursor. Never raises."""
    try:
        size = log_path.stat().st_size
    except OSError:
        return []  # no log yet: nothing to report
    try:
        offset = int((cursor_path.read_text(encoding="utf-8") or "").strip() or "0")
    except (OSError, ValueError):
        offset = 0
    if offset < 0 or offset > size:
        offset = 0  # log was truncated or rotated: read from the start
    try:
        with log_path.open("rb") as handle:
            handle.seek(offset)
            chunk = handle.read()
    except OSError:
        return []
    rows: List[dict] = []
    for raw in chunk.split(b"\n"):
        raw = raw.strip()
        if not raw:
            continue
        try:
            row = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            continue
        if isinstance(row, dict):
            rows.append(row)
    try:
        cursor_path.parent.mkdir(parents=True, exist_ok=True)
        cursor_path.write_text(str(offset + len(chunk)), encoding="utf-8")
    except OSError:
        pass
    return rows


def render(rows: List[dict]) -> str:
    photo_rows = [row for row in rows if row.get("type") == "photo"]
    chat_rows = [row for row in rows if row.get("type") != "photo"]
    sections: List[str] = []
    if photo_rows:
        sections.append(render_photos(photo_rows))
    if chat_rows:
        sections.append(render_chats(chat_rows))
    return "\n".join(sections)


def render_photos(photo_rows: List[dict]) -> str:
    """Summarize {"type": "photo"} rows: count plus absolute paths.

    The log stores paths relative to the photo dir; the window wants
    absolute paths it can open. Lists at most PHOTO_MAX_LISTED, then
    only the count of the rest."""
    base = _photo_dir()
    listed = photo_rows[-PHOTO_MAX_LISTED:]
    older = len(photo_rows) - len(listed)
    lines = ["[StackChan] 拍了 {} 张照片：".format(len(photo_rows))]
    for row in listed:
        rel = str(row.get("path") or "")
        absolute = str(base / rel) if rel else "?"
        lines.append("- {}".format(absolute))
    if older:
        lines.append("更早还有 {} 张未列出。".format(older))
    return "\n".join(lines)


def render_chats(rows: List[dict]) -> str:
    listed = rows[-MAX_LISTED:]
    older = len(rows) - len(listed)
    lines = [
        "[StackChan] 自上次以来你在机器人上和主人说过 {} 轮：".format(len(rows))
    ]
    for row in listed:
        heard = _short(row.get("heard"))
        reply = _short(row.get("reply"))
        tag = "（fallback，不是你答的）" if row.get("brain") == "api-fallback" else ""
        lines.append(
            "- {} 主人：「{}」 你：「{}」{}".format(
                _when(row.get("ts")), heard, reply, tag
            )
        )
    if older:
        lines.append("更早还有 {} 轮未列出。".format(older))
    return "\n".join(lines)


def _entrypoint_from_transcript(path: Path, max_lines: int = 50) -> Optional[str]:
    """First `entrypoint` value in a session transcript, or None.
    Same rule as cc_brain._session_entrypoint."""
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


def session_entrypoint(hook_input: str) -> Optional[str]:
    """Entrypoint of the session that fired the hook, or None when unknown."""
    try:
        payload = json.loads(hook_input or "{}")
    except ValueError:
        payload = {}
    transcript = payload.get("transcript_path") if isinstance(payload, dict) else None
    if transcript:
        found = _entrypoint_from_transcript(Path(str(transcript)).expanduser())
        if found:
            return found
    env = os.environ.get("CLAUDE_CODE_ENTRYPOINT", "").strip()
    return env or None


def main() -> int:
    try:
        try:
            hook_input = sys.stdin.read()
        except (OSError, ValueError):
            hook_input = ""
        if session_entrypoint(hook_input) != "cli":
            return 0  # not the owner's window: leave the rows for one that is
        log_path, cursor_path = _paths()
        rows = read_new_rows(log_path, cursor_path)
        if rows:
            sys.stdout.write(render(rows) + "\n")
            sys.stdout.flush()
        return 0
    except Exception:
        # Swallow everything: never block the owner's prompt.
        return 0


if __name__ == "__main__":
    sys.exit(main())
