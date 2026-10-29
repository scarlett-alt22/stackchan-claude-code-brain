# gateway/

Mac-side gateway for the robot, port 8124. Stdlib Python 3 only.

| File | What it is |
|---|---|
| `stackchan_gateway.py` | The HTTP gateway: device auth, STT / chat / TTS / photo routes, weather context, fallback to the VPS path. |
| `cc_brain.py` | Answers a chat request by forking your most recent interactive Claude Code session (`claude -p --resume … --fork-session --tools ""`). |
| `robot_log_hook.py` | Optional Claude Code `UserPromptSubmit` hook: shows new robot-log rows (what was said to the robot, photos taken) in your Claude Code window the next time you type there. |
| `install_runtime.py` | Copies the gateway into `~/Library/Application Support/StackChanGateway/` and writes the LaunchAgent from `com.example.stackchan-gateway.plist`, filling in token hashes. |
| `smoke_test_stt.py` | Sends one recording through the STT route. |
| `tests/` | Unit tests (`python3 -m unittest discover -s tests`). |

## Routes

| Method | Path | Behaviour |
|---|---|---|
| GET | `/health` | `{"status": "ok", "service": "stackchan-gateway"}` |
| POST | `/v1/audio/transcriptions` | STT-token auth, forwarded to `STACKCHAN_STT_UPSTREAM_URL` (local SenseVoice :8127 by default; mlx-whisper :8126 as fallback). A `\r` is appended to the response because the device's reader waits for CR. |
| POST | `/v1/chat/completions` | Device-token auth. With `STACKCHAN_BRAIN=cc` the answer comes from `cc_brain.py`; any failure falls back to `STACKCHAN_UPSTREAM_URL` (the VPS path). |
| POST | `/v1/audio/speech` | Device-token auth, proxied to the TTS server on 127.0.0.1:8125. Close-delimited; the device's Authorization header is not forwarded. |
| POST | `/v1/photos` | Device-token auth, raw `image/jpeg` up to 1 MB, saved under `~/StackChan/photos/YYYY-MM-DD/HHMMSS.jpg`. |

Each exchange is appended to `~/StackChan/robot-log.jsonl`. For cost, read the `usage` field, not `total_cost_usd`: a resumed fork reports the parent session's running total.

## Install

The installer expects the secrets in `SD-staging/` at the repository root. That directory is git-ignored and is the same set of files you copy to the SD card:
- `SD-staging/yaml/SC_SecConfig.yaml`
- `SD-staging/shim_ca.pem`

```sh
/usr/bin/python3 gateway/install_runtime.py
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.example.stackchan-gateway.plist
```

After changing the plist, run `launchctl bootout` then `bootstrap` again; a reload does not pick up environment changes. Keep the runtime copy outside `~/Documents`: macOS privacy controls can stop LaunchAgents from reading it there.

## Weather

When a question looks like a weather question, the gateway geocodes the city with Open-Meteo and injects the forecast as context. The default city is a placeholder (`DEFAULT_LOCATION` in `stackchan_gateway.py`); change it to yours. If Open-Meteo is unreachable, the model is told so and must not invent a forecast.

## Optional owner profile

If `owner_profile.txt` exists next to the gateway, its text is injected as a fixed system note into every chat request. **The robot's microphone is open to the room, so anything you put there can be heard by anyone nearby.** Delete the file to turn this off.
