import json
import hashlib
import http.client
import sys
import tempfile
import threading
import unittest
from datetime import datetime, timedelta
from http.server import ThreadingHTTPServer
from pathlib import Path
from zoneinfo import ZoneInfo


GATEWAY_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GATEWAY_DIR))

from stackchan_gateway import (  # noqa: E402
    DEFAULT_LOCATION,
    GatewayApplication,
    GatewayConfig,
    MAX_STT_REQUEST_BYTES,
    STT_PATH,
    TTLCache,
    WeatherService,
    extract_city,
    inject_system_context,
    is_weather_request,
    load_device_token,
    load_stt_token,
    make_handler,
    requested_city,
)


def fake_forecast(start: datetime):
    times = []
    temperatures = []
    probabilities = []
    precipitation = []
    codes = []
    winds = []
    for index in range(7 * 24):
        point = start + timedelta(hours=index)
        times.append(point.strftime("%Y-%m-%dT%H:00"))
        temperatures.append(27 + (index % 4))
        rainy = point.date() == start.date() and 14 <= point.hour < 17
        probabilities.append(70 if rainy else 5)
        precipitation.append(0.4 if rainy else 0.0)
        codes.append(80 if rainy else 1)
        winds.append(12)
    return {
        "current": {
            "time": start.strftime("%Y-%m-%dT%H:00"),
            "temperature_2m": 29.2,
            "apparent_temperature": 32.1,
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


class IntentTests(unittest.TestCase):
    def test_direct_weather_question(self):
        messages = [{"role": "user", "content": "下午会不会下雨？"}]
        self.assertTrue(is_weather_request(messages))

    def test_air_conditioner_temperature_is_not_weather(self):
        messages = [{"role": "user", "content": "把空调温度调低两度"}]
        self.assertFalse(is_weather_request(messages))

    def test_followup_uses_previous_weather_turn(self):
        messages = [
            {"role": "user", "content": "东京下午会不会下雨？"},
            {"role": "assistant", "content": "下午可能有雨。"},
            {"role": "user", "content": "那晚上呢？"},
        ]
        self.assertTrue(is_weather_request(messages))
        self.assertEqual(requested_city(messages), "东京")

    def test_city_extraction(self):
        self.assertEqual(extract_city("上海明天天气怎么样"), "上海")
        self.assertEqual(extract_city("东京下午会不会下雨"), "东京")
        self.assertIsNone(extract_city("下午会不会下雨"))

    def test_city_extraction_strips_leading_time_words(self):
        # 回归：时间词在城市名前面时，非贪婪的 `(城市)(?:的)?天气` 会把整段
        # 「今天东京」一起吞成城市名（CITY_STOPWORDS 只挡得住单独的「今天」）。
        self.assertEqual(extract_city("今天东京天气"), "东京")
        self.assertEqual(extract_city("明天上海天气怎么样"), "上海")
        self.assertEqual(extract_city("那今天东京的天气呢"), "东京")
        self.assertEqual(extract_city("周末杭州天气"), "杭州")
        # 只有时间词、没有城市 → 仍然交给默认地点，不能被剥成半个词
        self.assertIsNone(extract_city("今天天气怎么样"))
        self.assertIsNone(extract_city("明天的天气"))
        # 别误伤本来就以时间词起头的合法地名
        self.assertEqual(extract_city("周口天气"), "周口")
        self.assertEqual(extract_city("中山明天天气"), "中山")

    def test_default_city(self):
        messages = [{"role": "user", "content": "下午会不会下雨？"}]
        self.assertEqual(requested_city(messages), "东京")


class CacheTests(unittest.TestCase):
    def test_expiry(self):
        cache = TTLCache()
        cache.set("x", 1, 10, now=100)
        self.assertEqual(cache.get("x", now=109), 1)
        self.assertIsNone(cache.get("x", now=110))


class WeatherServiceTests(unittest.TestCase):
    def test_tokyo_context_and_cache(self):
        start = datetime(2026, 7, 21, 0, 0)
        calls = []

        def fetch(url, timeout):
            calls.append((url, timeout))
            return fake_forecast(start)

        service = WeatherService(
            fetch_json=fetch,
            now_fn=lambda: datetime(2026, 7, 21, 13, 4, tzinfo=ZoneInfo("Asia/Tokyo")),
        )
        context, location = service.context_for("东京")
        self.assertEqual(location, DEFAULT_LOCATION)
        self.assertIn("location=东京，东京都", context)
        self.assertIn("今天下午", context)
        self.assertIn("14:00～17:00", context)
        service.context_for("东京")
        self.assertEqual(len(calls), 1)

    def test_geocoded_city(self):
        start = datetime(2026, 7, 21, 0, 0)

        def fetch(url, timeout):
            if "geocoding-api" in url:
                return {
                    "results": [
                        {
                            "name": "上海",
                            "latitude": 31.2222,
                            "longitude": 121.4581,
                            "admin1": "上海",
                            "country": "中国",
                            "country_code": "CN",
                            "timezone": "Asia/Tokyo",
                        }
                    ]
                }
            return fake_forecast(start)

        service = WeatherService(
            fetch_json=fetch,
            now_fn=lambda: datetime(2026, 7, 21, 13, 4, tzinfo=ZoneInfo("Asia/Tokyo")),
        )
        context, location = service.context_for("上海")
        self.assertEqual(location.name, "上海")
        self.assertIn("location=上海", context)
        # ★2026-08-04 更新：原断言是 "reply in English only"，那个前提 07-22 起就不成立了
        # ——设备 TTS 已切到 :8125 中文音色。断言跟着线上走，不是把线上改回来迁就测试。
        # 18 字上限是 07-24 定的（Qwen3-TTS RTF≈4，30 字要等 22 秒）。
        self.assertIn("用提问用的语言回答", context)
        self.assertNotIn("用中文回答", context)
        self.assertIn("一般 50 个字以内", context)


class RequestTests(unittest.TestCase):
    def test_context_is_added_to_existing_system_prompt(self):
        payload = {
            "messages": [
                {"role": "system", "content": "role"},
                {"role": "system", "content": "system"},
                {"role": "user", "content": "下午会下雨吗"},
            ]
        }
        result = inject_system_context(payload, "tool data")
        self.assertEqual(payload["messages"][1]["content"], "system")
        self.assertEqual(result["messages"][1]["content"], "system\n\ntool data")

    def test_load_token_without_exposing_comment(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "secret.yaml"
            path.write_text(
                "wifi:\n  password: ignored\napikey:\n  aiservice: \"test-token\" # comment\n",
                encoding="utf-8",
            )
            self.assertEqual(load_device_token(path), "test-token")

    def test_load_stt_token(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "secret.yaml"
            path.write_text(
                "apikey:\n  stt: 'test-stt-token' # comment\n  aiservice: test-token\n",
                encoding="utf-8",
            )
            self.assertEqual(load_stt_token(path), "test-stt-token")

    def test_hash_verifier_accepts_only_matching_bearer(self):
        config = GatewayConfig(
            host="127.0.0.1",
            port=0,
            upstream_url="http://127.0.0.1:9/v1/chat/completions",
            upstream_ca=None,
            device_token_sha256=hashlib.sha256(b"test-token").hexdigest(),
        )
        application = GatewayApplication(config)
        self.assertTrue(application.authorized("Bearer test-token"))
        self.assertFalse(application.authorized("Bearer wrong-token"))

    def test_ordinary_chat_body_is_unchanged(self):
        config = GatewayConfig(
            host="127.0.0.1",
            port=0,
            upstream_url="http://127.0.0.1:9/v1/chat/completions",
            upstream_ca=None,
            device_token="test-token",
        )
        application = GatewayApplication(config)
        raw = json.dumps(
            {
                "stream": False,
                "messages": [{"role": "user", "content": "你好"}],
            },
            separators=(",", ":"),
        ).encode("utf-8")
        body, weather_used, location = application.prepare_request(raw)
        self.assertEqual(body, raw)
        self.assertFalse(weather_used)
        self.assertEqual(location, "")

    def test_weather_failure_injects_no_guess_marker(self):
        class FailingWeather:
            def context_for(self, query):
                raise RuntimeError("offline")

        config = GatewayConfig(
            host="127.0.0.1",
            port=0,
            upstream_url="http://127.0.0.1:9/v1/chat/completions",
            upstream_ca=None,
            device_token="test-token",
        )
        application = GatewayApplication(config, weather_service=FailingWeather())
        raw = json.dumps(
            {
                "stream": False,
                "messages": [{"role": "user", "content": "下午会下雨吗"}],
            },
            ensure_ascii=False,
        ).encode("utf-8")
        body, weather_used, location = application.prepare_request(raw)
        self.assertTrue(weather_used)
        self.assertEqual(location, "东京")
        self.assertIn("status=unavailable", body.decode("utf-8"))
        self.assertIn("不得猜测", body.decode("utf-8"))
        # ★2026-08-04 同上，跟着线上的中文提示词走。
        self.assertIn("用提问用的语言回答", body.decode("utf-8"))


class STTHandlerTests(unittest.TestCase):
    def setUp(self):
        config = GatewayConfig(
            host="127.0.0.1",
            port=0,
            upstream_url="http://127.0.0.1:9/v1/chat/completions",
            upstream_ca=None,
            device_token="chat-token",
            stt_token_sha256=hashlib.sha256(b"stt-token").hexdigest(),
        )
        self.application = GatewayApplication(config)
        self.server = ThreadingHTTPServer(
            ("127.0.0.1", 0), make_handler(self.application)
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, body, token="stt-token", content_type="multipart/form-data; boundary=x"):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_address[1], timeout=2
        )
        connection.request(
            "POST",
            STT_PATH,
            body=body,
            headers={
                "Authorization": "Bearer " + token,
                "Content-Type": content_type,
            },
        )
        response = connection.getresponse()
        result = (response.status, response.read())
        connection.close()
        return result

    def test_stt_body_and_headers_are_forwarded_unchanged(self):
        captured = {}

        def forward(body, authorization, content_type):
            captured.update(
                body=body, authorization=authorization, content_type=content_type
            )
            return 200, "application/json", b'{"text":"ok"}'

        self.application.forward_stt = forward
        body = b"--x\r\nContent-Disposition: form-data; name=model\r\n\r\ntest\r\n--x--\r\n"
        status, response_body = self.request(body)
        self.assertEqual(status, 200)
        self.assertEqual(response_body, b'{"text":"ok"}\r')
        self.assertEqual(captured["body"], body)
        self.assertEqual(captured["authorization"], "Bearer stt-token")
        self.assertEqual(captured["content_type"], "multipart/form-data; boundary=x")

    def test_stt_rejects_wrong_token(self):
        status, _body = self.request(b"body", token="wrong")
        self.assertEqual(status, 401)

    def test_stt_rejects_wrong_media_type(self):
        status, _body = self.request(b"body", content_type="application/json")
        self.assertEqual(status, 415)

    def test_stt_rejects_oversized_request_without_reading_body(self):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_address[1], timeout=2
        )
        connection.putrequest("POST", STT_PATH)
        connection.putheader("Authorization", "Bearer stt-token")
        connection.putheader("Content-Type", "multipart/form-data; boundary=x")
        connection.putheader("Content-Length", str(MAX_STT_REQUEST_BYTES + 1))
        connection.endheaders()
        response = connection.getresponse()
        self.assertEqual(response.status, 413)
        response.read()
        connection.close()

    def test_stt_upstream_failure_returns_502(self):
        def fail(_body, _authorization, _content_type):
            raise OSError("offline")

        self.application.forward_stt = fail
        status, _body = self.request(b"body")
        self.assertEqual(status, 502)


if __name__ == "__main__":
    unittest.main()
