#!/usr/bin/env python3
"""Tests for the 2026-10-28 trip fixes (code-to-muse 20261028-174524).

1. Default weather location comes from a small JSON file
   (env STACKCHAN_LOCATION_FILE, default ~/StackChan/location.json):
   a query the owner names still wins; missing file -> 东京 silently; invalid
   file -> 东京 plus one warning; the weather prompt names the default.
2. English weather questions are detected (temperature, how hot/cold/
   warm) with word boundaries; near-misses stay non-weather.
3. /v1/photos rejects log received vs declared length, and a body that
   ends early or stalls becomes a logged reject instead of silence.

Run: python3 -m unittest tests.test_trip_fixes   (from gateway/)
"""

import http.client
import json
import os
import socket
import sys
import tempfile
import threading
import unittest
from datetime import datetime, timedelta
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest import mock
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # gateway/

import stackchan_gateway as gw  # noqa: E402

JPEG = b"\xff\xd8" + b"\x00" * 100 + b"\xff\xd9"


def fake_forecast(start: datetime):
    times, temperatures, probabilities, precipitation, codes, winds = [], [], [], [], [], []
    for index in range(7 * 24):
        point = start + timedelta(hours=index)
        times.append(point.strftime("%Y-%m-%dT%H:00"))
        temperatures.append(15 + (index % 4))
        probabilities.append(5)
        precipitation.append(0.0)
        codes.append(1)
        winds.append(12)
    return {
        "current": {
            "time": start.strftime("%Y-%m-%dT%H:00"),
            "temperature_2m": 12.0,
            "apparent_temperature": 11.0,
            "precipitation": 0.0,
            "weather_code": 1,
            "wind_speed_10m": 9.0,
        },
        "hourly": {
            "time": times,
            "temperature_2m": temperatures,
            "precipitation_probability": probabilities,
            "precipitation": precipitation,
            "rain": precipitation,
            "showers": precipitation,
            "weather_code": codes,
            "wind_speed_10m": winds,
        },
    }


def weather_messages(text):
    return [{"role": "user", "content": text}]


class LocationFileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.file = Path(self.tmp.name) / "location.json"

    def env(self):
        return mock.patch.dict(os.environ, {gw.LOCATION_FILE_ENV: str(self.file)})

    def test_file_query_becomes_the_default(self):
        self.file.write_text(json.dumps({"query": "巴黎"}), encoding="utf-8")
        with self.env():
            self.assertEqual(gw.requested_city(weather_messages("天气怎么样")), "巴黎")
            self.assertEqual(gw.default_location_query(), "巴黎")

    def test_explicit_city_still_wins_over_file(self):
        self.file.write_text(json.dumps({"query": "巴黎"}), encoding="utf-8")
        with self.env():
            self.assertEqual(gw.requested_city(weather_messages("上海天气怎么样")), "上海")

    def test_missing_file_falls_back_to_default_silently(self):
        import logging

        records = []

        class _Capture(logging.Handler):
            def emit(self, record):
                records.append(record)

        handler = _Capture(level=logging.WARNING)
        logger = logging.getLogger("stackchan-gateway")
        logger.addHandler(handler)
        try:
            with self.env():
                self.assertEqual(gw.requested_city(weather_messages("天气怎么样")), "东京")
        finally:
            logger.removeHandler(handler)
        self.assertEqual(records, [])

    def test_invalid_json_warns_once_and_falls_back(self):
        self.file.write_text("{not json", encoding="utf-8")
        with self.env():
            with self.assertLogs("stackchan-gateway", level="WARNING") as logs:
                self.assertEqual(gw.requested_city(weather_messages("天气怎么样")), "东京")
        self.assertEqual(len(logs.output), 1)
        self.assertIn("location file invalid", logs.output[0])

    def test_file_without_query_warns_and_falls_back(self):
        self.file.write_text(json.dumps({"city": "巴黎"}), encoding="utf-8")
        with self.env():
            with self.assertLogs("stackchan-gateway", level="WARNING") as logs:
                self.assertEqual(gw.default_location_query(), "东京")
        self.assertIn("location file invalid", logs.output[0])

    def _service(self):
        start = datetime(2026, 10, 20, 0, 0)

        def fetch(url, timeout):
            if "geocoding-api" in url:
                return {
                    "results": [
                        {
                            "name": "巴黎",
                            "latitude": 51.5074,
                            "longitude": -0.1278,
                            "admin1": "英格兰",
                            "country": "英国",
                            "country_code": "GB",
                            "timezone": "Europe/Paris",
                        }
                    ]
                }
            return fake_forecast(start)

        return gw.WeatherService(
            fetch_json=fetch,
            now_fn=lambda: datetime(2026, 10, 20, 13, 4, tzinfo=ZoneInfo("Europe/Paris")),
        )

    def test_prompt_sentence_names_the_file_default(self):
        self.file.write_text(json.dumps({"query": "巴黎"}), encoding="utf-8")
        with self.env():
            city = gw.requested_city(weather_messages("天气怎么样"))
            context, location = self._service().context_for(city)
        self.assertEqual(location.name, "巴黎")
        self.assertIn("默认地点是巴黎；", context)

    def test_prompt_sentence_stays_default_without_a_file(self):
        with self.env():
            context, _location = self._service().context_for(
                gw.requested_city(weather_messages("天气怎么样"))
            )
        self.assertIn("默认地点是东京；", context)


class EnglishWeatherTest(unittest.TestCase):
    def test_whats_the_temperature(self):
        self.assertTrue(gw.is_weather_request(weather_messages("what's the temperature")))

    def test_how_cold_is_it(self):
        self.assertTrue(gw.is_weather_request(weather_messages("how cold is it outside?")))

    def test_how_warm_tomorrow(self):
        self.assertTrue(gw.is_weather_request(weather_messages("how warm will it be tomorrow")))

    def test_how_hot(self):
        self.assertTrue(gw.is_weather_request(weather_messages("How hot is it today")))

    def test_negative_mood(self):
        self.assertFalse(gw.is_weather_request(weather_messages("I'm not in the mood")))

    def test_negative_whether(self):
        self.assertFalse(
            gw.is_weather_request(weather_messages("tell me about whether you like cats"))
        )

    def test_ac_temperature_still_not_weather(self):
        # Pre-existing Chinese negative: setting the air conditioner.
        self.assertFalse(gw.is_weather_request(weather_messages("把空调温度调低两度")))


class PhotoRejectLogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.photo_dir = Path(self.tmp.name) / "photos"
        config = gw.GatewayConfig(
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
            photo_dir=self.photo_dir,
        )
        self.app = gw.GatewayApplication(config)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), gw.make_handler(self.app))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_port

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def raw_post(self, declared_length, chunks, close_write=True):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        head = (
            "POST /v1/photos HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Authorization: Bearer test-secret\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: {}\r\n"
            "\r\n"
        ).format(declared_length).encode("ascii")
        sock.sendall(head)
        for chunk in chunks:
            sock.sendall(chunk)
        if close_write:
            sock.shutdown(socket.SHUT_WR)
        data = b""
        try:
            while True:
                piece = sock.recv(4096)
                if not piece:
                    break
                data += piece
        finally:
            sock.close()
        status = int(data.split(b" ", 2)[1])
        body = data.split(b"\r\n\r\n", 1)[1]
        return status, json.loads(body.decode("utf-8"))

    def saved_jpegs(self):
        return list(self.photo_dir.rglob("*.jpg"))

    def test_client_closes_early_logged_with_both_numbers(self):
        partial = b"\xff\xd8" + b"\x00" * 98  # 100 bytes of a declared 41234
        with self.assertLogs("stackchan-gateway", level="WARNING") as logs:
            status, payload = self.raw_post(41234, [partial])
        self.assertEqual(status, 408)  # firmware retries 408, not other 4xx
        self.assertEqual(payload["error"], "truncated")
        joined = "\n".join(logs.output)
        self.assertIn("reason=truncated got=100 expected=41234", joined)
        self.assertEqual(self.saved_jpegs(), [])

    def test_stalled_body_times_out_and_is_logged(self):
        saved_timeout = gw.PHOTO_READ_TIMEOUT_SECONDS
        gw.PHOTO_READ_TIMEOUT_SECONDS = 0.3
        try:
            with self.assertLogs("stackchan-gateway", level="WARNING") as logs:
                status, payload = self.raw_post(1000, [b"\xff\xd8" + b"\x00" * 8], close_write=False)
        finally:
            gw.PHOTO_READ_TIMEOUT_SECONDS = saved_timeout
        self.assertEqual(status, 408)
        self.assertEqual(payload["error"], "read_timeout")
        joined = "\n".join(logs.output)
        self.assertIn("reason=read_timeout got=10 expected=1000", joined)
        self.assertEqual(self.saved_jpegs(), [])

    def test_normal_upload_unchanged(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request(
            "POST",
            "/v1/photos",
            body=JPEG,
            headers={"Authorization": "Bearer test-secret", "Content-Type": "image/jpeg"},
        )
        response = conn.getresponse()
        payload = json.loads(response.read().decode("utf-8"))
        conn.close()
        self.assertEqual(response.status, 200)
        self.assertTrue(payload.get("ok"))
        saved = self.saved_jpegs()
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0].read_bytes(), JPEG)


if __name__ == "__main__":
    unittest.main()
