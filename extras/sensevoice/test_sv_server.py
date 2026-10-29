"""sv_server.py 的单测 —— 不依赖真模型。

来历：Muse 10-27 复核时写（code-to-muse 任务 20261027-161406-sv-server-py），
我读过全文后拷来；修好 multipart 之后，下面说的两条「故意失败」已经转绿，
最后一条「截断后报错」改写成了验证修复后的行为。

运行（Mac 上，在 sv_server.py 所在目录）：
    .venv/bin/python -m pytest test_sv_server.py -v
（.venv 里若没有 pytest，先装 pytest 即可，不涉及模型下载。）

原理：import sv_server 之前先把 sys.modules["sherpa_onnx"] 换成桩，
这样 sv_server 顶层的 import sherpa_onnx 不会去碰真模型；
每条测试再用桩控制 VAD 段长、识别结果，断言 sv_server 自身的逻辑。

注意：下面有 2 条测试在当前 sv_server.py 上是故意失败的，
它们就是复核发现的 bug 的证据（见文件头注释和结果文件）：
  * test_multipart_trailing_0x0a_preserved —— sv_server.py:96 的 rstrip
  * test_multipart_boundary_inside_payload —— sv_server.py:87 的裸 split
其余测试在当前代码上应全部通过。
"""

import io
import os
import struct
import sys
import types
import wave

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# 桩 sherpa_onnx：必须在 import sv_server 之前装好
# ---------------------------------------------------------------------------


class _FakeSileroVad:
    def __init__(self):
        self.model = ""
        self.min_silence_duration = 0.25
        self.min_speech_duration = 0.15
        self.window_size = 512  # 与真实默认一致：16k 下 32ms


class _FakeVadModelConfig:
    def __init__(self):
        self.silero_vad = _FakeSileroVad()
        self.sample_rate = 16000


class _FakeSegment:
    def __init__(self, n_samples):
        self.samples = np.zeros(n_samples, dtype=np.float32)


class _FakeVAD:
    """测试通过 segments_to_emit 控制它吐出的说话段（每段的采样点数）。

    speech_seconds() 的循环逻辑（flush 后逐段 pop 求和）走的是真代码，
    桩只决定 VAD 吐出什么段，不决定怎么求和。
    """

    segments_to_emit = []

    def __init__(self, config, buffer_size_in_seconds=60):
        self._queue = [_FakeSegment(n) for n in type(self).segments_to_emit]

    def accept_waveform(self, samples):
        pass

    def flush(self):
        pass

    def empty(self):
        return not self._queue

    @property
    def front(self):
        return self._queue[0]

    def pop(self):
        self._queue.pop(0)


class _FakeResult:
    def __init__(self, text, lang, emotion, event):
        self.text = text
        self.lang = lang
        self.emotion = emotion
        self.event = event


class _FakeStream:
    def __init__(self, result):
        self.result = result

    def accept_waveform(self, sr, audio):
        pass


class _FakeRecognizer:
    def __init__(self, result):
        self._result = result
        self.calls = 0

    def create_stream(self):
        self.calls += 1
        return _FakeStream(self._result)

    def decode_stream(self, stream):
        pass


_fake_sherpa = types.ModuleType("sherpa_onnx")
_fake_sherpa.VadModelConfig = _FakeVadModelConfig
_fake_sherpa.VoiceActivityDetector = _FakeVAD
sys.modules["sherpa_onnx"] = _fake_sherpa

import sv_server  # noqa: E402


@pytest.fixture(autouse=True)
def _reset_vad_segments():
    _FakeVAD.segments_to_emit = []
    yield
    _FakeVAD.segments_to_emit = []


def _recognizer(text="你好", lang="<|zh|>", emotion="<|HAPPY|>", event="<|Laughter|>"):
    return _FakeRecognizer(_FakeResult(text, lang, emotion, event))


def _setup(monkeypatch, segments, recognizer=None):
    """定好 VAD 段长和识别器桩；门槛显式钉在 0.3s，避免受环境变量影响。"""
    _FakeVAD.segments_to_emit = list(segments)
    monkeypatch.setattr(sv_server, "MIN_SPEECH_SEC", 0.3)
    monkeypatch.setattr(sv_server, "_recognizer", recognizer or _recognizer())


def _make_wav(samples, sr=16000, ch=1):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(sr)
        frames = b"".join(struct.pack("<h", s) for s in samples)
        w.writeframes(frames)
    return buf.getvalue()


def _make_multipart(file_bytes, boundary=b"BOUNDARY", extra_fields=()):
    parts = []
    for name, value in extra_fields:
        parts.append(
            b"--" + boundary + b'\r\nContent-Disposition: form-data; name="'
            + name.encode() + b'"\r\n\r\n' + value + b"\r\n"
        )
    parts.append(
        b"--" + boundary + b'\r\nContent-Disposition: form-data; name="file"; '
        b'filename="a.wav"\r\nContent-Type: audio/wav\r\n\r\n' + file_bytes + b"\r\n"
    )
    return b"".join(parts) + b"--" + boundary + b"--\r\n"


def _content_type(boundary=b"BOUNDARY"):
    return "multipart/form-data; boundary=" + boundary.decode()


# ---------------------------------------------------------------------------
# VAD 门 + 识别（任务要求 1、2）
# ---------------------------------------------------------------------------


def test_short_speech_returns_no_speech_and_recognizer_not_called(monkeypatch):
    rec = _recognizer()
    _setup(monkeypatch, segments=[1600], recognizer=rec)  # 0.1s < 0.3s
    out = sv_server.transcribe(np.zeros(16000, dtype=np.float32))
    assert out["text"] == ""
    assert out["event"] == "no_speech"
    assert out["speech_sec"] == pytest.approx(0.1)
    assert rec.calls == 0


def test_zero_speech_returns_no_speech(monkeypatch):
    rec = _recognizer()
    _setup(monkeypatch, segments=[], recognizer=rec)
    out = sv_server.transcribe(np.zeros(16000, dtype=np.float32))
    assert out == {"text": "", "lang": "", "emotion": "", "event": "no_speech",
                   "speech_sec": 0.0}
    assert rec.calls == 0


def test_speech_calls_recognizer_and_strips_tags(monkeypatch):
    rec = _recognizer()
    _setup(monkeypatch, segments=[8000], recognizer=rec)  # 0.5s >= 0.3s
    out = sv_server.transcribe(np.zeros(16000, dtype=np.float32))
    assert rec.calls == 1
    assert out["text"] == "你好"
    assert out["lang"] == "zh"          # "<|zh|>" -> "zh"
    assert out["emotion"] == "HAPPY"    # "<|HAPPY|>" -> "HAPPY"
    assert out["event"] == "Laughter"   # "<|Laughter|>" -> "Laughter"
    assert out["speech_sec"] == pytest.approx(0.5)


def test_exactly_min_speech_calls_recognizer(monkeypatch):
    """边界：说话恰好 0.3s 时门应放行（speech < 0.3 为假）。

    这条就是反例测试的靶子：把 sv_server.py:129 的 `<` 改成 `<=`，
    它立刻失败（见改坏后运行输出）。
    """
    rec = _recognizer()
    _setup(monkeypatch, segments=[4800], recognizer=rec)  # 4800/16000 = 0.3s
    out = sv_server.transcribe(np.zeros(16000, dtype=np.float32))
    assert rec.calls == 1
    assert out["event"] != "no_speech"


def test_speech_seconds_sums_segments_without_double_count(monkeypatch):
    _setup(monkeypatch, segments=[1600, 3200])
    assert sv_server.speech_seconds(np.zeros(16000, dtype=np.float32)) == pytest.approx(0.3)


# ---------------------------------------------------------------------------
# multipart（任务要求 2）
# ---------------------------------------------------------------------------


def test_multipart_normal():
    wav = _make_wav([0, 1000, -1000, 12345])
    body = _make_multipart(wav, extra_fields=[("language", b"zh")])
    fields = sv_server.parse_multipart(body, _content_type())
    assert fields["file"] == wav
    assert fields["language"] == b"zh"


def test_multipart_missing_file():
    body = (
        b'--BOUNDARY\r\nContent-Disposition: form-data; name="language"'
        b'\r\n\r\nzh\r\n--BOUNDARY--\r\n'
    )
    fields = sv_server.parse_multipart(body, _content_type())
    assert "file" not in fields


def test_multipart_missing_boundary():
    with pytest.raises(ValueError, match="missing multipart boundary"):
        sv_server.parse_multipart(b"--X\r\n\r\n", "multipart/form-data")
    with pytest.raises(ValueError, match="missing multipart boundary"):
        sv_server.parse_multipart(b"--X\r\n\r\n", "")


def test_multipart_trailing_0x0a_preserved():
    """当前代码上应失败：见 sv_server.py:96。

    末尾采样 0x0a00 的字节是 00 0a，WAV 最后一个字节恰好是 0x0a；
    rstrip(b"\\r\\n") 会把这个真实音频字节连同分隔符 CRLF 一起吃掉，
    解析出的文件比发来的短 1 字节。
    """
    wav = _make_wav([0, 1000, -1000, 0x0A00])
    assert wav[-1:] == b"\x0a"
    body = _make_multipart(wav)
    fields = sv_server.parse_multipart(body, _content_type())
    assert fields["file"] == wav


def test_multipart_boundary_inside_payload():
    """当前代码上应失败：见 sv_server.py:87。

    触发条件是音频数据里恰好出现界定符的完整字节串（这里用短 boundary
    BB 演示：采样 0x2d2d('--') + 0x4242('BB') 拼出 '--BB'）。
    真实客户端的 boundary 通常是长随机串，实际概率极低，
    但解析器按构造就不 robust：它按裸 boundary 切分，
    而不是只认 CRLF + boundary 这种分隔符位置。
    """
    wav = _make_wav([0x2D2D, 0x4242, 100, 200])
    assert b"--BB" in wav
    body = _make_multipart(wav, boundary=b"BB")
    fields = sv_server.parse_multipart(body, "multipart/form-data; boundary=BB")
    assert fields["file"] == wav


# ---------------------------------------------------------------------------
# decode_audio（附加：复核发现的回退与声道/重采样行为）
# ---------------------------------------------------------------------------


def test_decode_resamples_8k_to_16k():
    wav = _make_wav([0, 10000, -10000, 5000] * 2000, sr=8000)  # 1s @ 8k
    out = sv_server.decode_audio(wav)
    assert out.dtype == np.float32
    assert len(out) == 16000


def test_decode_stereo_downmix():
    # 交错立体声：左 10000、右 -10000 交替，混成单声道后应约 0
    samples = []
    for _ in range(8000):
        samples += [10000, -10000]
    wav = _make_wav(samples, ch=2)
    out = sv_server.decode_audio(wav)
    assert len(out) == 8000
    assert np.abs(out).max() < 1e-6


def test_decode_garbage_falls_back_silently():
    """当前行为的定格：非 WAV 输入不报错，被当成裸 PCM 解出数组。

    这就是复核发现的问题（sv_server.py:59-62）：回退把错误输入
    静默变成噪声转写，而不是返回 400。测试通过 = 行为被确认，
    不是说这个行为是对的。
    """
    out = sv_server.decode_audio(b"\x00\x01" * 100)
    assert isinstance(out, np.ndarray)
    assert len(out) == 78  # (200 - 44) / 2


def test_trailing_0x0a_wav_decodes_end_to_end():
    """（10-27 修复后改写）原测试证明 rstrip 截掉 1 字节 → decode ValueError；
    修好后应是：解析出的文件与原文件一致，decode 出完整的 4 个采样、不报错。
    """
    wav = _make_wav([0, 1000, -1000, 0x0A00])
    parsed = sv_server.parse_multipart(_make_multipart(wav), _content_type())["file"]
    assert parsed == wav
    assert len(sv_server.decode_audio(parsed)) == 4
