#!/usr/bin/env python3
"""Tests for the POST /v1/audio/speech TTS proxy route.

The fake upstream dribbles 3 chunks, 0.3 s apart, close-delimited
(no Content-Length, no Transfer-Encoding) - mirroring the real :8125 TTS
service. Covers:
  - no / wrong Bearer key -> 401 and the upstream is never called
  - right key -> 200, body byte-identical to the upstream, the first chunk
    reaches the device before the upstream finished sending (streaming proof),
    response headers contain no Content-Length and no Transfer-Encoding
  - upstream unreachable -> 502
  - the device's Authorization header is not forwarded upstream

Run: python3 -m unittest test_tts_proxy
"""
import http.client
import socket
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

GATEWAY_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GATEWAY_DIR))

from stackchan_gateway import (  # noqa: E402
    GatewayApplication,
    GatewayConfig,
    TTS_PATH,
    make_handler,
)

DEVICE_TOKEN = "tts-device-token"

# Fake WAV-ish payload: 3 chunks, dribbled 0.3 s apart by the fake upstream.
CHUNKS = (b"RIFF" + b"\x01" * 4092, b"\x02" * 4096, b"\x03" * 2048)
EXPECTED_BODY = b"".join(CHUNKS)
TTS_BODY = b'{"model":"tts-1","input":"hello","voice":"alloy"}'


class DribbleState:
    def __init__(self):
        self.lock = threading.Lock()
        self.hits = 0
        self.authorization = "unset"
        self.body = b""
        self.content_type = ""
        self.done = threading.Event()
        self.done_at = 0.0


def make_dribble_handler(state):
    class DribbleHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *args):
            return

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            with state.lock:
                state.hits += 1
                state.authorization = self.headers.get("Authorization")
                state.body = body
                state.content_type = self.headers.get("Content-Type")
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Connection", "close")
            self.end_headers()
            for chunk in CHUNKS:
                self.wfile.write(chunk)
                self.wfile.flush()
                time.sleep(0.3)
            with state.lock:
                state.done_at = time.monotonic()
            state.done.set()

    return DribbleHandler


def make_app(tts_upstream_url):
    config = GatewayConfig(
        host="127.0.0.1",
        port=0,
        upstream_url="http://127.0.0.1:1/v1/chat/completions",
        upstream_ca=None,
        device_token=DEVICE_TOKEN,
        tts_upstream_url=tts_upstream_url,
    )
    return GatewayApplication(config)


def start_server(handler_class):
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_class)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


class TTSProxyTests(unittest.TestCase):
    def setUp(self):
        self.state = DribbleState()
        self.upstream, self.upstream_thread = start_server(
            make_dribble_handler(self.state)
        )
        self.app = make_app(
            "http://127.0.0.1:{}/v1/audio/speech".format(
                self.upstream.server_address[1]
            )
        )
        self.gateway, self.gateway_thread = start_server(make_handler(self.app))
        self.gateway_port = self.gateway.server_address[1]

    def tearDown(self):
        for server, thread in (
            (self.gateway, self.gateway_thread),
            (self.upstream, self.upstream_thread),
        ):
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def raw_post(self, token, body=TTS_BODY, content_type="application/json"):
        sock = socket.create_connection(("127.0.0.1", self.gateway_port), timeout=10)
        lines = [
            "POST {} HTTP/1.0".format(TTS_PATH),
            "Host: 127.0.0.1",
            "Content-Type: {}".format(content_type),
            "Content-Length: {}".format(len(body)),
        ]
        if token is not None:
            lines.append("Authorization: Bearer {}".format(token))
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body)
        return sock

    @staticmethod
    def read_response_head(sock):
        buf = b""
        while b"\r\n\r\n" not in buf:
            data = sock.recv(4096)
            if not data:
                break
            buf += data
        head, _, rest = buf.partition(b"\r\n\r\n")
        return head, rest

    def upstream_hits(self):
        with self.state.lock:
            return self.state.hits

    def test_no_token_returns_401_and_upstream_not_called(self):
        sock = self.raw_post(token=None)
        head, _rest = self.read_response_head(sock)
        sock.close()
        self.assertTrue(head.startswith(b"HTTP/1.0 401"), head)
        self.assertEqual(self.upstream_hits(), 0)

    def test_wrong_token_returns_401_and_upstream_not_called(self):
        sock = self.raw_post(token="wrong-token")
        head, _rest = self.read_response_head(sock)
        sock.close()
        self.assertTrue(head.startswith(b"HTTP/1.0 401"), head)
        self.assertEqual(self.upstream_hits(), 0)

    def test_streaming_close_delimited_and_byte_identical(self):
        sock = self.raw_post(token=DEVICE_TOKEN)
        head, rest = self.read_response_head(sock)
        head_lower = head.lower()
        self.assertTrue(head.startswith(b"HTTP/1.0 200"), head)
        self.assertIn(b"content-type: audio/wav", head_lower)
        # Close-delimited: neither length header may appear.
        self.assertNotIn(b"content-length", head_lower)
        self.assertNotIn(b"transfer-encoding", head_lower)

        # Streaming proof: the first body byte must arrive before the fake
        # upstream finished sending its 3 chunks (~0.6 s of dribbling).
        sock.settimeout(10)
        body = bytearray(rest)
        first_byte_at = time.monotonic() if body else None
        while True:
            data = sock.recv(65536)
            if not data:
                break
            if first_byte_at is None:
                first_byte_at = time.monotonic()
            body.extend(data)
        sock.close()

        self.assertEqual(bytes(body), EXPECTED_BODY)
        self.assertTrue(
            self.state.done.wait(timeout=10), "fake upstream never finished"
        )
        with self.state.lock:
            done_at = self.state.done_at
        self.assertIsNotNone(first_byte_at, "no body bytes received at all")
        self.assertLess(
            first_byte_at,
            done_at,
            "first byte arrived only after the upstream finished: not streaming",
        )

        # The device's key must not be forwarded; body must go through verbatim.
        with self.state.lock:
            self.assertIsNone(self.state.authorization)
            self.assertEqual(self.state.body, TTS_BODY)
            self.assertEqual(self.state.content_type, "application/json")

    def test_upstream_unreachable_returns_502(self):
        app = make_app("http://127.0.0.1:1/v1/audio/speech")
        gateway, thread = start_server(make_handler(app))
        try:
            port = gateway.server_address[1]
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            connection.request(
                "POST",
                TTS_PATH,
                body=TTS_BODY,
                headers={
                    "Authorization": "Bearer " + DEVICE_TOKEN,
                    "Content-Type": "application/json",
                },
            )
            response = connection.getresponse()
            status = response.status
            response.read()
            connection.close()
        finally:
            gateway.shutdown()
            gateway.server_close()
            thread.join(timeout=5)
        self.assertEqual(status, 502)


if __name__ == "__main__":
    unittest.main()
