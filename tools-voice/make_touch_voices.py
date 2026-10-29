#!/usr/bin/env python3
"""StackChan 触摸反馈的预合成句库生成器。

为什么要预合成：设备上 robot->speech() 走 Mac 的克隆音色 server，实测首字延迟
4~8 秒。摸一下头、八秒后才出声，那不是慢，是坏了。所以触摸反馈一律播 SD 上
已经合成好的 WAV，设备侧出声 <0.5s。

用法：
    python3 tools-voice/make_touch_voices.py              # 生成缺的
    python3 tools-voice/make_touch_voices.py --force      # 全部重新合成
    python3 tools-voice/make_touch_voices.py --only pet   # 只做某一类

零成本：打的是本机 :8125（Qwen3 克隆音色，launchd 常驻），不经过任何计费 API。

⚠️ 两件必须做对的事，都在 write_wav() 里：
  1) server 发的是**流式** WAV 头（RIFF/data 长度都是 0xFFFFFFFF，见
     tts_stream_server.py:streaming_wav_header），直接存盘会让设备端
     AudioGeneratorWAV 按 4GB 去读。这里按实际字节数重写头。
  2) 走 http.client 直连 127.0.0.1，不碰系统代理——localhost 被 Clash 劫过
     （07-25 那次 OB 的 502）。
"""

import argparse
import array
import http.client
import json
import math
import os
import re
import struct
import sys

TTS_HOST = "127.0.0.1"
TTS_PORT = 8125                  # 中文轨（英文轨是 8123，触摸台词是中文）
SAMPLE_RATE = 24000              # tts_stream_server.py:103，mono/16bit
CHANNELS = 1
BITS = 16

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHRASE_FILE = os.path.join(REPO, "tools-voice", "touch_phrases.txt")
OUT_ROOT = os.path.join(REPO, "SD-staging", "app", "AiStackChanEx", "voice")

# 设备侧 VoiceClip.h 里的目录名，改这里要同步改那边
CATEGORIES = ("pat", "pet", "pet_back", "shake")


def parse_phrases(path):
    """读台词文件 -> {分类: [句子, ...]}"""
    groups = {}
    current = None
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = re.fullmatch(r"\[(\w+)\]", line)
            if m:
                current = m.group(1)
                groups.setdefault(current, [])
                continue
            if current is None:
                print(f"warn: 台词 {line!r} 出现在任何 [分类] 之前，跳过", file=sys.stderr)
                continue
            groups[current].append(line)
    return groups


def synth(text):
    """打本机 TTS server，拿回裸 PCM（已剥掉流式 WAV 头）。"""
    conn = http.client.HTTPConnection(TTS_HOST, TTS_PORT, timeout=180)
    body = json.dumps({"input": text}).encode("utf-8")
    conn.request("POST", "/v1/audio/speech", body,
                 {"Content-Type": "application/json"})
    resp = conn.getresponse()
    if resp.status != 200:
        conn.close()
        raise RuntimeError(f"TTS {resp.status} {resp.reason}")
    data = resp.read()          # close-delimited，读到 EOF
    conn.close()

    # 剥掉 44 字节的流式头。校验一下别默默拿到别的东西。
    if not data.startswith(b"RIFF") or data[8:12] != b"WAVE":
        raise RuntimeError("返回的不是 WAV")
    idx = data.find(b"data")
    if idx < 0:
        raise RuntimeError("WAV 里找不到 data chunk")
    return data[idx + 8:]


FADE_OUT_MS = 25
TAIL_SILENCE_MS = 60

# ---- 音色漂移的粗筛 ------------------------------------------------------
# 克隆音色偶尔会漂成女声（2026-07-26 实测 pat/01 就漂了一次，中位 F0 223Hz，
# 而全批中位是 139Hz）。这里用自相关估基频，漂得离谱就重抽。
#
# ⚠️ 这是**粗筛不是判官**，别当它能替耳朵：
#   - 笑声/疑问句本来就高（同一批里「嘿嘿。」187Hz、「毛都乱了。」198Hz，
#     都是对的音色）；
#   - 短句有声帧少（<20 帧）时估计本来就抖，还可能倍频错误。
#   所以阈值定得很松，只拦 pat/01 那种明显的，剩下的照样得你听。
F0_REROLL_ABOVE_HZ = 210
F0_MIN_VOICED_FRAMES = 30
F0_MAX_TRIES = 3


def median_f0(pcm, fmin=70, fmax=350):
    """(中位 F0, 有声帧数)。纯 stdlib，不引 numpy——这脚本要能用系统 python3 跑。"""
    a = array.array("h")
    a.frombytes(pcm)
    win, hop = int(SAMPLE_RATE * 0.04), int(SAMPLE_RATE * 0.01)
    lo, hi = int(SAMPLE_RATE / fmax), int(SAMPLE_RATE / fmin)
    f0s = []
    for s in range(0, max(0, len(a) - win), hop):
        seg = [v / 32768.0 for v in a[s:s + win]]
        rms = math.sqrt(sum(v * v for v in seg) / len(seg))
        if rms < 0.05:
            continue                                   # 只看有声帧
        mean = sum(seg) / len(seg)
        seg = [v - mean for v in seg]
        e0 = sum(v * v for v in seg)
        if e0 <= 0:
            continue
        best_k, best_r = 0, 0.0
        for k in range(lo, min(hi, len(seg))):
            r = sum(seg[i] * seg[i + k] for i in range(0, len(seg) - k, 2))
            if r > best_r:
                best_r, best_k = r, k
        if best_k and best_r / (e0 / 2) > 0.35:        # /2 因为上面隔一个点采样
            f0s.append(SAMPLE_RATE / best_k)
    if not f0s:
        return float("nan"), 0
    f0s.sort()
    return f0s[len(f0s) // 2], len(f0s)


def soften_tail(pcm):
    """收尾：25ms 淡出 + 60ms 静音。

    实测 18 句里有 7 句结尾幅度 >0.10（最高 0.43，"我要散架了。"），也就是模型
    还在发声就停了、server 的 trim_model_silence 没得可裁。这种硬切在设备喇叭上
    会爆一下"喀"。淡出治的是那个爆音，治不了"字没说完"——真觉得哪句被吞了，
    改台词重跑（--force）比调参数快。
    """
    import array
    samples = array.array("h")
    samples.frombytes(pcm)

    fade = min(int(SAMPLE_RATE * FADE_OUT_MS / 1000), len(samples))
    for i in range(fade):
        k = (fade - i) / fade                       # 1 -> 0
        samples[len(samples) - fade + i] = int(samples[len(samples) - fade + i] * k)

    samples.extend([0] * int(SAMPLE_RATE * TAIL_SILENCE_MS / 1000))
    return samples.tobytes()


def write_wav(path, pcm):
    """按真实长度重写标准 WAV 头再落盘。"""
    byte_rate = SAMPLE_RATE * CHANNELS * BITS // 8
    block_align = CHANNELS * BITS // 8
    header = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, CHANNELS,
                                    SAMPLE_RATE, byte_rate, block_align, BITS)
    header += b"data" + struct.pack("<I", len(pcm))
    with open(path, "wb") as f:
        f.write(header)
        f.write(pcm)


def synth_screened(text, label):
    """合成 + 音色粗筛。漂得离谱就重抽，最多 F0_MAX_TRIES 次，取 F0 最低那条。"""
    takes = []
    for attempt in range(1, F0_MAX_TRIES + 1):
        pcm = synth(text)
        f0, frames = median_f0(pcm)
        takes.append((f0, frames, pcm))
        drifted = (f0 == f0 and f0 > F0_REROLL_ABOVE_HZ and frames >= F0_MIN_VOICED_FRAMES)
        note = f"F0={f0:.0f}Hz/{frames}帧"
        if not drifted:
            print(f"            {note}")
            return pcm
        print(f"            {note} 偏高，疑似音色漂移，第 {attempt} 次重抽…")
    # 三次都高：取最低的那条，并明确标出来让主人重点听
    takes.sort(key=lambda t: (t[0] if t[0] == t[0] else 9e9))
    print(f"            ⚠️ {label} 三次都偏高（最低 {takes[0][0]:.0f}Hz），"
          f"取最低那条，**这句要重点听**")
    return takes[0][2]


def load_manifest(path):
    if not os.path.exists(path):
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except ValueError:
        return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true", help="全部重新合成（会连你已经认可的也重抽）")
    ap.add_argument("--only", metavar="CATEGORY", help="只做这一类")
    ap.add_argument("--redo", metavar="LIST", default="",
                    help="强制重抽指定几句，逗号分隔，如 pat/1,pet/3（台词没改但想换一条采样时用）")
    args = ap.parse_args()

    redo = {s.strip() for s in args.redo.split(",") if s.strip()}
    manifest_path = os.path.join(OUT_ROOT, "manifest.json")
    manifest = load_manifest(manifest_path)

    groups = parse_phrases(PHRASE_FILE)

    unknown = set(groups) - set(CATEGORIES)
    if unknown:
        print(f"warn: 台词文件里有设备不认识的分类 {sorted(unknown)}"
              f"（设备只读 {CATEGORIES}）", file=sys.stderr)

    total = 0
    for category in CATEGORIES:
        if args.only and category != args.only:
            continue
        phrases = groups.get(category, [])
        if not phrases:
            print(f"[{category}] 没有台词，跳过")
            continue

        out_dir = os.path.join(OUT_ROOT, category)
        os.makedirs(out_dir, exist_ok=True)

        # 重新生成前先清掉旧的：句子数量变了会留下孤儿 WAV，
        # 设备是"目录里随机抽"，孤儿会一直被抽到。
        if args.force:
            for old in os.listdir(out_dir):
                if old.lower().endswith(".wav"):
                    os.remove(os.path.join(out_dir, old))

        for i, text in enumerate(phrases, 1):
            key = f"{category}/{i}"
            out_path = os.path.join(out_dir, f"{i:02d}.wav")

            # 只重做"台词变了 / 文件不在 / 你点名 --redo"的。
            # 这一条是有意的:主人已经用耳朵认可过的句子,不能因为改了别的行就被连坐重抽。
            unchanged = (manifest.get(key) == text and os.path.exists(out_path))
            if unchanged and not args.force and key not in redo:
                print(f"[{category}] {i:02d} 未变，保留：{text}")
                continue

            print(f"[{category}] {i:02d} 合成中：{text}", flush=True)
            pcm = soften_tail(synth_screened(text, key))
            write_wav(out_path, pcm)
            manifest[key] = text
            secs = len(pcm) / (SAMPLE_RATE * 2)
            print(f"            -> {out_path}  {len(pcm)/1024:.1f}KB / {secs:.2f}s")
            total += 1

    os.makedirs(OUT_ROOT, exist_ok=True)
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1, sort_keys=True)

    print(f"\n完成，新合成 {total} 句。")
    print(f"素材在 {OUT_ROOT}")
    print("拷到 SD 卡的 /app/AiStackChanEx/voice/ 下即可（目录结构一样）。")


if __name__ == "__main__":
    main()
