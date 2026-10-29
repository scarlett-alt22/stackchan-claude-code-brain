#!/usr/bin/env python3
"""Slow the existing touch-reaction WAV files without changing their pitch.

This uses a small WSOLA-style time-domain stretcher implemented with the Python
standard library, so the accepted voice takes are preserved and no cloud/API
resynthesis is needed.

Example:
    python3 tools-voice/stretch_touch_voices.py \
        --tempo 0.92 \
        --source SD-staging/app/AiStackChanEx/voice \
        --output deploy/voice-tempo-preview/voice \
        --preview deploy/voice-tempo-preview/preview-all.wav
"""

import argparse
import array
import hashlib
import json
import math
import os
import sys
import wave

try:
    import numpy as np
except ImportError:
    np = None


CATEGORIES = ("pat", "pet", "pet_back", "shake")


def read_wav(path):
    with wave.open(path, "rb") as wav:
        params = wav.getparams()
        if params.nchannels != 1 or params.sampwidth != 2:
            raise ValueError(f"{path}: expected mono 16-bit PCM")
        if params.comptype != "NONE":
            raise ValueError(f"{path}: expected uncompressed PCM")
        raw = wav.readframes(params.nframes)
    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    return params.framerate, samples


def write_wav(path, sample_rate, samples):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pcm = array.array("h", samples)
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm.tobytes())


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(128 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def stretch_wsola(samples, sample_rate, tempo):
    """Return samples played at ``tempo`` while approximately preserving pitch.

    tempo < 1 slows the speech. Frames are overlap-added, and each new frame is
    aligned to the most similar waveform near its expected input position.
    """
    if not 0.75 <= tempo <= 1.25:
        raise ValueError("tempo must be between 0.75 and 1.25")
    if len(samples) < sample_rate // 10:
        return array.array("h", samples)

    frame = int(round(sample_rate * 0.040))       # 40 ms
    if frame % 2:
        frame += 1
    synth_hop = frame // 2                        # 20 ms, 50% overlap
    overlap = frame - synth_hop
    analysis_hop = synth_hop * tempo
    search = int(round(sample_rate * 0.010))      # ±10 ms alignment window
    corr_stride = 4
    candidate_stride = 2

    target_len = max(frame, int(round(len(samples) / tempo)))
    output = [0.0] * (target_len + frame)
    first = min(frame, len(samples))
    for i in range(first):
        output[i] = samples[i]

    fade_in = [
        0.5 - 0.5 * math.cos(math.pi * i / max(1, overlap - 1))
        for i in range(overlap)
    ]

    out_pos = synth_hop
    nominal = analysis_hop
    produced = first
    max_start = max(0, len(samples) - frame)

    while out_pos + frame <= target_len + frame and nominal < max_start:
        expected = max(0, min(max_start, int(round(nominal))))
        lo = max(0, expected - search)
        hi = min(max_start, expected + search)

        target_energy = 0.0
        for i in range(0, overlap, corr_stride):
            value = output[out_pos + i]
            target_energy += value * value

        best = expected
        if target_energy > 1.0:
            best_score = -2.0
            for candidate in range(lo, hi + 1, candidate_stride):
                dot = 0.0
                candidate_energy = 0.0
                for i in range(0, overlap, corr_stride):
                    a = output[out_pos + i]
                    b = samples[candidate + i]
                    dot += a * b
                    candidate_energy += b * b
                if candidate_energy <= 1.0:
                    continue
                score = dot / math.sqrt(target_energy * candidate_energy)
                if score > best_score:
                    best_score = score
                    best = candidate

        for i in range(overlap):
            alpha = fade_in[i]
            existing = output[out_pos + i]
            incoming = samples[best + i]
            output[out_pos + i] = existing * (1.0 - alpha) + incoming * alpha
        for i in range(overlap, frame):
            output[out_pos + i] = samples[best + i]

        produced = max(produced, out_pos + frame)
        out_pos += synth_hop
        nominal += analysis_hop

    # The source files already contain a short silent tail. If rounding leaves a
    # few unfilled samples, zero-padding extends only that tail rather than speech.
    result = array.array("h")
    for value in output[:target_len]:
        result.append(max(-32768, min(32767, int(round(value)))))
    return result


def stretch_wsola_numpy(samples, tempo):
    """Use the same NumPy WSOLA implementation as the accepted TTS server."""
    x = np.frombuffer(samples.tobytes(), dtype="<i2").astype(np.float32) / 32768.0
    if abs(tempo - 1.0) < 1e-3 or x.size == 0:
        return array.array("h", samples)

    frame, synth_hop, search = 1024, 256, 160
    window = np.hanning(frame).astype(np.float32)
    analysis_hop = synth_hop * tempo
    output_len = int(len(x) / tempo) + frame
    output = np.zeros(output_len, dtype=np.float32)
    weight = np.zeros(output_len, dtype=np.float32)
    if len(x) < frame + search:
        return array.array("h", samples)

    next_frame = x[:frame].copy()
    output[:frame] += next_frame * window
    weight[:frame] += window
    output_pos, analysis_pos = synth_hop, analysis_hop

    while (output_pos + frame < output_len and
           int(analysis_pos) + frame + search < len(x)):
        low = max(0, int(analysis_pos) - search)
        high = min(len(x) - frame, int(analysis_pos) + search)
        if high <= low:
            break
        candidates = np.lib.stride_tricks.sliding_window_view(
            x[low:high + frame], frame
        )
        best = low + int(np.argmax(candidates[:high - low + 1] @ next_frame))
        output[output_pos:output_pos + frame] += x[best:best + frame] * window
        weight[output_pos:output_pos + frame] += window
        next_frame = x[
            best + synth_hop:best + synth_hop + frame
        ]
        if len(next_frame) < frame:
            break
        output_pos += synth_hop
        analysis_pos += analysis_hop

    output = output[:output_pos + frame]
    weight = weight[:output_pos + frame]
    output = np.where(weight > 1e-6, output / np.maximum(weight, 1e-6), output)
    pcm = np.clip(output * 32768.0, -32768, 32767).astype("<i2")
    result = array.array("h")
    result.frombytes(pcm.tobytes())
    if sys.byteorder != "little":
        result.byteswap()
    return result


def peak(samples):
    return max((abs(value) for value in samples), default=0)


def collect_wavs(root):
    paths = []
    for category in CATEGORIES:
        directory = os.path.join(root, category)
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if name.lower().endswith(".wav"):
                paths.append((category, name, os.path.join(directory, name)))
    return paths


def make_preview(output_root, entries, preview_path, gap_ms=300):
    preview = array.array("h")
    sample_rate = None
    for entry in entries:
        path = os.path.join(output_root, entry["category"], entry["name"])
        rate, samples = read_wav(path)
        if sample_rate is None:
            sample_rate = rate
        elif rate != sample_rate:
            raise ValueError("preview inputs have mixed sample rates")
        preview.extend(samples)
        preview.extend([0] * int(rate * gap_ms / 1000))
    write_wav(preview_path, sample_rate, preview)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tempo", type=float, default=0.92)
    parser.add_argument("--source", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--preview")
    args = parser.parse_args()

    entries = []
    engine = "numpy-wsola" if np is not None else "stdlib-wsola"
    for category, name, source_path in collect_wavs(args.source):
        sample_rate, original = read_wav(source_path)
        if np is not None:
            stretched = stretch_wsola_numpy(original, args.tempo)
        else:
            stretched = stretch_wsola(original, sample_rate, args.tempo)
        output_path = os.path.join(args.output, category, name)
        write_wav(output_path, sample_rate, stretched)
        entries.append({
            "category": category,
            "name": name,
            "source_sha256": sha256(source_path),
            "output_sha256": sha256(output_path),
            "sample_rate": sample_rate,
            "source_frames": len(original),
            "output_frames": len(stretched),
            "source_seconds": len(original) / sample_rate,
            "output_seconds": len(stretched) / sample_rate,
            "duration_ratio": len(stretched) / max(1, len(original)),
            "source_peak": peak(original),
            "output_peak": peak(stretched),
        })
        print(
            f"{category}/{name}: "
            f"{len(original) / sample_rate:.3f}s -> "
            f"{len(stretched) / sample_rate:.3f}s"
        )

    if not entries:
        raise SystemExit("no WAV files found")

    os.makedirs(args.output, exist_ok=True)
    report_path = os.path.join(os.path.dirname(args.output), "report.json")
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(
            {
                "tempo": args.tempo,
                "pitch_preserving": True,
                "engine": engine,
                "files": entries,
            },
            f, ensure_ascii=False, indent=2,
        )
        f.write("\n")

    if args.preview:
        make_preview(args.output, entries, args.preview)
        print(f"preview: {args.preview}")
    print(f"report: {report_path}")


if __name__ == "__main__":
    main()
