#!/usr/bin/env python3
"""MiniMax TTS server — OpenAI-compatible /v1/audio/speech for StackChan.

Draft v2 (2026-10-04), derived from tts_minimax_server.orig.py (read-only,
never modified). Python 3.12, stdlib only.

Device-side hard constraints (verified by Claude Code, encoded here):
  - The device reads a raw TCP stream and does NOT decode chunked encoding.
    Responses must carry Content-Length, or be close-delimited
    (Connection: close, no Content-Length, no chunked).
  - The device plays WAV (24000 Hz / mono / 16-bit) and MP3: request JSON
    field "response_format" == "mp3" (case-insensitive) selects MP3,
    anything else (or missing) selects WAV. In WAV streaming mode the WAV
    header's RIFF size and data size are 0xFFFFFFFF; the device plays
    until the connection closes.
  - ~1.1 s underrun tolerance: once audio starts, a gap longer than ~1 s
    makes the device disconnect. Streaming therefore buffers a headstart
    and fills real stalls with silence frames (a small pause, never a
    disconnect). The device waits up to ~65 s for response headers.
  - The device sends OpenAI-compatible POST /v1/audio/speech with the
    text in JSON field "input".

Env:
  TTS_PORT                (default 8125)
  MINIMAX_API_KEY         (required)
  MINIMAX_VOICE_ID        (required)
  MINIMAX_MODEL           (default speech-2.8-hd)
  MINIMAX_HOST            (default https://api.minimax.io)
  TTS_OUTPUT_GAIN         (default 1.0)  int16 PCM gain, clipped
                                         (ignored on the mp3 path: warn once)
  MINIMAX_STREAM          (0/1, default 0)
  MINIMAX_HEADSTART_MS    (default 300)  ms of audio buffered before
                                        streaming starts
  MINIMAX_RETRY_TIMEOUT_S (default 60)   total MiniMax budget, incl. one retry
  MINIMAX_MP3_BITRATE     (default 64000)  mp3 path only; allowed values:
                                        32000/64000/128000/256000
                                        (illegal value -> 64000 + log)

Error policy: one retry on transient errors (timeout / 5xx / connection
failure); final failure -> 502. If streaming already started, a short
silence tail is sent before closing.
"""

import json
import os
import queue
import socket
import socketserver
import struct
import threading
import time
import urllib.error
import urllib.request


# ------------------------------------------------------------- config

def _getenv(name, default):
    v = os.environ.get(name, default)
    return v.strip() if isinstance(v, str) else v


PORT = int(_getenv("TTS_PORT", "8125"))
API_KEY = _getenv("MINIMAX_API_KEY", "")
VOICE_ID = _getenv("MINIMAX_VOICE_ID", "")
MODEL = _getenv("MINIMAX_MODEL", "speech-2.8-hd")
HOST = _getenv("MINIMAX_HOST", "https://api.minimax.io").rstrip("/")
GAIN = float(_getenv("TTS_OUTPUT_GAIN", "1.0"))
STREAM = _getenv("MINIMAX_STREAM", "0") == "1"
HEADSTART_MS = int(_getenv("MINIMAX_HEADSTART_MS", "300"))
RETRY_TIMEOUT_S = float(_getenv("MINIMAX_RETRY_TIMEOUT_S", "60"))

_ALLOWED_MP3_BITRATES = (32000, 64000, 128000, 256000)

SAMPLE_RATE = 24000
CHANNELS = 1
BITS = 16
BYTES_PER_MS = SAMPLE_RATE * CHANNELS * BITS // 8 // 1000  # 48 bytes per ms
_SILENCE_SLICE_MS = 100

LOG_TAG = f"minimax-tts:{PORT}"


def log(msg):
    # Never log API_KEY or Authorization here or anywhere below.
    print(f"[{LOG_TAG}] {msg}", flush=True)


def sanitize(text):
    return "".join(c for c in text.strip() if ord(c) >= 32 or c in "\n\t").strip()


def _mp3_bitrate():
    raw = _getenv("MINIMAX_MP3_BITRATE", "64000")
    try:
        v = int(raw)
    except (TypeError, ValueError):
        v = None
    if v not in _ALLOWED_MP3_BITRATES:
        log(f"invalid MINIMAX_MP3_BITRATE={raw!r}, falling back to 64000")
        return 64000
    return v


MP3_BITRATE = _mp3_bitrate()


_mp3_gain_warned = False


def warn_mp3_gain_once():
    """The mp3 path never applies TTS_OUTPUT_GAIN; warn once per process.

    Only warns when gain is actually set to something != 1.0.
    """
    global _mp3_gain_warned
    if GAIN == 1.0 or _mp3_gain_warned:
        return
    _mp3_gain_warned = True
    log(f"warning: TTS_OUTPUT_GAIN={GAIN} ignored for mp3 output")


# ------------------------------------------------------------- errors

class MiniMaxError(Exception):
    """MiniMax API-level failure: do not retry."""


class TransientError(Exception):
    """Timeout / HTTP 5xx / connection failure: one retry allowed."""


# ------------------------------------------------------------- MiniMax client

def _payload(text, stream):
    # NOTE: format="pcm" under streaming is NOT confirmed by official docs.
    # Official docs only promise hex output for streams; whether pcm is
    # accepted and whether each chunk's hex is incremental audio is
    # unverified — the real-API probe must validate before MINIMAX_STREAM=1.
    return {
        "model": MODEL,
        "text": text,
        "stream": stream,
        "language_boost": "Chinese",
        "voice_setting": {"voice_id": VOICE_ID, "speed": 1.0, "vol": 1.0,
                          "pitch": 0},
        "audio_setting": {"sample_rate": SAMPLE_RATE, "bitrate": 128000,
                          "format": "pcm" if stream else "wav",
                          "channel": CHANNELS},
    }


def _payload_mp3(text, stream):
    """MiniMax request for the mp3 path (client response_format == mp3).

    Same as _payload except audio_setting: format mp3, bitrate from
    MINIMAX_MP3_BITRATE (validated at startup), sample_rate unchanged.
    """
    p = _payload(text, stream)
    p["audio_setting"] = {"sample_rate": SAMPLE_RATE, "bitrate": MP3_BITRATE,
                          "format": "mp3", "channel": CHANNELS}
    return p


def _open(payload, deadline):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        HOST + "/v1/t2a_v2", data=data,
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + API_KEY})
    timeout = max(1.0, deadline - time.time())
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        if 500 <= e.code < 600:
            raise TransientError(f"HTTP {e.code}") from e
        raise MiniMaxError(f"HTTP {e.code}") from e
    except (urllib.error.URLError, TimeoutError, socket.timeout,
            ConnectionError, OSError) as e:
        raise TransientError(f"{type(e).__name__}: {e}") from e


def _call_with_retry(payload, deadline):
    """Open the MiniMax request; retry once on transient errors."""
    last = None
    for attempt in (1, 2):
        try:
            return _open(payload, deadline)
        except TransientError as e:
            last = e
            if attempt == 1 and time.time() < deadline:
                log(f"minimax transient error ({e}), retrying once")
                continue
            raise
    raise last  # pragma: no cover — loop always returns or raises


# ------------------------------------------------------------- WAV helpers

def extract_pcm(wav):
    """Return (sample_rate, channels, bits_per_sample, pcm_bytes).

    Walks the RIFF chunks to find 'data' instead of assuming a 44-byte
    header, so extra chunks (LIST, bext, ...) don't break parsing.
    """
    if len(wav) < 12 or wav[0:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    fmt = None
    pcm = None
    pos = 12
    while pos + 8 <= len(wav):
        cid, csize = struct.unpack("<4sI", wav[pos:pos + 8])
        body = wav[pos + 8:pos + 8 + csize]
        if cid == b"fmt " and len(body) >= 16:
            a_fmt, ch, sr, _br, _ba, bits = struct.unpack("<HHIIHH", body[:16])
            if a_fmt != 1:
                raise ValueError(f"unsupported WAV encoding {a_fmt}, need PCM")
            fmt = (sr, ch, bits)
        elif cid == b"data":
            pcm = bytes(body)
        pos += 8 + csize + (csize & 1)  # chunks are word-aligned
    if fmt is None:
        raise ValueError("WAV has no fmt chunk")
    if pcm is None:
        raise ValueError("WAV has no data chunk")
    return fmt[0], fmt[1], fmt[2], pcm


def wav_header(data_len, stream, sample_rate=SAMPLE_RATE,
               channels=CHANNELS, bits=BITS):
    """Canonical 44-byte WAV header. Streaming uses 0xFFFFFFFF sizes."""
    riff_size = 0xFFFFFFFF if stream else 36 + data_len
    data_size = 0xFFFFFFFF if stream else data_len
    byte_rate = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    return struct.pack("<4sI4s4sIHHIIHH4sI",
                       b"RIFF", riff_size, b"WAVE",
                       b"fmt ", 16, 1, channels, sample_rate,
                       byte_rate, block_align, bits,
                       b"data", data_size)


def apply_gain(pcm, gain):
    """Multiply int16 PCM by gain, clipping to [-32768, 32767]."""
    if gain == 1.0:
        return pcm
    n = len(pcm) // 2
    out = bytearray(n * 2)
    for i in range(n):
        (s,) = struct.unpack_from("<h", pcm, i * 2)
        v = int(s * gain)
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        struct.pack_into("<h", out, i * 2, v)
    return bytes(out) + pcm[n * 2:]


# ------------------------------------------------------------- non-streaming

def minimax_synthesize(text):
    """Return (wav_bytes, stats); raise MiniMaxError/TransientError."""
    deadline = time.time() + RETRY_TIMEOUT_S
    t0 = time.time()
    with _call_with_retry(_payload(text, False), deadline) as resp:
        obj = json.loads(resp.read().decode("utf-8"))
    base = obj.get("base_resp", {})
    if base.get("status_code", 0) != 0:
        raise MiniMaxError(f"minimax error: {base}")
    data = obj.get("data", {})
    if data.get("status") != 2:
        raise MiniMaxError(f"minimax incomplete: status={data.get('status')}")
    try:
        wav = bytes.fromhex(data.get("audio", ""))
    except (ValueError, TypeError) as e:
        raise MiniMaxError(f"bad audio hex: {e}") from e
    sr, ch, bits, pcm = extract_pcm(wav)
    pcm = apply_gain(pcm, GAIN)
    out = wav_header(len(pcm), stream=False,
                     sample_rate=sr, channels=ch, bits=bits) + pcm
    stats = {"t_synth": round(time.time() - t0, 2), "bytes": len(out),
             "usage_chars": obj.get("extra_info", {}).get("usage_characters")}
    return out, stats


def minimax_synthesize_mp3(text):
    """Return (mp3_bytes, stats); raise MiniMaxError/TransientError.

    MP3 path: MiniMax's bytes are returned untouched — no WAV header,
    no gain. (Gain on the mp3 path is not applicable; see
    warn_mp3_gain_once.)
    """
    deadline = time.time() + RETRY_TIMEOUT_S
    t0 = time.time()
    with _call_with_retry(_payload_mp3(text, False), deadline) as resp:
        obj = json.loads(resp.read().decode("utf-8"))
    base = obj.get("base_resp", {})
    if base.get("status_code", 0) != 0:
        raise MiniMaxError(f"minimax error: {base}")
    data = obj.get("data", {})
    if data.get("status") != 2:
        raise MiniMaxError(f"minimax incomplete: status={data.get('status')}")
    try:
        mp3 = bytes.fromhex(data.get("audio", ""))
    except (ValueError, TypeError) as e:
        raise MiniMaxError(f"bad audio hex: {e}") from e
    stats = {"t_synth": round(time.time() - t0, 2), "bytes": len(mp3),
             "usage_chars": obj.get("extra_info", {}).get("usage_characters")}
    return mp3, stats


# ------------------------------------------------------------- streaming

def _stream_producer(text, q, deadline, payload=None):
    """Read MiniMax SSE in a thread.

    Puts ("audio", audio_bytes) / ("error", message) / ("done", None).
    Retries once on transient errors only if no audio arrived yet —
    once streaming has started we never retry (the device is already fed).

    payload defaults to the WAV-path request; the mp3 path passes its own.
    Chunk handling is format-agnostic: status=1 chunks are hex-decoded and
    forwarded; the status=2 tail (real-API behavior: the whole audio
    repeated) is discarded once any incremental chunk arrived.
    """
    if payload is None:
        payload = _payload(text, True)
    chunks_seen = 0
    last = None
    for attempt in (1, 2):
        try:
            with _open(payload, deadline) as resp:
                for raw in resp:
                    line = raw.decode("utf-8", "replace").strip()
                    if not line.startswith("data:"):
                        continue
                    body = line[5:].strip()
                    if body == "[DONE]":
                        break
                    try:
                        obj = json.loads(body)
                    except ValueError as e:
                        q.put(("error", f"bad stream JSON: {e}"))
                        return
                    base = obj.get("base_resp", {})
                    if base.get("status_code", 0) != 0:
                        q.put(("error", f"minimax error: {base}"))
                        return
                    data = obj.get("data", {})
                    audio_hex = data.get("audio") or ""
                    # 真接口实测（2026-10-04）：status=2 尾包把整段音频再发一遍
                    # （== 之前所有增量包拼接）。已收到过增量包就丢掉它，否则
                    # 设备会把整句念两遍；一个增量包都没有时才拿它兜底。
                    if data.get("status") == 2 and chunks_seen > 0:
                        audio_hex = ""
                    if audio_hex:
                        try:
                            chunk = bytes.fromhex(audio_hex)
                        except ValueError as e:
                            q.put(("error", f"bad audio hex: {e}"))
                            return
                        chunks_seen += 1
                        q.put(("audio", chunk))
                    if data.get("status") == 2:
                        break
            q.put(("done", None))
            return
        except TransientError as e:
            last = e
            if attempt == 1 and chunks_seen == 0 and time.time() < deadline:
                log(f"minimax stream transient error ({e}), retrying once")
                continue
            break
    q.put(("error", f"transient failure: {last}"))


def _wait_chunk_fill_silence(sock, q, deadline):
    """Wait for the next producer item; while waiting, emit 100 ms silence
    slices so the device (~1.1 s underrun tolerance) never starves.
    Returns ("audio"/"done"/"error", payload), or ("silence", nbytes) after
    a fill, or None when the deadline is exceeded."""
    remaining = deadline - time.time()
    if remaining <= 0:
        return None
    try:
        return q.get(timeout=min(_SILENCE_SLICE_MS / 1000.0, remaining))
    except queue.Empty:
        n = _SILENCE_SLICE_MS * BYTES_PER_MS
        try:
            sock.sendall(b"\x00" * n)
        except OSError:
            return ("error", "device disconnected")
        return ("silence", n)


def serve_streaming(sock, text):
    t0 = time.time()
    deadline = t0 + RETRY_TIMEOUT_S
    q = queue.Queue()
    prod = threading.Thread(target=_stream_producer, args=(text, q, deadline),
                            daemon=True)
    prod.start()

    # 1) wait for the first audio chunk (or a hard failure)
    try:
        kind, payload = q.get(timeout=max(1.0, deadline - time.time()))
    except queue.Empty:
        kind, payload = "error", "no audio from MiniMax in time"
    if kind != "audio":
        log(f"stream setup failed: {payload}")
        sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n")
        return
    t_first = round(time.time() - t0, 2)

    # 2) accumulate the headstart before the device hears anything
    headstart_bytes = HEADSTART_MS * BYTES_PER_MS
    buffered = bytearray(payload)
    stream_done = False
    stream_error = None
    while len(buffered) < headstart_bytes and not stream_done \
            and stream_error is None:
        try:
            kind, payload = q.get(timeout=max(0.5, deadline - time.time()))
        except queue.Empty:
            kind, payload = "error", "headstart timeout"
        if kind == "audio":
            buffered += payload
        elif kind == "done":
            stream_done = True
        else:
            stream_error = payload
    if stream_error is not None:
        log(f"stream setup failed: {stream_error}")
        sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n")
        return

    # 3) commit: close-delimited 200 + streaming WAV header, then audio
    try:
        sock.sendall(b"HTTP/1.1 200 OK\r\n"
                      b"Content-Type: audio/wav\r\n"
                      b"Connection: close\r\n\r\n"
                      + wav_header(0, stream=True))
        sock.sendall(apply_gain(bytes(buffered), GAIN))
    except OSError:
        return
    sent_pcm = len(buffered)

    # 4) forward chunks as they arrive, filling stalls with silence
    error = None
    sent_total = 44 + sent_pcm  # header + headstart audio
    while not stream_done and error is None:
        item = _wait_chunk_fill_silence(sock, q, deadline)
        if item is None:  # total budget exceeded
            error = "retry budget exceeded"
            break
        kind, payload = item
        if kind == "silence":
            sent_total += payload
        elif kind == "audio":
            try:
                sock.sendall(apply_gain(payload, GAIN))
            except OSError:
                return
            sent_pcm += len(payload)
            sent_total += len(payload)
        elif kind == "done":
            stream_done = True
        else:
            error = payload

    if error is not None:
        # Streaming already started: never leave the device hanging —
        # a short silence tail, then close.
        log(f"stream aborted mid-way ({error}); silence tail, closing")
        try:
            sock.sendall(b"\x00" * (200 * BYTES_PER_MS))
        except OSError:
            pass
    log(f"ok stream text={text[:80]!r} t_first={t_first}s "
        f"t_total={round(time.time() - t0, 2)}s bytes={sent_total}")


def serve_streaming_mp3(sock, text):
    """Stream MP3 to the device (client response_format == mp3).

    - No WAV header, no headstart buffering, no silence fill: each
      status=1 chunk is hex-decoded and forwarded immediately with the
      socket write (MP3 frames are self-delimiting). The status=2 tail
      (whole audio repeated) is discarded by _stream_producer, same as the
      WAV path.
    - Close-delimited (no Content-Length, no chunked): the device reads
      raw TCP until close.
    - TTS_OUTPUT_GAIN is never applied on the mp3 path (see
      warn_mp3_gain_once). If the upstream fails mid-stream there is no
      PCM silence tail to send — we just log and close.
    """
    warn_mp3_gain_once()
    t0 = time.time()
    deadline = t0 + RETRY_TIMEOUT_S
    q = queue.Queue()
    prod = threading.Thread(target=_stream_producer,
                            args=(text, q, deadline, _payload_mp3(text, True)),
                            daemon=True)
    prod.start()

    # Wait for the first audio chunk (or a hard failure), like the WAV
    # path's step 1 — but then commit immediately: no headstart.
    try:
        kind, payload = q.get(timeout=max(1.0, deadline - time.time()))
    except queue.Empty:
        kind, payload = "error", "no audio from MiniMax in time"
    if kind != "audio":
        log(f"mp3 stream setup failed: {payload}")
        sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n")
        return
    t_first = round(time.time() - t0, 2)

    try:
        sock.sendall(b"HTTP/1.1 200 OK\r\n"
                      b"Content-Type: audio/mpeg\r\n"
                      b"Connection: close\r\n\r\n" + payload)
    except OSError:
        return
    sent = len(payload)

    # Forward chunks as they arrive; no silence fill for mp3 (PCM zeros
    # would corrupt an MP3 stream).
    error = None
    done = False
    while not done and error is None:
        remaining = deadline - time.time()
        if remaining <= 0:
            error = "retry budget exceeded"
            break
        try:
            kind, payload = q.get(timeout=remaining)
        except queue.Empty:
            error = "retry budget exceeded"
            break
        if kind == "audio":
            try:
                sock.sendall(payload)
            except OSError:
                return
            sent += len(payload)
        elif kind == "done":
            done = True
        else:
            error = payload

    if error is not None:
        log(f"mp3 stream aborted mid-way ({error}); closing")
    log(f"ok mp3-stream text={text[:80]!r} t_first={t_first}s "
        f"t_total={round(time.time() - t0, 2)}s bytes={sent}")


# ------------------------------------------------------------- HTTP server

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        sock.settimeout(70)
        try:
            head, rest = self._read_headers(sock)
        except (OSError, ValueError):
            return
        if head is None:
            return
        line = head.split("\r\n", 1)[0]
        try:
            method, path, _ = line.split(" ", 2)
        except ValueError:
            return

        if path.startswith("/health"):
            body = json.dumps({
                "status": "ok", "backend": "minimax",
                "host": HOST, "model": MODEL, "voice_id_set": bool(VOICE_ID),
                "gain": GAIN, "stream": STREAM,
                "headstart_ms": HEADSTART_MS, "port": PORT}).encode()
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         b"Content-Length: " + str(len(body)).encode() +
                         b"\r\nConnection: close\r\n\r\n" + body)
            return

        if method != "POST" or not path.startswith("/v1/audio/speech"):
            sock.sendall(b"HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n")
            return

        clen = 0
        for h in head.split("\r\n")[1:]:
            if h.lower().startswith("content-length:"):
                try:
                    clen = int(h.split(":", 1)[1].strip())
                except ValueError:
                    pass
        body = rest
        while len(body) < clen:
            more = sock.recv(65536)
            if not more:
                break
            body += more
        try:
            req_json = json.loads(body.decode("utf-8", "replace"))
            text = req_json.get("input", "") or req_json.get("text", "")
        except (ValueError, AttributeError):
            sock.sendall(b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n")
            return

        text = sanitize(text)
        if not text:
            sock.sendall(b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n")
            return

        fmt = str(req_json.get("response_format") or "").strip().lower()
        is_mp3 = (fmt == "mp3")

        if STREAM:
            if is_mp3:
                try:
                    serve_streaming_mp3(sock, text)
                except Exception as e:  # noqa: BLE001 — device must not hang
                    log(f"mp3 stream handler crashed: {type(e).__name__}: {e}")
                return
            try:
                serve_streaming(sock, text)
            except Exception as e:  # noqa: BLE001 — device must not hang
                log(f"stream handler crashed: {type(e).__name__}: {e}")
            return

        if is_mp3:
            warn_mp3_gain_once()
            t0 = time.time()
            try:
                mp3, stats = minimax_synthesize_mp3(text)
            except Exception as e:  # noqa: BLE001 — device must not hang
                # Never logs API_KEY: messages come from our own code only.
                log(f"mp3 synth failed: {type(e).__name__}: {e}")
                sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n")
                return
            sock.sendall(b"HTTP/1.1 200 OK\r\n"
                          b"Content-Type: audio/mpeg\r\n"
                          b"Content-Length: " + str(len(mp3)).encode() + b"\r\n"
                          b"Connection: close\r\n\r\n" + mp3)
            log(f"ok mp3 text={text[:80]!r} "
                f"t_total={round(time.time() - t0, 2)}s {stats}")
            return

        t0 = time.time()
        try:
            wav, stats = minimax_synthesize(text)
        except Exception as e:  # noqa: BLE001 — device must not hang
            # Never logs API_KEY: messages come from our own code only.
            log(f"synth failed: {type(e).__name__}: {e}")
            sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n")
            return

        sock.sendall(b"HTTP/1.1 200 OK\r\n"
                      b"Content-Type: audio/wav\r\n"
                      b"Content-Length: " + str(len(wav)).encode() + b"\r\n"
                      b"Connection: close\r\n\r\n" + wav)
        log(f"ok text={text[:80]!r} t_total={round(time.time() - t0, 2)}s {stats}")

    def _read_headers(self, sock):
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(65536)
            if not chunk:
                return None, b""
            buf += chunk
            if len(buf) > 1 << 20:
                raise ValueError("header too large")
        head, rest = buf.split(b"\r\n\r\n", 1)
        return head.decode("latin1"), rest


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    if not API_KEY:
        log("FATAL: MINIMAX_API_KEY not set")
        raise SystemExit(1)
    if not VOICE_ID:
        log("FATAL: MINIMAX_VOICE_ID not set")
        raise SystemExit(1)
    log(f"starting on :{PORT} model={MODEL} host={HOST} voice_id_set={bool(VOICE_ID)} "
        f"stream={STREAM} gain={GAIN} headstart_ms={HEADSTART_MS}")
    # Loopback only: the gateway (:8124) is the authenticated front door and reaches
    # this server on 127.0.0.1. Binding 0.0.0.0 would let anyone on the LAN spend the
    # TTS quota with no auth. Set TTS_BIND=0.0.0.0 only if you know why.
    bind = os.environ.get("TTS_BIND", "127.0.0.1")
    with Server((bind, PORT), Handler) as srv:
        srv.serve_forever()


if __name__ == "__main__":
    main()
