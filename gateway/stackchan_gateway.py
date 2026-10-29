#!/usr/bin/env python3
"""Small OpenAI-compatible gateway for StackChan tools.

The gateway keeps the existing API-hosted Claude as the answering model.  It
only enriches weather questions with trusted Open-Meteo data before forwarding
the request to the existing VPS shim.

2026-10-04: added an optional "cc brain" mode (env STACKCHAN_BRAIN=cc).
In cc mode, chat requests are answered by OWNER's open Claude Code window
via a forked headless session (see cc_brain.py); on any brain failure the
gateway falls back to the old VPS path. Default (STACKCHAN_BRAIN=api) keeps
the original behavior byte-for-byte.

2026-10-04: added POST /v1/audio/speech, an authenticated TTS proxy
(code-to-muse 20261004-134159-travel-gateway-tts-proxy). The device's Bearer
key (same as chat) is verified here and NOT forwarded upstream; the TTS
response is relayed close-delimited (no Content-Length, no Transfer-Encoding)
because the device reads the raw TCP stream and does not decode chunked.
"""

import copy
import hashlib
import hmac
import http.client
import json
import logging
import os
import re
import socket
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional, Tuple
from zoneinfo import ZoneInfo

try:
    import cc_brain
except ImportError:  # cc brain module not installed: cc mode unavailable
    cc_brain = None


def _cc_brain_enabled() -> bool:
    return (
        cc_brain is not None
        and os.environ.get("STACKCHAN_BRAIN", "api").strip().lower() == "cc"
    )


LOGGER = logging.getLogger("stackchan-gateway")
ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SECRET_CONFIG = ROOT_DIR / "SD-staging/yaml/SC_SecConfig.yaml"
DEFAULT_UPSTREAM_CA = ROOT_DIR / "SD-staging/shim_ca.pem"
DEFAULT_UPSTREAM_URL = "https://YOUR_VPS_IP:8443/v1/chat/completions"
# Local mlx-whisper (extras/stt_whisper_server.py). Never default to a cloud
# service: the device audio and STT token would leave the Mac without anyone choosing that.
DEFAULT_STT_UPSTREAM_URL = "http://127.0.0.1:8126/v1/audio/transcriptions"
DEFAULT_TIMEZONE = "Asia/Tokyo"
MAX_REQUEST_BYTES = 256 * 1024
MAX_STT_REQUEST_BYTES = 256 * 1024
CHAT_PATH = "/v1/chat/completions"
STT_PATH = "/v1/audio/transcriptions"
# ★2026-10-27 ears (SenseVoice on :8127). The STT reply carries emotion/event
# next to text, but the device only reads "text" and sends it back as the
# chat message, so the tone would be lost. The gateway keeps the last STT
# result and, when the next chat text matches it, tells the brain how the owner
# sounded. Neutral speech adds nothing; labels only, never the audio.
TONE_WINDOW_SECONDS = 60
TONE_EMOTIONS = {
    "HAPPY": "开心",
    "SAD": "难过",
    "ANGRY": "生气",
    "FEARFUL": "害怕",
    "DISGUSTED": "嫌弃",
    "SURPRISED": "惊讶",
}
TONE_EVENTS = {
    "Laughter": "笑声",
    "Cry": "哭声",
    "Cough": "咳嗽",
    "Sneeze": "喷嚏",
    "Applause": "掌声",
    "BGM": "背景音乐",
    "Breath": "呼吸声",
}


def tone_hint(emotion: Any, event: Any) -> str:
    """One line for the brain, or "" when the tone says nothing beyond the words."""
    parts = []
    if emotion in TONE_EMOTIONS:
        parts.append("语气像是" + TONE_EMOTIONS[emotion])
    if event in TONE_EVENTS:
        parts.append("录音里有" + TONE_EVENTS[event])
    if not parts:
        return ""
    return "（耳朵：识别模型听着，主人这句{}。模型猜的，可能不准。）".format("，".join(parts))


def _completion_text(response_body: bytes) -> str:
    """choices[0].message.content of an upstream chat body, or "" if not there.

    Never raises: a fallback body that is not JSON, has no choices, or whose
    content is not a string simply has no recordable reply.
    """
    try:
        payload = json.loads(response_body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return ""
    if not isinstance(payload, dict):
        return ""
    choices = payload.get("choices")
    if not isinstance(choices, list) or not choices:
        return ""
    first = choices[0]
    if not isinstance(first, dict):
        return ""
    message = first.get("message")
    if not isinstance(message, dict):
        return ""
    content = message.get("content")
    return content if isinstance(content, str) else ""


# ★2026-10-28 STT hallucination filter. On silence/noise the recognizer
# sometimes returns subtitle-credit boilerplate from its training data
# (seen live: "字幕志愿者 杨茜茜" three times) and the robot then answers
# nonsense. Matching is done on the transcript with whitespace and
# punctuation stripped. Entry kinds: "contains" drops when the pattern
# appears anywhere; "contains_all" drops when every part appears;
# "whole" drops only when the pattern is the entire transcript, so
# ordinary speech that merely mentions 谢谢 or 订阅 is never dropped.
STT_HALLUCINATION_PATTERNS = (
    ("contains", "字幕志愿者"),
    ("contains_all", ("字幕由", "提供")),
    ("contains", "请不吝点赞订阅转发打赏"),
    ("contains", "明镜与点点栏目"),
    ("whole", "谢谢观看"),
)


def stt_hallucination(text: str) -> bool:
    """True when a transcript is known STT boilerplate rather than speech."""
    normalized = "".join(ch for ch in text if ch.isalnum())
    if not normalized:
        return False
    for kind, pattern in STT_HALLUCINATION_PATTERNS:
        if kind == "contains" and pattern in normalized:
            return True
        if kind == "contains_all" and all(part in normalized for part in pattern):
            return True
        if kind == "whole" and normalized == pattern:
            return True
    return False


# ★2026-10-04 TTS proxy (code-to-muse 20261004-134159-travel-gateway-tts-proxy).
# The device used to reach the TTS service directly on :8125 with no auth.
# Routing it through the gateway means only one port has to be exposed.
TTS_PATH = "/v1/audio/speech"
DEFAULT_TTS_UPSTREAM_URL = "http://127.0.0.1:8125/v1/audio/speech"
MAX_TTS_REQUEST_BYTES = 256 * 1024
TTS_CONNECT_TIMEOUT = 5  # seconds to establish the TCP connection
TTS_READ_TIMEOUT = 65  # seconds allowed for each read of the upstream response
TTS_CHUNK_BYTES = 4096
# ★2026-10-04 photo upload (code-to-muse 20261004-071145-stackchan-camera).
# The device snaps a JPEG on demand (camera init→snap→deinit) and POSTs the
# raw bytes here; the gateway stores it under ~/StackChan/photos/ and
# appends a {"type": "photo"} row to robot-log.jsonl.
PHOTOS_PATH = "/v1/photos"
MAX_PHOTO_BYTES = 1024 * 1024
# ★2026-10-28 Per-read inactivity timeout for photo bodies (code-to-muse
# 20261028-174524). A slow-but-alive upload keeps resetting it per chunk;
# only a body that stalls entirely trips it. Before this, the read had no
# timeout at all, so a stalled upload hung the handler thread forever and
# left no outcome line in the log (2026-10-25 16:34:47).
PHOTO_READ_TIMEOUT_SECONDS = 30
DEFAULT_PHOTO_DIR = Path.home() / "StackChan" / "photos"
DEFAULT_ROBOT_LOG = Path.home() / "StackChan" / "robot-log.jsonl"

WEATHER_TERMS = re.compile(
    r"天气|下雨|降雨|雨量|阵雨|雷雨|带伞|雨伞|气温|体感温度|最高温|最低温|"
    r"风力|风速|台风|冷不冷|热不热|weather|forecast|rain|umbrella|windy",
    re.IGNORECASE,
)
WEATHER_TEMPERATURE = re.compile(
    r"(?:今天|明天|后天|现在|当前|外面|室外|东京).{0,6}(?:温度|几度)|"
    r"(?:温度|几度).{0,6}(?:今天|明天|后天|现在|当前|外面|室外|东京)",
)
# ★2026-10-28 English weather phrasings (code-to-muse 20261028-174524).
# WEATHER_TERMS already substring-matches weather/forecast/rain, but
# "what's the temperature" matched nothing, so abroad the robot got no
# weather context and said it could not check. New patterns are English
# only and word-bounded, so they cannot change any Chinese outcome and do
# not fire on near-misses like "whether" or "I'm not in the mood".
WEATHER_ENGLISH = re.compile(
    r"\btemperatures?\b|\bhow\s+(?:hot|cold|warm)\b",
    re.IGNORECASE,
)
FOLLOWUP_TERMS = re.compile(
    r"^(?:那|那么|再看|还有)?(?:今天|明天|后天|上午|中午|下午|傍晚|晚上|夜里|"
    r"周末|几点|多久|概率|大不大|会吗|呢|怎么样|如何|之后|后来).{0,14}[？?。！!]*$"
)
# ★2026-08-04 城市名候选的前导噪声。**长的必须排在短的前面**（`那么` 在 `那` 前，
# 否则 `那么` 只会被剥掉一个 `那`、剩个 `么` 粘在城市名上）。
# 这里只列完整词，不用字符类——`中午` 不会误伤「中山」、`周末` 不会误伤「周口」。
CITY_LEADING_NOISE = re.compile(
    r"^(?:请问|帮我|帮忙|查一下|查查|看一下|看看|查询|"
    r"那么|那|再看|还有|"
    r"大后天|今天|明天|后天|现在|当前|上午|中午|下午|傍晚|晚上|夜里|"
    r"未来|本周|这周|周末|外面|室外)"
)
CITY_STOPWORDS = {
    "今天",
    "明天",
    "后天",
    "现在",
    "当前",
    "上午",
    "中午",
    "下午",
    "傍晚",
    "晚上",
    "夜里",
    "未来",
    "本周",
    "这周",
    "周末",
    "外面",
    "室外",
}


@dataclass(frozen=True)
class Location:
    query: str
    name: str
    latitude: float
    longitude: float
    admin1: str = ""
    country: str = "中国"
    timezone: str = DEFAULT_TIMEZONE

    @property
    def display_name(self) -> str:
        parts = [self.name]
        if self.admin1 and self.admin1 != self.name:
            parts.append(self.admin1)
        if self.country and self.country != "中国":
            parts.append(self.country)
        return "，".join(parts)


DEFAULT_LOCATION = Location(
    query="东京",
    name="东京",
    latitude=35.6762,
    longitude=139.6503,
    admin1="东京都",
)

# ★2026-10-28 Default location from a file (code-to-muse 20261028-174524).
# Abroad, the owner asked "天气怎么样" and got home-city weather because the
# default was hard-coded above. The file (env STACKCHAN_LOCATION_FILE,
# default ~/StackChan/location.json) holds {"query": "巴黎"} and is
# re-read on every request, so changing it needs no restart. Missing file
# -> 东京 exactly as before, silently. A file that exists but is
# unreadable, invalid JSON, or has no usable "query" -> 东京 plus one
# warning. A non-东京 query flows into WeatherService.resolve_location,
# i.e. the existing geocoding path with its 24 h cache.
LOCATION_FILE_ENV = "STACKCHAN_LOCATION_FILE"
DEFAULT_LOCATION_FILE = Path.home() / "StackChan" / "location.json"


def default_location_query() -> str:
    """The default weather location query: the location file, else 东京."""
    override = os.environ.get(LOCATION_FILE_ENV, "").strip()
    path = Path(override).expanduser() if override else DEFAULT_LOCATION_FILE
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return DEFAULT_LOCATION.query
    except OSError:
        LOGGER.warning("location file unreadable path=%s; using 东京", path)
        return DEFAULT_LOCATION.query
    try:
        data = json.loads(raw)
    except ValueError:
        LOGGER.warning("location file invalid path=%s; using 东京", path)
        return DEFAULT_LOCATION.query
    query = data.get("query") if isinstance(data, dict) else None
    if not isinstance(query, str) or not query.strip():
        LOGGER.warning("location file invalid path=%s; using 东京", path)
        return DEFAULT_LOCATION.query
    return query.strip()


def _default_location_name(location: Location) -> str:
    """Name of the current default, for the weather prompt sentence.

    When this request itself resolved the default, the geocoded name is
    the accurate one; otherwise the raw default query is what the owner wrote.
    """
    query = default_location_query()
    return location.name if location.query == query else query


class TTLCache:
    def __init__(self) -> None:
        self._values: Dict[str, Tuple[float, Any]] = {}

    def get(self, key: str, now: Optional[float] = None) -> Any:
        current = time.monotonic() if now is None else now
        entry = self._values.get(key)
        if entry is None:
            return None
        expires_at, value = entry
        if current >= expires_at:
            self._values.pop(key, None)
            return None
        return value

    def set(self, key: str, value: Any, ttl_seconds: int, now: Optional[float] = None) -> None:
        current = time.monotonic() if now is None else now
        self._values[key] = (current + ttl_seconds, value)


def _content_to_text(content: Any) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        chunks: List[str] = []
        for item in content:
            if isinstance(item, dict) and item.get("type") == "text":
                text = item.get("text")
                if isinstance(text, str):
                    chunks.append(text)
        return "\n".join(chunks)
    return ""


def user_messages(messages: Any) -> List[str]:
    if not isinstance(messages, list):
        return []
    result: List[str] = []
    for message in messages:
        if not isinstance(message, dict) or message.get("role") != "user":
            continue
        text = _content_to_text(message.get("content")).strip()
        if text:
            result.append(text)
    return result


def is_direct_weather_query(text: str) -> bool:
    return bool(
        WEATHER_TERMS.search(text)
        or WEATHER_TEMPERATURE.search(text)
        or WEATHER_ENGLISH.search(text)
    )


def is_weather_request(messages: Any) -> bool:
    users = user_messages(messages)
    if not users:
        return False
    latest = users[-1]
    if is_direct_weather_query(latest):
        return True
    if not FOLLOWUP_TERMS.search(latest.strip()):
        return False
    return any(is_direct_weather_query(item) for item in users[-4:-1])


def _clean_city_candidate(candidate: str) -> Optional[str]:
    value = candidate
    # ★2026-08-04 修「今天东京」被整段当成城市名。
    # 反复剥前导噪声：`(?P<city>…{2,8}?)(?:的)?天气` 是非贪婪的，遇到
    # 「今天东京天气」只有把「今天东京」整段吃下去后面才接得上「天气」，
    # 于是时间词被粘进城市名。CITY_STOPWORDS 是等值比较，挡得住单独的
    # 「今天」，挡不住「今天东京」——所以在这里剥，而不是改那几条正则
    # （改正则会动到已经在过的用例）。循环是为了「那今天东京」这种叠两层的。
    while True:
        stripped = CITY_LEADING_NOISE.sub("", value, count=1)
        if stripped == value:
            break
        value = stripped
    value = value.strip(" 的市县区？?，,。！! ")
    if not value or value in CITY_STOPWORDS:
        return None
    if len(value) < 2 or len(value) > 20:
        return None
    return value


def extract_city(text: str) -> Optional[str]:
    compact = re.sub(r"\s+", "", text)
    patterns = (
        r"(?P<city>[\u4e00-\u9fff]{2,8})(?:市|县)(?=.{0,10}(?:天气|下雨|降雨|气温|风力|风速|带伞))",
        r"(?P<city>[\u4e00-\u9fff]{2,8}?)(?=(?:今天|明天|后天|现在|当前|上午|中午|下午|傍晚|晚上|夜里|周末|未来).{0,5}(?:天气|下雨|降雨|气温|风力|风速|带伞))",
        r"(?P<city>[\u4e00-\u9fff]{2,8}?)(?:的)?天气",
        r"(?P<city>[\u4e00-\u9fff]{2,8}?)(?=会不会下雨)",
    )
    for pattern in patterns:
        match = re.search(pattern, compact)
        if match:
            cleaned = _clean_city_candidate(match.group("city"))
            if cleaned:
                return cleaned

    english = re.search(r"(?:weather|forecast)\s+(?:in|for)\s+([A-Za-z .'-]{2,40})", text, re.I)
    if english:
        return english.group(1).strip()
    return None


def requested_city(messages: Any) -> str:
    users = user_messages(messages)
    if not users:
        return default_location_query()
    current = extract_city(users[-1])
    if current:
        return current
    for previous in reversed(users[:-1]):
        if not is_direct_weather_query(previous):
            continue
        city = extract_city(previous)
        if city:
            return city
    return default_location_query()


def fetch_json_url(url: str, timeout: int = 12) -> Dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/json", "User-Agent": "StackChanWeatherGateway/1.0"},
        method="GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read(512 * 1024)
    parsed = json.loads(payload.decode("utf-8"))
    if not isinstance(parsed, dict):
        raise ValueError("weather response is not a JSON object")
    return parsed


def weather_description(code: Any) -> str:
    try:
        value = int(code)
    except (TypeError, ValueError):
        return "未知"
    if value == 0:
        return "晴"
    if value == 1:
        return "大致晴朗"
    if value == 2:
        return "多云"
    if value == 3:
        return "阴"
    if value in (45, 48):
        return "有雾"
    if 51 <= value <= 57:
        return "毛毛雨"
    if 61 <= value <= 67:
        return "雨"
    if 71 <= value <= 77:
        return "雪"
    if 80 <= value <= 82:
        return "阵雨"
    if value in (85, 86):
        return "阵雪"
    if 95 <= value <= 99:
        return "雷雨"
    return "天气代码{}".format(value)


def _number(value: Any, digits: int = 1) -> str:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return "未知"
    if digits == 0:
        return str(int(round(number)))
    return ("{:.%df}" % digits).format(number)


def _hourly_points(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    hourly = data.get("hourly")
    if not isinstance(hourly, dict):
        return []
    times = hourly.get("time")
    if not isinstance(times, list):
        return []
    fields = (
        "temperature_2m",
        "precipitation_probability",
        "precipitation",
        "rain",
        "showers",
        "weather_code",
        "wind_speed_10m",
    )
    result: List[Dict[str, Any]] = []
    for index, timestamp in enumerate(times):
        if not isinstance(timestamp, str):
            continue
        try:
            parsed_time = datetime.fromisoformat(timestamp)
        except ValueError:
            continue
        point: Dict[str, Any] = {"time": parsed_time}
        for field in fields:
            values = hourly.get(field)
            point[field] = values[index] if isinstance(values, list) and index < len(values) else None
        result.append(point)
    return result


def _date_label(day: Any, today: Any) -> str:
    delta = (day - today).days
    if delta == 0:
        return "今天"
    if delta == 1:
        return "明天"
    if delta == 2:
        return "后天"
    return day.strftime("%m月%d日")


def _period_summary(points: Iterable[Dict[str, Any]]) -> str:
    selected = list(points)
    if not selected:
        return "无数据"
    temperatures = [float(item["temperature_2m"]) for item in selected if item.get("temperature_2m") is not None]
    probabilities = [float(item["precipitation_probability"]) for item in selected if item.get("precipitation_probability") is not None]
    precipitation = [float(item["precipitation"]) for item in selected if item.get("precipitation") is not None]
    winds = [float(item["wind_speed_10m"]) for item in selected if item.get("wind_speed_10m") is not None]
    codes = [item.get("weather_code") for item in selected if item.get("weather_code") is not None]
    descriptions = []
    for code in codes:
        description = weather_description(code)
        if description not in descriptions:
            descriptions.append(description)
    temp_text = "未知"
    if temperatures:
        temp_text = "{}～{}℃".format(_number(min(temperatures), 0), _number(max(temperatures), 0))
    probability_text = _number(max(probabilities), 0) if probabilities else "未知"
    precipitation_text = _number(sum(precipitation), 1) if precipitation else "未知"
    wind_text = _number(max(winds), 0) if winds else "未知"
    condition_text = "/".join(descriptions[:3]) if descriptions else "未知"
    return "{}，{}，最高降雨概率{}%，预计降水{}mm，最大风速{}km/h".format(
        condition_text,
        temp_text,
        probability_text,
        precipitation_text,
        wind_text,
    )


def _rain_intervals(points: List[Dict[str, Any]], now_local: datetime) -> List[str]:
    active: List[Dict[str, Any]] = []
    horizon = now_local.replace(tzinfo=None) + timedelta(hours=72)
    lower_bound = now_local.replace(tzinfo=None) - timedelta(minutes=59)
    for point in points:
        point_time = point["time"]
        if point_time < lower_bound or point_time > horizon:
            continue
        probability = float(point.get("precipitation_probability") or 0)
        precipitation = float(point.get("precipitation") or 0)
        if probability >= 20 or precipitation >= 0.1:
            active.append(point)

    if not active:
        return []
    groups: List[List[Dict[str, Any]]] = []
    for point in active:
        if not groups or point["time"] - groups[-1][-1]["time"] > timedelta(hours=1):
            groups.append([point])
        else:
            groups[-1].append(point)

    intervals: List[str] = []
    today = now_local.date()
    for group in groups[:8]:
        start = group[0]["time"]
        end = group[-1]["time"] + timedelta(hours=1)
        probabilities = [float(item.get("precipitation_probability") or 0) for item in group]
        precipitation = [float(item.get("precipitation") or 0) for item in group]
        label = _date_label(start.date(), today)
        intervals.append(
            "{} {}～{}，最高概率{}%，预计降水{}mm".format(
                label,
                start.strftime("%H:%M"),
                end.strftime("%H:%M"),
                _number(max(probabilities), 0),
                _number(sum(precipitation), 1),
            )
        )
    return intervals


def build_weather_context(data: Dict[str, Any], location: Location, now_local: datetime) -> str:
    points = _hourly_points(data)
    current = data.get("current") if isinstance(data.get("current"), dict) else {}
    lines = [
        "[WEATHER_TOOL_RESULT]",
        "status=success",
        "location={} (纬度{:.4f}, 经度{:.4f})".format(
            location.display_name, location.latitude, location.longitude
        ),
        "timezone={}".format(location.timezone),
        "queried_at={}".format(now_local.strftime("%Y-%m-%d %H:%M")),
        "当前：{}，气温{}℃，体感{}℃，降水{}mm，风速{}km/h。".format(
            weather_description(current.get("weather_code")),
            _number(current.get("temperature_2m"), 1),
            _number(current.get("apparent_temperature"), 1),
            _number(current.get("precipitation"), 1),
            _number(current.get("wind_speed_10m"), 1),
        ),
        "分时预报（时间段为当地时间）：",
    ]

    periods = (
        ("凌晨", 0, 6),
        ("上午", 6, 12),
        ("下午", 12, 18),
        ("晚上", 18, 24),
    )
    today = now_local.date()
    for offset in range(3):
        day = today + timedelta(days=offset)
        day_points = [point for point in points if point["time"].date() == day]
        label = _date_label(day, today)
        for period_name, start_hour, end_hour in periods:
            period_points = [
                point for point in day_points if start_hour <= point["time"].hour < end_hour
            ]
            lines.append("- {}{}：{}".format(label, period_name, _period_summary(period_points)))

    intervals = _rain_intervals(points, now_local)
    if intervals:
        lines.append("未来72小时可能降雨时段（概率≥20%或预计降水≥0.1mm）：")
        lines.extend("- " + item for item in intervals)
    else:
        lines.append("未来72小时未发现概率≥20%或预计降水≥0.1mm的降雨时段。")

    lines.extend(
        (
            "回答规则：只依据以上实时工具数据回答；结合用户所问的日期和上午/下午/晚上时段；"
            "没有数据就明确说不知道，不要凭常识补造。",
            "默认地点是{}；只有 location 字段显示其他城市时才回答其他城市。".format(
                _default_location_name(location)
            ),
            # ★2026-07-23 改中文：原文是 "reply in English only … the device's cloned
            # voice is English-only"。那个前提 07-22 起不成立了——设备的 TTS 已切到
            # :8125 中文音色(Qwen3-TTS)。切回英文时这两处要和 SD 的 customEndpoint 一起回退。
            # ★2026-07-24 OWNER: "两三句话" -> "一句话/18字"。实测 Qwen3-TTS 合成
            # wall ≈ 1.34s + 0.69s×字数(RTF≈4)，首字延迟跟字数近似线性。30 字的回复
            # 要等 22 秒才出声。天气回复是日志里最长的一类，先从这里砍。
            # ★2026-10-05: TTS 换成 MiniMax 流式后首包与长度无关，放宽到"一两句、50 字内"，与 cc_brain.robot_rules 一致。
            "回答语言：用提问用的语言回答（默认中文；用英文问就用英文答）。一两句话，一般 50 个字以内，口语化，"
            "不要念工具标签、字段名或英文缩写。",
            "[/WEATHER_TOOL_RESULT]",
        )
    )
    return "\n".join(lines)


def build_weather_failure_context(location_name: str, reason: str) -> str:
    safe_reason = re.sub(r"[^A-Za-z0-9_. -]", "", reason)[:80] or "unavailable"
    return "\n".join(
        (
            "[WEATHER_TOOL_RESULT]",
            "status=unavailable",
            "location={}".format(location_name),
            "reason={}".format(safe_reason),
            "回答规则：实时天气工具暂时不可用。明确告诉用户暂时查不到，不得猜测实时天气。",
            # ★2026-07-23 改中文，同上。
            # ★2026-07-24 OWNER: 同上，"两三句话" -> "一句话/18字"。
            "回答语言：用提问用的语言回答（默认中文；用英文问就用英文答）。一两句话，一般 50 个字以内，口语化。",
            "[/WEATHER_TOOL_RESULT]",
        )
    )


# ★2026-07-23 OWNER: 让设备"认识主人"。固定摘要，不接 OB、不做检索。
# 每次请求都重新读文件 —— 改内容立刻生效不用重启网关，删掉文件就是关闭注入。
# 白名单思路：只写这台小机器人用得上的，不写项目细节/技术栈/关系史。
# ⚠️ 它是开放麦克风，房间里任何人都能问它，写进来的东西等于对在场的人可见。
PROFILE_PATH = Path(__file__).resolve().parent / "owner_profile.txt"


def load_profile() -> str:
    try:
        return PROFILE_PATH.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def inject_system_context(payload: Dict[str, Any], context: str) -> Dict[str, Any]:
    result = copy.deepcopy(payload)
    messages = result.get("messages")
    if not isinstance(messages, list):
        raise ValueError("messages must be a list")
    last_initial_system: Optional[int] = None
    for index, message in enumerate(messages):
        if isinstance(message, dict) and message.get("role") == "system":
            last_initial_system = index
            continue
        break
    if last_initial_system is None:
        messages.insert(0, {"role": "system", "content": context})
        return result
    message = messages[last_initial_system]
    existing = _content_to_text(message.get("content"))
    message["content"] = existing + "\n\n" + context
    return result


class WeatherService:
    def __init__(
        self,
        fetch_json: Callable[[str, int], Dict[str, Any]] = fetch_json_url,
        now_fn: Optional[Callable[[], datetime]] = None,
    ) -> None:
        self._fetch_json = fetch_json
        self._now_fn = now_fn or (lambda: datetime.now(ZoneInfo(DEFAULT_TIMEZONE)))
        self._location_cache = TTLCache()
        self._forecast_cache = TTLCache()

    def resolve_location(self, query: str) -> Location:
        normalized = query.strip()
        if normalized.lower() == "tokyo" or normalized in ("东京", "东京市"):
            return DEFAULT_LOCATION
        cached = self._location_cache.get(normalized.lower())
        if cached is not None:
            return cached
        params = urllib.parse.urlencode(
            {"name": normalized, "count": 8, "language": "zh", "format": "json"}
        )
        data = self._fetch_json(
            "https://geocoding-api.open-meteo.com/v1/search?" + params, 12
        )
        results = data.get("results")
        if not isinstance(results, list) or not results:
            raise LookupError("location_not_found")

        def score(item: Any) -> Tuple[int, int]:
            if not isinstance(item, dict):
                return (9, 9)
            name = str(item.get("name") or "")
            exact = 0 if name.lower() == normalized.lower() else 1
            china = 0 if item.get("country_code") == "CN" else 1
            return (exact, china)

        chosen = min(results, key=score)
        if not isinstance(chosen, dict):
            raise LookupError("location_invalid")
        location = Location(
            query=normalized,
            name=str(chosen.get("name") or normalized),
            latitude=float(chosen["latitude"]),
            longitude=float(chosen["longitude"]),
            admin1=str(chosen.get("admin1") or ""),
            country=str(chosen.get("country") or ""),
            timezone=str(chosen.get("timezone") or DEFAULT_TIMEZONE),
        )
        self._location_cache.set(normalized.lower(), location, 24 * 60 * 60)
        return location

    def forecast(self, location: Location) -> Dict[str, Any]:
        key = "{:.4f},{:.4f}".format(location.latitude, location.longitude)
        cached = self._forecast_cache.get(key)
        if cached is not None:
            return cached
        params = urllib.parse.urlencode(
            {
                "latitude": "{:.4f}".format(location.latitude),
                "longitude": "{:.4f}".format(location.longitude),
                "current": (
                    "temperature_2m,apparent_temperature,precipitation,rain,showers,"
                    "weather_code,wind_speed_10m"
                ),
                "hourly": (
                    "temperature_2m,precipitation_probability,precipitation,rain,showers,"
                    "weather_code,wind_speed_10m"
                ),
                "timezone": location.timezone or DEFAULT_TIMEZONE,
                "forecast_days": 7,
            }
        )
        data = self._fetch_json("https://api.open-meteo.com/v1/forecast?" + params, 15)
        if not isinstance(data.get("hourly"), dict):
            raise ValueError("forecast_missing_hourly")
        self._forecast_cache.set(key, data, 10 * 60)
        return data

    def context_for(self, query: str) -> Tuple[str, Location]:
        location = self.resolve_location(query)
        data = self.forecast(location)
        timezone = ZoneInfo(location.timezone or DEFAULT_TIMEZONE)
        now_local = self._now_fn().astimezone(timezone)
        return build_weather_context(data, location, now_local), location


def load_api_token(path: Path, key: str) -> str:
    in_apikey = False
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if raw_line and not raw_line[0].isspace():
            in_apikey = bool(re.match(r"^apikey\s*:\s*(?:#.*)?$", raw_line))
            continue
        if not in_apikey:
            continue
        match = re.match(r"^\s+{}\s*:\s*(.+?)\s*(?:#.*)?$".format(re.escape(key)), raw_line)
        if not match:
            continue
        value = match.group(1).strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        if value:
            return value
    raise ValueError("apikey.{} is missing from secret config".format(key))


def load_device_token(path: Path) -> str:
    return load_api_token(path, "aiservice")


def load_stt_token(path: Path) -> str:
    return load_api_token(path, "stt")


def robot_log_path() -> Path:
    """robot-log.jsonl location; STACKCHAN_ROBOT_LOG overrides the default."""
    override = os.environ.get("STACKCHAN_ROBOT_LOG", "").strip()
    return Path(override).expanduser() if override else DEFAULT_ROBOT_LOG


@dataclass
class GatewayConfig:
    host: str
    port: int
    upstream_url: str
    upstream_ca: Optional[Path]
    device_token: str = ""
    device_token_sha256: str = ""
    upstream_timeout: int = 70
    stt_upstream_url: str = DEFAULT_STT_UPSTREAM_URL
    stt_token_sha256: str = ""
    stt_timeout: int = 30
    # ★2026-10-04 TTS proxy: upstream URL for POST /v1/audio/speech;
    # env STACKCHAN_TTS_UPSTREAM overrides the default.
    tts_upstream_url: str = DEFAULT_TTS_UPSTREAM_URL
    # ★2026-10-04 photo upload. Directory for stored photos; env
    # STACKCHAN_PHOTO_DIR overrides the default.
    photo_dir: Path = DEFAULT_PHOTO_DIR

    @classmethod
    def from_environment(cls) -> "GatewayConfig":
        token_sha256 = os.environ.get("STACKCHAN_DEVICE_TOKEN_SHA256", "").strip().lower()
        if token_sha256 and not re.fullmatch(r"[0-9a-f]{64}", token_sha256):
            raise ValueError("STACKCHAN_DEVICE_TOKEN_SHA256 must be 64 hexadecimal characters")
        stt_token_sha256 = os.environ.get("STACKCHAN_STT_TOKEN_SHA256", "").strip().lower()
        if not re.fullmatch(r"[0-9a-f]{64}", stt_token_sha256):
            raise ValueError("STACKCHAN_STT_TOKEN_SHA256 must be 64 hexadecimal characters")
        secret_path = Path(
            os.environ.get("STACKCHAN_SECRET_CONFIG", str(DEFAULT_SECRET_CONFIG))
        ).expanduser()
        upstream_url = os.environ.get("STACKCHAN_UPSTREAM_URL", DEFAULT_UPSTREAM_URL)
        ca_env = os.environ.get("STACKCHAN_UPSTREAM_CA")
        if ca_env is None:
            # Not configured: use the repo's SD-staging cert if it exists (relay
            # setup), else none (home-only setup; see GatewayApplication).
            upstream_ca = DEFAULT_UPSTREAM_CA if DEFAULT_UPSTREAM_CA.is_file() else None
        else:
            ca_value = ca_env.strip()
            upstream_ca = Path(ca_value).expanduser() if ca_value else None
        return cls(
            host=os.environ.get("STACKCHAN_GATEWAY_HOST", "0.0.0.0"),
            port=int(os.environ.get("STACKCHAN_GATEWAY_PORT", "8124")),
            upstream_url=upstream_url,
            upstream_ca=upstream_ca,
            device_token="" if token_sha256 else load_device_token(secret_path),
            device_token_sha256=token_sha256,
            upstream_timeout=int(os.environ.get("STACKCHAN_UPSTREAM_TIMEOUT", "70")),
            stt_upstream_url=os.environ.get(
                "STACKCHAN_STT_UPSTREAM_URL", DEFAULT_STT_UPSTREAM_URL
            ),
            stt_token_sha256=stt_token_sha256,
            stt_timeout=int(os.environ.get("STACKCHAN_STT_TIMEOUT", "30")),
            tts_upstream_url=os.environ.get(
                "STACKCHAN_TTS_UPSTREAM", DEFAULT_TTS_UPSTREAM_URL
            ).strip()
            or DEFAULT_TTS_UPSTREAM_URL,
            photo_dir=Path(
                os.environ.get("STACKCHAN_PHOTO_DIR", str(DEFAULT_PHOTO_DIR))
            ).expanduser(),
        )


class GatewayApplication:
    def __init__(self, config: GatewayConfig, weather_service: Optional[WeatherService] = None) -> None:
        self.config = config
        self.weather_service = weather_service or WeatherService()
        # ★2026-10-05 look: the photo upload carries no system prompt, so the
        # device's emoji contract from the last chat request is reused.
        self._last_device_system = ""
        # ★2026-10-27 ears: (text, emotion, event, monotonic time) of the last STT reply.
        self._last_stt: Optional[Tuple[str, Any, Any, float]] = None
        self._upstream_ssl_context: Optional[ssl.SSLContext] = None
        if config.upstream_url.startswith("https://"):
            # The upstream is only the api-fallback path (the brain is Claude Code).
            # With a pinned CA file (the VPS relay's self-signed cert) use it; without
            # one, fall back to the system trust store instead of refusing to start, so
            # a home-only setup with no VPS still boots. A set-but-missing CA path is
            # still a config error and stops startup.
            if config.upstream_ca is None:
                LOGGER.warning("no upstream CA configured; api-fallback uses the system trust store")
                self._upstream_ssl_context = ssl.create_default_context()
            elif not config.upstream_ca.is_file():
                raise FileNotFoundError("upstream CA file is required for HTTPS")
            else:
                self._upstream_ssl_context = ssl.create_default_context(cafile=str(config.upstream_ca))

    def authorized(self, authorization: str) -> bool:
        prefix = "Bearer "
        if not authorization.startswith(prefix):
            return False
        supplied = authorization[len(prefix) :]
        if self.config.device_token_sha256:
            supplied_hash = hashlib.sha256(supplied.encode("utf-8")).hexdigest()
            return hmac.compare_digest(supplied_hash, self.config.device_token_sha256)
        return hmac.compare_digest(supplied, self.config.device_token)

    def authorized_stt(self, authorization: str) -> bool:
        prefix = "Bearer "
        if not authorization.startswith(prefix) or not self.config.stt_token_sha256:
            return False
        supplied = authorization[len(prefix) :]
        supplied_hash = hashlib.sha256(supplied.encode("utf-8")).hexdigest()
        return hmac.compare_digest(supplied_hash, self.config.stt_token_sha256)

    def prepare_request(self, raw_body: bytes) -> Tuple[bytes, bool, str]:
        payload = json.loads(raw_body.decode("utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("request JSON must be an object")
        if payload.get("stream") is True:
            raise ValueError("streaming responses are not supported")
        messages = payload.get("messages")

        # ★2026-07-23 每轮都注入 OWNER 摘要。唤醒词在设备本地处理，
        # 没被叫醒它根本不发请求 —— 所以"叫醒时才注入"= 这里无条件注入。
        profile = load_profile()
        if profile:
            payload = inject_system_context(payload, profile)

        if not is_weather_request(messages):
            if not profile:
                return raw_body, False, ""
            return (
                json.dumps(
                    payload, ensure_ascii=False, separators=(",", ":")
                ).encode("utf-8"),
                False,
                "",
            )

        city = requested_city(messages)
        try:
            context, location = self.weather_service.context_for(city)
            location_name = location.display_name
        except Exception as exc:  # Weather failure must not break ordinary Claude forwarding.
            LOGGER.warning("weather lookup failed type=%s", type(exc).__name__)
            context = build_weather_failure_context(city, type(exc).__name__)
            location_name = city
        enriched = inject_system_context(payload, context)
        return (
            json.dumps(enriched, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
            True,
            location_name,
        )

    def forward(self, body: bytes, authorization: str) -> Tuple[int, str, bytes]:
        request = urllib.request.Request(
            self.config.upstream_url,
            data=body,
            headers={
                "Authorization": authorization,
                "Content-Type": "application/json",
                "Accept": "application/json",
                "User-Agent": "StackChanGateway/1.0",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request,
                timeout=self.config.upstream_timeout,
                context=self._upstream_ssl_context,
            ) as response:
                response_body = response.read(1024 * 1024)
                return response.status, response.headers.get_content_type(), response_body
        except urllib.error.HTTPError as exc:
            return exc.code, exc.headers.get_content_type(), exc.read(1024 * 1024)

    def forward_stt(
        self, body: bytes, authorization: str, content_type: str
    ) -> Tuple[int, str, bytes]:
        request = urllib.request.Request(
            self.config.stt_upstream_url,
            data=body,
            headers={
                "Authorization": authorization,
                "Content-Type": content_type,
                "Accept": "application/json",
                "User-Agent": "StackChanGateway/1.0",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request,
                timeout=self.config.stt_timeout,
            ) as response:
                response_body = response.read(1024 * 1024)
                return response.status, response.headers.get_content_type(), response_body
        except urllib.error.HTTPError as exc:
            return exc.code, exc.headers.get_content_type(), exc.read(1024 * 1024)

    def drop_stt_hallucination(self, response_body: bytes) -> bytes:
        """Blank the text of a known STT hallucination; return the body to relay.

        The device already treats empty text as "didn't catch that". Bodies
        that are not JSON, have no string text, or do not match a pattern are
        returned byte-for-byte unchanged. The dropped text is never logged.
        """
        try:
            result = json.loads(response_body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return response_body
        if not isinstance(result, dict):
            return response_body
        text = result.get("text")
        if not isinstance(text, str) or not stt_hallucination(text):
            return response_body
        LOGGER.info("stt hallucination dropped")
        result = dict(result)
        result["text"] = ""
        return json.dumps(result, ensure_ascii=False).encode("utf-8")

    def remember_stt(self, response_body: bytes, now: Optional[float] = None) -> None:
        """Keep text/emotion/event of an STT reply for the chat request that follows."""
        try:
            result = json.loads(response_body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if not isinstance(result, dict):
            return
        text = str(result.get("text") or "").strip()
        if not text:
            return
        self._last_stt = (
            text,
            result.get("emotion"),
            result.get("event"),
            time.monotonic() if now is None else now,
        )

    def tone_for(self, heard: str, now: Optional[float] = None) -> str:
        """The tone line for this chat text, used once; "" if it is not the last STT text."""
        last, self._last_stt = self._last_stt, None
        if last is None:
            return ""
        text, emotion, event, at = last
        now = time.monotonic() if now is None else now
        if now - at > TONE_WINDOW_SECONDS or "".join(text.split()) != "".join(heard.split()):
            return ""
        LOGGER.info("tone emotion=%s event=%s", emotion or "-", event or "-")
        return tone_hint(emotion, event)

    # ★2026-10-04 photo upload (POST /v1/photos, code-to-muse
    # 20261004-071145-stackchan-camera). The device POSTs raw JPEG bytes
    # (never base64); the photo is stored, never forwarded anywhere.
    @staticmethod
    def validate_photo(body: bytes) -> Optional[str]:
        """Return an error string, or None when the body looks like a JPEG."""
        if len(body) < 4:
            return "too_short"
        if not body.startswith(b"\xff\xd8"):
            return "not_jpeg"
        if not body.endswith(b"\xff\xd9"):
            return "truncated_jpeg"
        return None

    def save_photo(
        self, body: bytes, now: Optional[datetime] = None
    ) -> Tuple[str, int]:
        """Store a validated JPEG; return (relative path, byte count).

        Layout: <photo_dir>/YYYY-MM-DD/HHMMSS.jpg in Mac local time.
        Same-second collisions become HHMMSS-2.jpg, HHMMSS-3.jpg, ...
        Written to a temp file then renamed so a partial write never
        leaves a corrupt photo behind. `now` is injectable for tests.
        """
        # Mac local time.
        moment = now or datetime.now().astimezone()
        day_dir = self.config.photo_dir / moment.strftime("%Y-%m-%d")
        day_dir.mkdir(parents=True, exist_ok=True)
        stem = moment.strftime("%H%M%S")
        candidate = day_dir / (stem + ".jpg")
        suffix = 2
        while candidate.exists():
            candidate = day_dir / ("{}-{}.jpg".format(stem, suffix))
            suffix += 1
        tmp = day_dir / (candidate.name + ".tmp")
        tmp.write_bytes(body)
        tmp.rename(candidate)
        return (str(candidate.relative_to(self.config.photo_dir)), len(body))

    def append_photo_log(self, rel_path: str, nbytes: int) -> None:
        row = {
            "type": "photo",
            "ts": datetime.now().astimezone().isoformat(),
            "path": rel_path,
            "bytes": nbytes,
        }
        log_path = robot_log_path()
        try:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            with log_path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(row, ensure_ascii=False) + "\n")
        except OSError as exc:
            # Logging must never break the 200 response to the device.
            LOGGER.warning("photo log failed type=%s", type(exc).__name__)

    # ★2026-10-04 cc brain mode (STACKCHAN_BRAIN=cc). Answers the chat request
    # from OWNER's open Claude Code window via a forked headless session.
    # Any brain failure falls back to the old VPS path below.
    def _log_robot_safely(
        self,
        heard: str,
        reply: str,
        brain: str,
        session_id: Optional[str],
        latency_ms: Optional[int],
        cost_usd: Optional[float],
        error: Optional[str],
        usage: Optional[Dict[str, Any]] = None,
    ) -> None:
        # cc_brain.append_robot_log never raises on its own; this is a second
        # safety net so a log failure can never break the reply to the device.
        try:
            cc_brain.append_robot_log(
                heard, reply, brain, session_id, latency_ms, cost_usd, error, usage
            )
        except Exception as exc:
            LOGGER.warning("robot log failed type=%s", type(exc).__name__)

    def chat_via_cc(self, raw_body: bytes, authorization: str) -> Tuple[int, str, bytes]:
        payload = json.loads(raw_body.decode("utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("request JSON must be an object")
        messages = payload.get("messages")
        if not isinstance(messages, list):
            raise ValueError("messages must be a list")
        system_text = ""
        for message in messages:
            if isinstance(message, dict) and message.get("role") == "system":
                system_text = _content_to_text(message.get("content"))
                break
        turns = [
            (message.get("role"), _content_to_text(message.get("content")).strip())
            for message in messages
            if isinstance(message, dict) and message.get("role") in ("user", "assistant")
        ]
        turns = [(role, text) for role, text in turns if text]
        if not turns or turns[-1][0] != "user":
            raise ValueError("no user message")
        heard = turns[-1][1]
        history = turns[:-1]

        extra_context = ""
        if is_weather_request(messages):
            city = requested_city(messages)
            try:
                extra_context, _location = self.weather_service.context_for(city)
            except Exception as exc:  # Weather failure must not break the cc path.
                LOGGER.warning("weather lookup failed type=%s", type(exc).__name__)
                extra_context = build_weather_failure_context(city, type(exc).__name__)
        tone = self.tone_for(heard)
        if tone:
            extra_context = (extra_context + "\n" + tone).strip()

        self._last_device_system = system_text
        look = cc_brain.look_enabled()
        try:
            result = cc_brain.answer(heard, system_text, history, extra_context, look=look)
        except cc_brain.CCBrainError as exc:
            error_name = type(exc).__name__
            LOGGER.warning("brain=api-fallback error=%s", error_name)
            upstream_body, _weather_used, _location = self.prepare_request(raw_body)
            # ★2026-10-28: log AFTER the forward returns, so the robot-log row
            # records what the fallback model actually said instead of "".
            # When no reply can be parsed (non-200, non-JSON, no choices),
            # the row keeps "" and the error gains the upstream status, e.g.
            # "CCBadOutput;upstream=502". The device still gets exactly the
            # status/content-type/body that forward() produced.
            # forward() raises when the upstream can't be reached at all (both
            # brains down, e.g. the 10-26 outage); that row matters most.
            try:
                status, content_type, response_body = self.forward(upstream_body, authorization)
            except Exception as forward_exc:
                self._log_robot_safely(
                    heard, "", "api-fallback", None, None, None,
                    "{};upstream={}".format(error_name, type(forward_exc).__name__),
                )
                raise
            reply = _completion_text(response_body) if status == 200 else ""
            log_error = error_name if reply else "{};upstream={}".format(error_name, status)
            self._log_robot_safely(heard, reply, "api-fallback", None, None, None, log_error)
            return status, content_type, response_body

        self._log_robot_safely(
            heard, result.reply, "cc", result.session_id,
            result.latency_ms, result.cost_usd, None, result.usage,
        )
        # Never log what the owner said or the reply text: those go only to robot-log.jsonl.
        LOGGER.info(
            "request brain=cc session=%s latency_ms=%d cache_write=%s cache_read=%s",
            (result.session_id or "")[:8] or "-",
            result.latency_ms,
            (result.usage or {}).get("cache_creation_input_tokens"),
            (result.usage or {}).get("cache_read_input_tokens"),
        )
        # robot-log keeps the raw reply (look_via_cc finds the [[look]] there);
        # the device only gets the tag while the look switch is on.
        device_reply = result.reply if look else cc_brain.strip_look_tags(result.reply)
        completion = {
            "id": "chatcmpl-cc-" + uuid.uuid4().hex[:12],
            "object": "chat.completion",
            "created": int(time.time()),
            "model": "stackchan-cc-brain",
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": device_reply or "嗯。"},
                    "finish_reason": "stop",
                }
            ],
            "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
        }
        return 200, "application/json", json.dumps(completion, ensure_ascii=False).encode("utf-8")


    # ★2026-10-05 look, second round. The device took the photo because my
    # last reply ended in [[look]]; ask the same window again, with the photo.
    LOOK_WINDOW_SECONDS = 300
    LOOK_HISTORY_ROWS = 8

    def _recent_cc_rows(self) -> List[Dict[str, Any]]:
        """Chat rows from the tail of robot-log.jsonl, oldest first."""
        path = robot_log_path()
        try:
            with path.open("rb") as handle:
                handle.seek(0, os.SEEK_END)
                size = handle.tell()
                handle.seek(max(0, size - 256 * 1024))
                tail = handle.read().decode("utf-8", "replace")
        except OSError:
            return []
        rows = []
        for line in tail.splitlines():
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if isinstance(row, dict) and row.get("brain") in ("cc", "cc-look"):
                rows.append(row)
        return rows

    def look_via_cc(self, jpeg: bytes) -> Tuple[str, Optional[str]]:
        """Return (reply, error_name). reply is "" when there is nothing to say."""
        if not (_cc_brain_enabled() and cc_brain.look_enabled()):
            return "", "look_disabled"
        rows = self._recent_cc_rows()
        trigger_index = None
        for index in range(len(rows) - 1, -1, -1):
            if cc_brain.has_look_tag(str(rows[index].get("reply") or "")):
                trigger_index = index
                break
        if trigger_index is None:
            return "", "no_look_request"
        trigger = rows[trigger_index]
        try:
            asked_at = datetime.fromisoformat(str(trigger.get("ts")))
            age = (datetime.now().astimezone() - asked_at).total_seconds()
        except (TypeError, ValueError):
            age = None
        if age is None or age > self.LOOK_WINDOW_SECONDS:
            return "", "look_request_expired"
        heard = str(trigger.get("heard") or "").strip()
        history: List[Tuple[str, str]] = []
        for row in rows[max(0, trigger_index - self.LOOK_HISTORY_ROWS) : trigger_index]:
            if row.get("heard"):
                history.append(("user", str(row["heard"])))
            if row.get("reply"):
                history.append(("assistant", cc_brain.strip_look_tags(str(row["reply"]))))
        try:
            result = cc_brain.answer(
                heard,
                self._last_device_system,
                history,
                cc_brain.LOOK_FOLLOWUP_CONTEXT,
                image_jpeg=jpeg,
                look=False,
            )
        except cc_brain.CCBrainError as exc:
            error_name = type(exc).__name__
            LOGGER.warning("look failed error=%s", error_name)
            self._log_robot_safely(heard, "", "cc-look", None, None, None, error_name)
            return "", error_name
        # One look per question: a second tag in the answer is dropped.
        reply = cc_brain.strip_look_tags(result.reply)
        self._log_robot_safely(
            heard, reply, "cc-look", result.session_id,
            result.latency_ms, result.cost_usd, None, result.usage,
        )
        LOGGER.info(
            "request look session=%s latency_ms=%d cache_write=%s cache_read=%s",
            (result.session_id or "")[:8] or "-",
            result.latency_ms,
            (result.usage or {}).get("cache_creation_input_tokens"),
            (result.usage or {}).get("cache_read_input_tokens"),
        )
        return reply, None


def make_handler(application: GatewayApplication) -> Any:
    class GatewayHandler(BaseHTTPRequestHandler):
        server_version = "StackChanGateway/1.0"
        # ★2026-10-04 TTS proxy depends on this: HTTP/1.0 (the class default,
        # now spelled out) never uses chunked framing, and the server closes
        # the connection after each request, which is what makes the
        # close-delimited TTS response well-defined without Content-Length.
        protocol_version = "HTTP/1.0"

        def log_message(self, _format: str, *_args: Any) -> None:
            return

        def _send(self, status: int, content_type: str, body: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _send_json(self, status: int, payload: Dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self._send(status, "application/json; charset=utf-8", body)

        def _proxy_tts(
            self,
            application: GatewayApplication,
            raw_body: bytes,
            content_type: str,
            started: float,
            peer: str,
        ) -> None:
            """Stream POST /v1/audio/speech to the TTS upstream and back.

            The response is close-delimited: only Content-Type (from the
            upstream) and Connection: close are sent, never Content-Length
            or Transfer-Encoding. The body is relayed in small chunks with
            a flush after each one until the upstream closes its side, so
            the device gets the first audio bytes without waiting for the
            whole utterance.
            """
            upstream = application.config.tts_upstream_url
            parts = urllib.parse.urlparse(upstream)
            if parts.scheme not in ("http", "https") or not parts.hostname:
                LOGGER.error("tts bad upstream url peer=%s", peer)
                self._send_json(502, {"error": "upstream_unavailable"})
                return
            if parts.scheme == "https":
                connection = http.client.HTTPSConnection(
                    parts.hostname, parts.port or 443, timeout=TTS_CONNECT_TIMEOUT
                )
            else:
                connection = http.client.HTTPConnection(
                    parts.hostname, parts.port or 80, timeout=TTS_CONNECT_TIMEOUT
                )
            upstream_path = parts.path or TTS_PATH
            if parts.query:
                upstream_path += "?" + parts.query
            try:
                connection.connect()  # the 5 s connect timeout applies here
                connection.sock.settimeout(TTS_READ_TIMEOUT)  # read timeout below
                connection.putrequest("POST", upstream_path)
                connection.putheader(
                    "Content-Type", content_type or "application/octet-stream"
                )
                connection.putheader("Content-Length", str(len(raw_body)))
                connection.putheader("User-Agent", "StackChanGateway/1.0")
                # Deliberately NOT forwarding the device's Authorization header.
                connection.endheaders()
                connection.send(raw_body)
                upstream_response = connection.getresponse()
            except (OSError, http.client.HTTPException) as exc:
                LOGGER.error("tts upstream connect failure type=%s", type(exc).__name__)
                self._send_json(502, {"error": "upstream_unavailable"})
                connection.close()
                return
            first_byte_ms: Optional[int] = None
            total_bytes = 0
            try:
                # protocol_version is HTTP/1.0: send_response / end_headers
                # never synthesize Transfer-Encoding, and with no
                # Content-Length the client reads until the connection closes.
                self.send_response(upstream_response.status)
                self.send_header(
                    "Content-Type",
                    upstream_response.getheader(
                        "Content-Type", "application/octet-stream"
                    ),
                )
                self.send_header("Connection", "close")
                self.end_headers()
                while True:
                    chunk = upstream_response.read(TTS_CHUNK_BYTES)
                    if not chunk:
                        break
                    if first_byte_ms is None:
                        first_byte_ms = int((time.monotonic() - started) * 1000)
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    total_bytes += len(chunk)
            except (OSError, http.client.HTTPException) as exc:
                # Already streaming to the device: just close the connection.
                LOGGER.warning(
                    "tts upstream stream failure type=%s bytes=%d",
                    type(exc).__name__,
                    total_bytes,
                )
                self.close_connection = True
                connection.close()
                return
            duration_ms = int((time.monotonic() - started) * 1000)
            # One line: byte count, first-byte latency, total latency.
            # Never the key or the Authorization header.
            LOGGER.info(
                "request tts upstream_status=%d bytes=%d first_byte_ms=%d duration_ms=%d",
                upstream_response.status,
                total_bytes,
                first_byte_ms if first_byte_ms is not None else -1,
                duration_ms,
            )
            connection.close()
            self.close_connection = True

        def _read_photo_body(self, length: int) -> Tuple[bytes, Optional[str]]:
            """Read exactly `length` photo body bytes, with accounting.

            Returns (body, None) on success, else (partial_body, reason)
            with reason "truncated" (client closed early), "read_timeout"
            (body stalled past PHOTO_READ_TIMEOUT_SECONDS), or
            "read_failed" (connection reset mid-body). The handler is
            HTTP/1.0, so the connection closes after this request and
            putting a timeout on the socket is safe. Before 2026-10-28
            this was a bare rfile.read(length): a stalled upload blocked
            the handler thread forever and left no outcome line in the
            log at all (2026-10-25 16:34:47), and an early close was
            logged as a bare reason with no byte counts.
            """
            self.connection.settimeout(PHOTO_READ_TIMEOUT_SECONDS)
            # read1, not read: read(n) would keep buffering until n bytes
            # or EOF and lose the partial count when the timeout fires.
            read_chunk = getattr(self.rfile, "read1", self.rfile.read)
            chunks: List[bytes] = []
            remaining = length
            try:
                while remaining > 0:
                    chunk = read_chunk(min(65536, remaining))
                    if not chunk:
                        break
                    chunks.append(chunk)
                    remaining -= len(chunk)
            except (socket.timeout, TimeoutError):
                return b"".join(chunks), "read_timeout"
            except OSError:
                return b"".join(chunks), "read_failed"
            body = b"".join(chunks)
            if len(body) < length:
                return body, "truncated"
            return body, None

        def do_GET(self) -> None:
            if self.path != "/health":
                self._send_json(404, {"error": "not_found"})
                return
            self._send_json(200, {"status": "ok", "service": "stackchan-gateway"})

        def do_POST(self) -> None:
            started = time.monotonic()
            peer = self.client_address[0] if self.client_address else "unknown"
            LOGGER.info("incoming peer=%s path=%s", peer, self.path)
            if self.path == STT_PATH:
                authorization = self.headers.get("Authorization", "")
                if not application.authorized_stt(authorization):
                    LOGGER.warning("stt request rejected peer=%s reason=unauthorized", peer)
                    self._send_json(401, {"error": "unauthorized"})
                    return
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError:
                    self._send_json(400, {"error": "invalid_content_length"})
                    return
                if length <= 0 or length > MAX_STT_REQUEST_BYTES:
                    self._send_json(413, {"error": "request_too_large"})
                    return
                content_type = self.headers.get("Content-Type", "")
                if not content_type.lower().startswith("multipart/form-data;"):
                    self._send_json(415, {"error": "unsupported_media_type"})
                    return
                raw_body = self.rfile.read(length)
                try:
                    status, response_type, response_body = application.forward_stt(
                        raw_body, authorization, content_type
                    )
                except Exception as exc:
                    LOGGER.error("stt upstream failure type=%s", type(exc).__name__)
                    self._send_json(502, {"error": "upstream_unavailable"})
                    return
                if status == 200:
                    # Filter before remembering, so a dropped hallucination
                    # never reaches the tone memory (its text is "" anyway).
                    response_body = application.drop_stt_hallucination(response_body)
                    try:
                        application.remember_stt(response_body)
                    except Exception as exc:  # The tone is extra; never break the STT reply.
                        LOGGER.warning("stt tone parse failed type=%s", type(exc).__name__)
                duration_ms = int((time.monotonic() - started) * 1000)
                LOGGER.info(
                    "request stt upstream_status=%d duration_ms=%d request_bytes=%d",
                    status,
                    duration_ms,
                    length,
                )
                # Whisper's legacy response reader terminates body reads on CR.
                # A trailing CR is valid JSON whitespace and prevents its Stream
                # timeout (expressed in seconds by this ESP32 framework) from
                # looking like a permanent wait.
                self._send(status, response_type or "application/json", response_body + b"\r")
                return
            # ★2026-10-04 photo upload (code-to-muse 20261004-071145-stackchan-camera).
            # Same chat auth; raw JPEG bytes in, file on disk + robot-log row out.
            if self.path == PHOTOS_PATH:
                authorization = self.headers.get("Authorization", "")
                if not application.authorized(authorization):
                    LOGGER.warning("photo request rejected peer=%s reason=unauthorized", peer)
                    self._send_json(401, {"error": "unauthorized"})
                    return
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError:
                    self._send_json(400, {"error": "invalid_content_length"})
                    return
                if length <= 0 or length > MAX_PHOTO_BYTES:
                    self._send_json(413, {"error": "request_too_large"})
                    return
                content_type = self.headers.get("Content-Type", "")
                if content_type.split(";")[0].strip().lower() != "image/jpeg":
                    self._send_json(415, {"error": "unsupported_media_type"})
                    return
                raw_body, read_error = self._read_photo_body(length)
                if read_error is not None:
                    # Body never fully arrived (client closed early, stalled
                    # past the read timeout, or the connection reset). Log
                    # both numbers; never the image bytes.
                    LOGGER.warning(
                        "photo request rejected peer=%s reason=%s got=%d expected=%d",
                        peer,
                        read_error,
                        len(raw_body),
                        length,
                    )
                    # 408, not 400: the firmware retries 408 (and 5xx/transport errors)
                    # but gives up on other 4xx, and an incomplete body is exactly the
                    # upload worth resending.
                    self._send_json(408, {"error": read_error})
                    return
                error = application.validate_photo(raw_body)
                if error is not None:
                    # Invalid JPEG: reject before anything touches the disk.
                    LOGGER.warning(
                        "photo request rejected peer=%s reason=%s got=%d expected=%d",
                        peer,
                        error,
                        len(raw_body),
                        length,
                    )
                    self._send_json(400, {"error": error})
                    return
                try:
                    rel_path, nbytes = application.save_photo(raw_body)
                except OSError as exc:
                    LOGGER.error("photo save failed type=%s", type(exc).__name__)
                    self._send_json(500, {"error": "save_failed"})
                    return
                application.append_photo_log(rel_path, nbytes)
                duration_ms = int((time.monotonic() - started) * 1000)
                # Log only the byte count and the relative path, never the pixels.
                LOGGER.info(
                    "request photo path=%s bytes=%d duration_ms=%d",
                    rel_path,
                    nbytes,
                    duration_ms,
                )
                response = {"ok": True, "path": rel_path}
                # ★2026-10-05 look: the device sends this header only when it
                # took the photo because my reply ended in [[look]]. The
                # answer rides back on this response; the device speaks it.
                if self.headers.get("X-StackChan-Look", "").strip() == "1":
                    reply, look_error = application.look_via_cc(raw_body)
                    response["reply"] = reply
                    if look_error:
                        response["look_error"] = look_error
                self._send_json(200, response)
                return
            # ★2026-10-04 TTS proxy (code-to-muse 20261004-134159-travel-gateway-tts-proxy).
            # Same device key as chat; the request body goes to the TTS
            # service verbatim and the audio streams back close-delimited
            # (no Content-Length, no Transfer-Encoding) because the device
            # reads the raw TCP stream and does not decode chunked framing.
            if self.path == TTS_PATH:
                authorization = self.headers.get("Authorization", "")
                if not application.authorized(authorization):
                    LOGGER.warning("tts request rejected peer=%s reason=unauthorized", peer)
                    self._send_json(401, {"error": "unauthorized"})
                    return
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError:
                    self._send_json(400, {"error": "invalid_content_length"})
                    return
                if length <= 0 or length > MAX_TTS_REQUEST_BYTES:
                    self._send_json(413, {"error": "request_too_large"})
                    return
                tts_content_type = self.headers.get("Content-Type", "")
                raw_body = self.rfile.read(length)
                self._proxy_tts(application, raw_body, tts_content_type, started, peer)
                return
            if self.path != CHAT_PATH:
                self._send_json(404, {"error": "not_found"})
                return
            authorization = self.headers.get("Authorization", "")
            if not application.authorized(authorization):
                LOGGER.warning("request rejected peer=%s reason=unauthorized", peer)
                self._send_json(401, {"error": "unauthorized"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self._send_json(400, {"error": "invalid_content_length"})
                return
            if length <= 0 or length > MAX_REQUEST_BYTES:
                self._send_json(413, {"error": "request_too_large"})
                return
            raw_body = self.rfile.read(length)
            try:
                if _cc_brain_enabled():
                    # ★2026-10-04 cc brain mode; falls back to the old path
                    # inside chat_via_cc when the brain fails.
                    status, content_type, response_body = application.chat_via_cc(
                        raw_body, authorization
                    )
                    weather_used, location = False, ""
                else:
                    upstream_body, weather_used, location = application.prepare_request(raw_body)
                    status, content_type, response_body = application.forward(
                        upstream_body, authorization
                    )
            except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
                self._send_json(400, {"error": type(exc).__name__})
                return
            except Exception as exc:
                LOGGER.error("upstream failure type=%s", type(exc).__name__)
                self._send_json(502, {"error": "upstream_unavailable"})
                return
            duration_ms = int((time.monotonic() - started) * 1000)
            LOGGER.info(
                "request weather=%s location=%s upstream_status=%d duration_ms=%d",
                weather_used,
                location or "-",
                status,
                duration_ms,
            )
            self._send(status, content_type or "application/json", response_body)

    return GatewayHandler


def main() -> int:
    logging.basicConfig(
        level=os.environ.get("STACKCHAN_GATEWAY_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    try:
        config = GatewayConfig.from_environment()
        application = GatewayApplication(config)
    except Exception as exc:
        # Type only, never str(exc): a config error message could carry a secret.
        # FileNotFoundError here almost always means STACKCHAN_UPSTREAM_CA points
        # at a file that does not exist.
        LOGGER.error("startup failed type=%s", type(exc).__name__)
        return 1
    server = ThreadingHTTPServer((config.host, config.port), make_handler(application))
    LOGGER.info("listening host=%s port=%d", config.host, config.port)
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
