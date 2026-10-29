#!/usr/bin/env python3
"""Tests for tools/wav_to_header.py (stdlib only).

The good case synthesizes a 0.5 s 440 Hz sine beep at 16 kHz mono 16-bit,
runs the script on it, and checks the emitted header: the embedded array
must be the complete WAV file (RIFF header + PCM), byte for byte, and the
header text must match the shipped placeholder header
(src/driver/OfflineVoice.h). The bad cases must all exit non-zero with a
message and write no header.

Run: python3 -m unittest tools.test_wav_to_header   (from the task dir)
"""

import math
import re
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCRIPT = HERE / "wav_to_header.py"
PLACEHOLDER = ROOT / "src" / "driver" / "OfflineVoice.h"


def write_wav(path: Path, rate=16000, channels=1, width=2, seconds=0.5, freq=440.0):
    frames = int(rate * seconds)
    raw = bytearray()
    for index in range(frames):
        sample = int(0.5 * 32767 * math.sin(2.0 * math.pi * freq * index / rate))
        for _ in range(channels):
            if width == 2:
                raw += sample.to_bytes(2, "little", signed=True)
            else:
                raw.append((sample >> 8) + 128)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(width)
        wav.setframerate(rate)
        wav.writeframes(bytes(raw))
    return bytes(raw)


class WavToHeaderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def run_script(self, in_path, out_path):
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(in_path), str(out_path)],
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_good_beep_header_and_placeholder_match(self):
        wav_path = self.dir / "beep.wav"
        write_wav(wav_path)
        wav_bytes = wav_path.read_bytes()
        out_path = self.dir / "OfflineVoice.h"
        proc = self.run_script(wav_path, out_path)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        header = out_path.read_text(encoding="utf-8")
        self.assertIn("const size_t OFFLINE_VOICE_WAV_LEN = {};".format(len(wav_bytes)), header)
        # The embedded array is the whole WAV file, byte for byte.
        array_body = header.split("OFFLINE_VOICE_WAV[] PROGMEM = {", 1)[1].split("};", 1)[0]
        embedded = bytes(int(tok, 16) for tok in re.findall(r"0x([0-9a-f]{2})", array_body))
        self.assertEqual(embedded, wav_bytes)
        self.assertIn("const uint32_t OFFLINE_VOICE_SAMPLE_RATE = 16000;", header)
        self.assertIn("const uint16_t OFFLINE_VOICE_CHANNELS = 1;", header)
        self.assertIn("const uint16_t OFFLINE_VOICE_BITS_PER_SAMPLE = 16;", header)

    def test_shipped_header_matches_checked_in_recording(self):
        # The public repo ships a synthetic two-tone beep
        # (tools/offline-voice-placeholder.wav) instead of a recorded voice line;
        # replace it with your own recording. Either way the shipped header
        # must be exactly what the script generates from the checked-in WAV.
        out_path = self.dir / "OfflineVoice.h"
        proc = self.run_script(HERE / "offline-voice-placeholder.wav", out_path)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(PLACEHOLDER.exists(), "shipped header missing")
        self.assertEqual(PLACEHOLDER.read_text(encoding="utf-8"), out_path.read_text(encoding="utf-8"))

    def test_bad_sample_rate_rejected(self):
        wav_path = self.dir / "bad-rate.wav"
        write_wav(wav_path, rate=8000)
        out_path = self.dir / "out.h"
        proc = self.run_script(wav_path, out_path)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("8000", proc.stderr)
        self.assertFalse(out_path.exists())

    def test_bad_channels_rejected(self):
        wav_path = self.dir / "bad-channels.wav"
        write_wav(wav_path, channels=2)
        out_path = self.dir / "out.h"
        proc = self.run_script(wav_path, out_path)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("channels 2", proc.stderr)
        self.assertFalse(out_path.exists())

    def test_bad_sample_width_rejected(self):
        wav_path = self.dir / "bad-width.wav"
        write_wav(wav_path, width=1)
        out_path = self.dir / "out.h"
        proc = self.run_script(wav_path, out_path)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("sample width 1", proc.stderr)
        self.assertFalse(out_path.exists())

    def test_non_wav_rejected(self):
        junk = self.dir / "junk.wav"
        junk.write_bytes(b"this is not a wav file")
        out_path = self.dir / "out.h"
        proc = self.run_script(junk, out_path)
        self.assertNotEqual(proc.returncode, 0)
        self.assertFalse(out_path.exists())


if __name__ == "__main__":
    unittest.main()
