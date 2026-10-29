#!/usr/bin/env python3
"""本机 SenseVoice STT server —— 跟 Whisper 版（stt_whisper_server.py）同一个接口，给机器人当"耳朵"。

为什么换：Whisper 在没人说话时会编整句（实测静音 →「请不吝点赞 订阅 转发…」这类字幕腔），
  而且只给文字、不给语气。SenseVoice（FunAudioLLM 官方 2024-07-17 int8，经 sherpa-onnx）
  同一次识别还给出语种 / 情绪 / 声音事件，5.6s 中文约 0.07s 识别完。
  前面再加 silero VAD：判定没人说话就直接回空，不让识别模型去猜。

接口跟 stt_whisper_server.py 一致：POST /v1/audio/transcriptions（multipart: file / 可选 language），
回 {"text": ...}，另外多带 lang / emotion / event / speech_sec 几个字段——网关和设备只读 text，
多出来的字段不影响旧链路。换后端＝改网关的 STACKCHAN_STT_UPSTREAM_URL，网关代码不用动。

🔴 只绑 127.0.0.1：没有鉴权（鉴权在网关那层）。别改成 0.0.0.0。
⚠️ 模型目录用 2024-07-17 那版；2025-09-09 那版是粤语微调（WSYue），会把普通话判成粤语。
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
from pathlib import Path

import numpy as np
import sherpa_onnx

HERE = Path(__file__).resolve().parent
HOST = os.environ.get("STT_HOST", "127.0.0.1")
PORT = int(os.environ.get("STT_PORT", "8127"))
MODEL_DIR = Path(os.environ.get(
    "SV_MODEL_DIR", HERE / "models" / "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17"))
VAD_MODEL = Path(os.environ.get("SV_VAD_MODEL", HERE / "models" / "silero_vad.onnx"))
MIN_SPEECH_SEC = float(os.environ.get("SV_MIN_SPEECH_SEC", "0.3"))
TARGET_SR = 16000

_lock = threading.Lock()          # 识别器串行用，别并发 decode
_ready = threading.Event()
_recognizer = None


def log(msg: str) -> None:
    print(f"[sv{PORT}] {msg}", flush=True)


# --------------------------------------------------------------------------
#  音频解码 / multipart —— 照抄 stt_server.py，设备发 16kHz/16bit/mono WAV
# --------------------------------------------------------------------------
def decode_audio(raw: bytes) -> np.ndarray:
    try:
        w = wave.open(io.BytesIO(raw), "rb")
        sr, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        frames = w.readframes(w.getnframes())
    except Exception:
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


def parse_multipart(body: bytes, content_type: str) -> dict:
    m = re.search(r"boundary=([^;]+)", content_type or "", re.I)
    if not m:
        raise ValueError("missing multipart boundary")
    boundary = b"--" + m.group(1).strip().strip('"').encode("latin1")

    # 分隔符只认「CRLF + --boundary」（RFC 2046），前面补一个 CRLF 好让第一条也对得上。
    # 原来（照抄 stt_server.py）按裸 boundary 切、再 rstrip(b"\r\n")，有两个毛病，
    # Muse 10-27 复核用测试证实：①音频数据里碰巧出现 --boundary 会被切坏；
    # ②WAV 末尾字节是 0x0a/0x0d 会被一起吃掉，单个 0x0a 让长度变奇数→decode 报错→400。
    # 按 CRLF+boundary 切之后，分隔符前的那个 CRLF 已经属于分隔符，payload 不用再剥。
    fields = {}
    for part in (b"\r\n" + body).split(b"\r\n" + boundary):
        if not part.startswith(b"\r\n"):       # 前导杂质，或结尾的 "--" 收尾
            continue
        head, sep, payload = part[2:].partition(b"\r\n\r\n")
        if not sep:
            continue
        name = re.search(rb'name="([^"]*)"', head)
        if not name:
            continue
        fields[name.group(1).decode("latin1")] = payload
    return fields


# --------------------------------------------------------------------------
#  VAD + 识别
# --------------------------------------------------------------------------
def speech_seconds(audio: np.ndarray) -> float:
    """silero VAD 判出的说话总时长。每次新建一个 VAD，避免跨请求残留状态。"""
    cfg = sherpa_onnx.VadModelConfig()
    cfg.silero_vad.model = str(VAD_MODEL)
    cfg.silero_vad.min_silence_duration = 0.25
    cfg.silero_vad.min_speech_duration = 0.15
    cfg.sample_rate = TARGET_SR
    vad = sherpa_onnx.VoiceActivityDetector(cfg, buffer_size_in_seconds=60)
    win = cfg.silero_vad.window_size
    for i in range(0, len(audio), win):
        vad.accept_waveform(audio[i:i + win])
    vad.flush()
    total = 0
    while not vad.empty():
        total += len(vad.front.samples)
        vad.pop()
    return total / TARGET_SR


def _tag(s: str) -> str:
    """'<|HAPPY|>' -> 'HAPPY'"""
    return (s or "").strip().removeprefix("<|").removesuffix("|>")


def transcribe(audio: np.ndarray) -> dict:
    speech = speech_seconds(audio)
    if speech < MIN_SPEECH_SEC:
        return {"text": "", "lang": "", "emotion": "", "event": "no_speech",
                "speech_sec": round(speech, 2)}
    with _lock:
        stream = _recognizer.create_stream()
        stream.accept_waveform(TARGET_SR, audio)
        _recognizer.decode_stream(stream)
        r = stream.result
    return {"text": (r.text or "").strip(), "lang": _tag(r.lang),
            "emotion": _tag(r.emotion), "event": _tag(r.event),
            "speech_sec": round(speech, 2)}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
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
                             "model": MODEL_DIR.name, "port": PORT})
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
            audio = decode_audio(raw)
            result = transcribe(audio)
        except Exception as exc:
            log(f"! {type(exc).__name__}: {exc}")
            self._json(400, {"error": f"{type(exc).__name__}: {exc}"})
            return

        dur = len(audio) / TARGET_SR
        log(f"{dur:.2f}s audio -> {time.time()-t0:.2f}s  speech={result['speech_sec']}s "
            f"{result['lang']}/{result['emotion']}/{result['event']}  {result['text']!r}")
        self._json(200, result)


def load() -> None:
    global _recognizer
    _recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
        model=str(MODEL_DIR / "model.int8.onnx"),
        tokens=str(MODEL_DIR / "tokens.txt"),
        use_itn=True,
        num_threads=4,
    )


def main() -> int:
    log(f"loading {MODEL_DIR.name} + {VAD_MODEL.name} ...")
    t0 = time.time()
    load()
    _ready.set()
    log(f"model ready in {time.time()-t0:.1f}s; listening on {HOST}:{PORT}")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
