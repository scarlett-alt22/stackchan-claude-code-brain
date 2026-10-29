#!/usr/bin/env python3
"""Exercise the installed STT gateway without printing secrets or transcript text."""

import argparse
import json
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path

from stackchan_gateway import MAX_STT_REQUEST_BYTES, STT_PATH, load_stt_token


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SECRET_CONFIG = ROOT_DIR / "SD-staging/yaml/SC_SecConfig.yaml"
DEFAULT_GATEWAY = "http://127.0.0.1:8124"


def multipart_body(audio: bytes, boundary: str) -> bytes:
    prefix = (
        "--{0}\r\n"
        'Content-Disposition: form-data; name="model"\r\n\r\n'
        "FunAudioLLM/SenseVoiceSmall\r\n"
        "--{0}\r\n"
        'Content-Disposition: form-data; name="file"; filename="smoke.wav"\r\n'
        "Content-Type: audio/wav\r\n\r\n"
    ).format(boundary).encode("ascii")
    suffix = ("\r\n--{}--\r\n".format(boundary)).encode("ascii")
    return prefix + audio + suffix


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("audio", type=Path)
    parser.add_argument("--gateway", default=DEFAULT_GATEWAY)
    parser.add_argument("--secret-config", type=Path, default=DEFAULT_SECRET_CONFIG)
    args = parser.parse_args()

    boundary = "StackChanSmoke" + uuid.uuid4().hex
    body = multipart_body(args.audio.read_bytes(), boundary)
    if len(body) > MAX_STT_REQUEST_BYTES:
        print("request_too_large", file=sys.stderr)
        return 2

    token = load_stt_token(args.secret_config)
    request = urllib.request.Request(
        args.gateway.rstrip("/") + STT_PATH,
        data=body,
        headers={
            "Authorization": "Bearer " + token,
            "Content-Type": "multipart/form-data; boundary=" + boundary,
            "Accept": "application/json",
        },
        method="POST",
    )
    del token
    try:
        with urllib.request.urlopen(request, timeout=40) as response:
            payload = json.loads(response.read(1024 * 1024).decode("utf-8"))
            text_present = isinstance(payload, dict) and bool(payload.get("text"))
            print("status={} text_present={}".format(response.status, text_present))
            return 0 if response.status == 200 and text_present else 1
    except urllib.error.HTTPError as exc:
        print("status={} text_present=False".format(exc.code))
        return 1


if __name__ == "__main__":
    sys.exit(main())
