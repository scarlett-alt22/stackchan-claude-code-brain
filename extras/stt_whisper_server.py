#!/usr/bin/env python3
"""Local STT server: OpenAI-compatible POST /v1/audio/transcriptions for the robot.

Runs whisper-large-v3-turbo on Apple Silicon via mlx-whisper.

Why local instead of a cloud STT API (measured): a 3 s recording took 7.2 s on
the cloud API we used before, and short Chinese phrases sometimes came back with
invented English tails. Locally (warm): 0.78 s audio -> 0.94 s, 4.83 s audio ->
1.13 s, both transcribed correctly.

The gateway only forwards to this server; switching STT backends means changing
STACKCHAN_STT_UPSTREAM_URL in the gateway's environment, not its code.

Bind to 127.0.0.1 only: this server has no auth (auth lives in the gateway).
The model stays loaded: cold start 3.68 s vs warm 0.94 s.

Setup:
    python3 -m venv .venv && .venv/bin/pip install mlx-whisper numpy
    .venv/bin/python stt_whisper_server.py
Env: STT_HOST (127.0.0.1), STT_PORT (8126), STT_MODEL, STT_LANG (zh).
"""

import io
import json
import os
import re
import sys
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np

import mlx_whisper

HOST = os.environ.get("STT_HOST", "127.0.0.1")
PORT = int(os.environ.get("STT_PORT", "8126"))
MODEL = os.environ.get("STT_MODEL", "mlx-community/whisper-large-v3-turbo")
DEFAULT_LANG = os.environ.get("STT_LANG", "zh")
TARGET_SR = 16000

_lock = threading.Lock()          # MLX 不是线程安全的, 串行化推理
_ready = threading.Event()


def log(msg: str) -> None:
    print(f"[stt{PORT}] {msg}", flush=True)


# --------------------------------------------------------------------------
#  音频解码 —— 故意不依赖 ffmpeg
#  设备发的是 16kHz/16bit/mono linear PCM WAV(firmware/src/driver/Audio.h),
#  正好是 whisper 的原生采样率, 常规路径下连重采样都不用走。
# --------------------------------------------------------------------------
def decode_audio(raw: bytes) -> np.ndarray:
    try:
        w = wave.open(io.BytesIO(raw), "rb")
        sr, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        frames = w.readframes(w.getnframes())
    except Exception:
        # WAV 头坏了/根本不是 WAV -> 按设备的既定格式当裸 PCM 收(跳过 44 字节头)
        log("! wav header unreadable, falling back to raw 16k/16bit/mono PCM")
        sr, ch, width = TARGET_SR, 1, 2
        frames = raw[44:] if len(raw) > 44 else raw

    if width != 2:
        raise ValueError(f"unsupported sample width: {width}")

    x = np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0
    if ch == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    if sr != TARGET_SR:
        n = int(len(x) / sr * TARGET_SR)
        x = np.interp(
            np.linspace(0, len(x) / sr, n, endpoint=False),
            np.arange(len(x)) / sr,
            x,
        ).astype(np.float32)
    return x


# --------------------------------------------------------------------------
#  multipart 解析 —— 只认设备真正会发的那几个字段
#  设备的 body(firmware/src/stt/Whisper.cpp): model / 可选 language / file
# --------------------------------------------------------------------------
def parse_multipart(body: bytes, content_type: str) -> dict:
    m = re.search(r"boundary=([^;]+)", content_type or "", re.I)
    if not m:
        raise ValueError("missing multipart boundary")
    boundary = b"--" + m.group(1).strip().strip('"').encode("latin1")

    fields = {}
    for part in body.split(boundary):
        if not part or part in (b"--\r\n", b"--", b"\r\n"):
            continue
        head, _, payload = part.partition(b"\r\n\r\n")
        if not _:
            continue
        name = re.search(rb'name="([^"]*)"', head)
        if not name:
            continue
        fields[name.group(1).decode("latin1")] = payload.rstrip(b"\r\n")
    return fields


def transcribe(audio: np.ndarray, language: str) -> str:
    with _lock:
        result = mlx_whisper.transcribe(
            audio, path_or_hf_repo=MODEL, language=language, fp16=True
        )
    return (result.get("text") or "").strip()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):        # 默认那行访问日志太吵, 自己打有用的
        pass

    def _json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/health"):
            self._json(200, {"status": "ok" if _ready.is_set() else "loading",
                             "model": MODEL, "port": PORT})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        if not self.path.startswith("/v1/audio/transcriptions"):
            self._json(404, {"error": "not found"})
            return
        if not _ready.is_set():
            self._json(503, {"error": "model still loading"})
            return

        length = int(self.headers.get("Content-Length") or 0)
        body = b""
        while len(body) < length:
            chunk = self.rfile.read(min(65536, length - len(body)))
            if not chunk:
                break
            body += chunk

        t0 = time.time()
        try:
            fields = parse_multipart(body, self.headers.get("Content-Type", ""))
            raw = fields.get("file")
            if not raw:
                raise ValueError("no file field")
            lang = (fields.get("language") or b"").decode("utf-8", "replace").strip()
            audio = decode_audio(raw)
            text = transcribe(audio, lang or DEFAULT_LANG)
        except Exception as exc:
            log(f"! {type(exc).__name__}: {exc}")
            self._json(400, {"error": f"{type(exc).__name__}: {exc}"})
            return

        dur = len(audio) / TARGET_SR
        log(f"{dur:.2f}s audio -> {time.time()-t0:.2f}s  {text!r}")
        self._json(200, {"text": text})


def main() -> int:
    log(f"loading {MODEL} ...")
    t0 = time.time()
    # 拿 0.5s 静音跑一次, 把权重和 kernel 都预热掉, 别让第一句话付冷启动
    mlx_whisper.transcribe(
        np.zeros(TARGET_SR // 2, dtype=np.float32),
        path_or_hf_repo=MODEL, language=DEFAULT_LANG, fp16=True,
    )
    _ready.set()
    log(f"model ready in {time.time()-t0:.1f}s; listening on {HOST}:{PORT}")

    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
