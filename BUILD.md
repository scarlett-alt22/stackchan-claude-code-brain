# BUILD.md — instructions for the AI agent doing the build

You are helping the owner of a StackChan (M5Stack CoreS3) turn it into the system described in [README.md](README.md): the robot talks, and the answer comes from a fork of the owner's own Claude Code session. **Stage A** runs on the home network: the robot talks to the Mac over the LAN, no VPS needed. **Stage B** (optional) adds a VPS relay so the robot also works away from home on a phone hotspot.

Work in the order below. **Do not start a step until the previous step's check passes.** When a check fails, find the broken hop (see "Bisecting" at the end) before changing code.

Background on why things are the way they are: [docs/JOURNEY.md](docs/JOURNEY.md) (human-oriented) and [docs/FIELD_NOTES.md](docs/FIELD_NOTES.md) (symptom / cause / fix / verification). Hop-by-hop details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Target

```
Stage A (home):    StackChan ──HTTP over LAN──▶ Mac :8124 gateway
Stage B (outside): StackChan ──HTTPS──▶ VPS :8446 (socat TLS) ──▶ 127.0.0.1:18124 ══ssh -R══▶ Mac :8124 gateway

Mac :8124 gateway
  ├ STT    → SenseVoice 127.0.0.1:8127 (or mlx-whisper :8126)
  ├ brain  → claude -p --resume <session> --fork-session --tools ""
  ├ TTS    → tts_minimax_server 127.0.0.1:8125
  └ photos → stored on the Mac
```

## Ask the owner first (you cannot do these for them)

- [ ] A StackChan on CoreS3 that already speaks with upstream [AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex). If it has never spoken, stop and do the upstream setup first.
- [ ] A Mac that can stay on and awake, ideally with a fixed LAN address (a DHCP reservation on the router). Apple Silicon is needed only for the Whisper fallback.
- [ ] Claude Code installed and logged in, with an interactive session they use (`claude` at `/opt/homebrew/bin/claude`, or set `STACKCHAN_CLAUDE_BIN`).
- [ ] Stage B only: a VPS with a public IP and ssh access from the Mac.
- [ ] A MiniMax account, API key, and a cloned voice ID. Overseas host is the default; mainland accounts set `MINIMAX_HOST`.
- [ ] Home Wi-Fi and phone-hotspot SSID/password. **The owner types these into `SC_SecConfig.yaml` themselves (see A0).** Do not ask them to paste passwords, tokens or keys into the chat, and never print secrets in command output.
- [ ] An SD card for first boot (it can be removed afterwards).

## Invariants — do not change these

| Rule | Why |
|---|---|
| The brain fork runs with `--tools "" --strict-mcp-config --settings '{"disableAllHooks": true}'` | The mic is open to the room. Anyone nearby can talk to it. With tools, a passer-by could make the Mac run commands. |
| Stage B: no inbound port at home; the device only talks to `https://YOUR_VPS_IP:8446` | The Mac dials out the ssh tunnel. This is what makes the hotspot case work. Never port-forward the gateway to the internet instead. |
| Set both `NO_PROXY` and `no_proxy` to `127.0.0.1,localhost` in the gateway's launchd env | A system proxy on the Mac otherwise captures loopback calls. Python urllib reads lowercase, others read uppercase. |
| TTS responses are close-delimited, never `Transfer-Encoding: chunked` | The device HTTP client does not parse chunked. |
| The gateway appends `\r` to STT responses | The device's Whisper reader waits for CR. Without it, it times out. |
| `tts.audioFormat: "mp3"` when going through the relay | 24 kHz WAV (48 KB/s) stutters over a long RTT; 64 kbps mp3 (8 KB/s) does not. |
| Device token is stored only as SHA-256 (`STACKCHAN_DEVICE_TOKEN_SHA256`) | If `STACKCHAN_STT_TOKEN_SHA256` is not 64 hex chars the gateway refuses to start. That is intended. |
| Do not reformat SPIFFS or switch to LittleFS | The robot's persona is stored in SPIFFS. |
| Do not add "wait until the buffer is full" logic to the audio read path | Measured: it stretched 3.5 s of audio to 13 s of playback. |
| Real IPs, SSIDs, keys, tokens never go into git | See [PUBLISHING_CHECKLIST.md](PUBLISHING_CHECKLIST.md). |

## Stage A — make it speak with the owner's session

### A0. Secrets and the SD-staging folder
- Create `SD-staging/` at the repository root (git-ignored). It mirrors what goes on the SD card:
  - `SD-staging/yaml/SC_SecConfig.yaml` from `config/SC_SecConfig.yaml.example`
  - `SD-staging/app/AiStackChanEx/SC_ExConfig.yaml` from `config/SC_ExConfig.yaml.example`
  - Stage B only: `SD-staging/shim_ca.pem`, made in B1
- The owner generates two random tokens and types them, plus the Wi-Fi details, into `SC_SecConfig.yaml` themselves. `apikey.tts` must equal `apikey.aiservice`: the gateway checks the TTS route against the device token.

### A1. STT on the Mac: SenseVoice on 127.0.0.1:8127 (default) — or Whisper on :8126 (fallback)
**Default: `extras/sensevoice/sv_server.py`.** Same HTTP contract as the Whisper server; also returns `lang` / `emotion` / `event`, which the gateway passes to the brain as a tone hint. A silero VAD in front returns empty text when nobody spoke (Whisper invents whole sentences on silence).
- Copy `extras/sensevoice/` to `~/Library/Application Support/StackChanSenseVoice/`; there: `python3 -m venv .venv && .venv/bin/pip install sherpa-onnx numpy pytest` (pytest is only for the check below).
- Download into `models/` from the `asr-models` release of k2-fsa/sherpa-onnx:
  ```
  mkdir -p models && cd models
  curl -LO https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2
  tar xjf sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2   # ~163 MB; gives model.int8.onnx + tokens.txt in a folder of the same name
  curl -LO https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx
  ```
  If the server dies with `Protobuf parsing failed`, the download was truncated: `model.int8.onnx` should be about 239 MB. Do not use the 2025-09-09 model: it is a Cantonese fine-tune and labels Mandarin as Cantonese.
- Install `config/com.example.stackchan-sensevoice.plist.example` as a LaunchAgent (replace `__HOME__`). It binds 127.0.0.1 only; there is no auth on this hop.
- **Check**: `curl http://127.0.0.1:8127/health` is ok; `.venv/bin/python -m pytest test_sv_server.py` passes (no model needed); posting a silent WAV returns empty text.
- The gateway default (`config/gateway.env.example`) points at :8127.

**Fallback: `extras/stt_whisper_server.py` on :8126** (needs Apple Silicon):
- Copy `extras/stt_whisper_server.py` and `extras/start_stt.sh` into `~/Library/Application Support/StackChanSTT/`.
- In that directory: `python3 -m venv .venv && .venv/bin/pip install mlx-whisper numpy`. Use its own venv; mlx-whisper pins its own `mlx`.
- Install `config/com.example.stackchan-stt.plist.example` as a LaunchAgent (replace `__HOME__`).
- **Don't place it under `~/Documents`**: launchd jobs there failed to exec the venv python because of macOS privacy controls.
- The first start downloads the model and warms it up; `/health` says `loading` until ready.
- **Check**: `curl http://127.0.0.1:8126/health` returns `"status": "ok"`. Then POST a short 16 kHz mono WAV as multipart field `file` to `/v1/audio/transcriptions`; the text comes back in about a second.

### A2. TTS on the Mac: `extras/tts_minimax_server.py` on 127.0.0.1:8125
- Stdlib only; runs on the system `/usr/bin/python3`. Install `config/com.example.stackchan-tts.plist.example` as a LaunchAgent.
- Env: `MINIMAX_API_KEY`, `MINIMAX_VOICE_ID`, `TTS_PORT=8125`, **`MINIMAX_STREAM=1`** (the server defaults to non-streaming, which makes the first sound wait for the whole sentence), `MINIMAX_MODEL=speech-2.8-turbo`. Optional: `MINIMAX_HOST` (mainland accounts), `MINIMAX_MP3_BITRATE` (default 64000).
- Request: `POST /v1/audio/speech` with JSON `{"input": "<text>", "response_format": "mp3"}`. Without `"mp3"` it streams WAV.
- It binds 127.0.0.1 by default (`TTS_BIND`); the device reaches it only through the gateway.
- **Check**: the response plays as audio, and `GET /health` returns OK with `"stream": true` and `"voice_id_set": true` (the ID itself is never printed).

### A3. Gateway on the Mac: `gateway/stackchan_gateway.py` on :8124
- Easiest: `/usr/bin/python3 gateway/install_runtime.py` — copies the gateway to `~/Library/Application Support/StackChanGateway/`, writes the LaunchAgent and fills in the token hashes (see [gateway/README.md](gateway/README.md)). By hand: install `config/com.example.stackchan-gateway.plist.example` as a launchd job, filling in values from `config/gateway.env.example`.
- Stage A needs no CA file: leave `STACKCHAN_UPSTREAM_CA` unset or empty and the gateway starts (the https upstream is only the rarely used api-fallback path, which then uses the system trust store). A CA path that is set but points at a missing file stops startup with `startup failed type=FileNotFoundError`.
- `STACKCHAN_BRAIN=cc` turns on the Claude Code brain.
- `PATH` must include `/opt/homebrew/bin`, otherwise the launchd child can't find `claude`.
- After editing the plist, run `launchctl bootout` and then `bootstrap`. A plain reload does not pick up env changes.
- **Check**: `curl http://127.0.0.1:8124/health` returns `{"status": "ok", "service": "stackchan-gateway"}`.

### A4. Brain
- `gateway/cc_brain.py` picks the most recently active **interactive** Claude Code session: it reads `~/.claude/projects/*/*.jsonl` and keeps only entries with `entrypoint: "cli"` active within the last 72 h.
- So the owner must have used Claude Code interactively recently.
- **Three tokens, three routes**: chat and TTS take the device token (`apikey.aiservice`, which `apikey.tts` must equal); STT takes the separate STT token (`apikey.stt`). Using the device token on `/v1/audio/transcriptions` returns `401`.
- Optional: put a few lines about the owner in `owner_profile.txt` next to the gateway (git-ignored); it is added to the brain's context each turn. Without it nothing breaks.
- **Check**: send one chat completion through the gateway with the device token in `Authorization: Bearer`. Read the token from the owner's file or env; don't echo it. Then confirm:
  - The per-turn log (`robot-log.jsonl`) shows the cc brain answered; the row is not marked `api-fallback`.
  - The reply is one or two short sentences.
- If it fell back, check in this order: the `claude` path, the session age, the 45 s timeout (`STACKCHAN_CC_TIMEOUT`).

### A5. Firmware
- **Build**: `pio run -e m5stack-cores3` compiles only (no device needed); add `-t upload` to flash. A clean build fetches M5GFX 0.2.24 from its git tag, as pinned in `platformio.ini`. **Always pass `-e m5stack-cores3`**: the `default_envs` in `platformio.ini` is a Core2 target.
- **SD layout**:
  - `SC_ExConfig.yaml` (from `config/SC_ExConfig.yaml.example`) goes in `/app/AiStackChanEx/`.
  - `SC_SecConfig.yaml` goes in `/yaml/`.
- **Endpoints (Stage A)**: LLM, TTS and STT all point to `http://<Mac LAN IP>:8124/...` — plain http on the home network, no CA file. The author has run it this way at home since 2026-10-27 on the current firmware. It stops working outside the home network; that is what Stage B is for.
- `tts.audioFormat: "mp3"` is fine on the LAN too, and saves changing it later.
- On a healthy boot the config is mirrored into SPIFFS; after that the card is optional.
- **Device quirks that look like crashes but aren't**:
  - Opening the serial port with pyserial resets the ESP32. Wait 10 s or more.
  - After flashing, serial RX sometimes goes dead while TX keeps logging. Ask the owner to unplug and replug USB once.
  - The network needs about 90 s to settle after a reboot. The device does not answer ping, so use serial or HTTP.
- **Check**: the owner talks to the robot and it answers in the cloned voice. The gateway log shows STT, then brain, then TTS.

## Stage B — take it outside (optional)

### B1. VPS relay and tunnel

- **Certificate**: follow [docs/CERTS.md](docs/CERTS.md). One self-signed cert with `CA:TRUE` and **both `DNS:<ip>` and `IP:<ip>` in the SAN** serves as the VPS server cert and as the device's `shim_ca.pem`. Without the `DNS:` entry, the device's mbedTLS fails with `-9984` even though curl and openssl succeed.
- **VPS**: `socat OPENSSL-LISTEN:8446,reuseaddr,fork,cert=cert.pem,key=key.pem,verify=0 TCP:127.0.0.1:18124`, kept running by a service manager such as systemd.
- **Mac**: run `extras/stackchan-tunnel.sh` under launchd (`config/com.example.stackchan-tunnel.plist.example`). It does `ssh -N -R 127.0.0.1:18124:127.0.0.1:8124 <vps>` with keepalives and writes a timestamped log line on every (re)connect.
- **Mac power**: set it to never sleep on AC.
- **Check**: from anywhere, `curl --cacert shim_ca.pem https://YOUR_VPS_IP:8446/health` returns the gateway's health JSON.

### B2. Point the device at the relay
- Put `shim_ca.pem` in the SD card root.
- Switch LLM, TTS and STT endpoints to `https://YOUR_VPS_IP:8446/...`, each with `customRootCAFile: "/shim_ca.pem"`. Boot with the card in so the config is mirrored again.
- **Check**: on home Wi-Fi the robot still answers, now with the gateway log showing the request arriving through the tunnel (peer 127.0.0.1).

### B3. Travel checks
Follow [docs/TRAVEL_MODE.md](docs/TRAVEL_MODE.md). In short:
1. Add `wifi2` (the phone hotspot) to `SC_SecConfig.yaml`. The owner fills it in.
2. Confirm `audioFormat: "mp3"` is set.
3. **Check**: with the SD card removed, cold-boot; the serial log shows boot from the SPIFFS mirror.
4. **Check**: with the hotspot off, boot. Expect `Can't connect to WiFi. Start offline mode.` Then turn the hotspot on: within about 20 s the device reboots itself and comes back online.
5. **Check**: the tunnel log has no unexpected gaps between `starting` lines over 24 h.

## Bisecting when it doesn't talk

Test from the inside out and stop at the first hop that fails:
1. On the Mac: `curl http://127.0.0.1:8127/health` (or :8126 for Whisper), then `:8125/health`, then `:8124/health`.
2. On the VPS, through the tunnel: `curl http://127.0.0.1:18124/health`.
3. On the VPS, through TLS: `curl -k https://127.0.0.1:8446/health`.
4. On the device: check serial for which URL it hits and the HTTP status. Check `customRootCAFile` in **each** of the llm, tts and stt sections, because each section reads its own.
5. For cost, read `usage` in `robot-log.jsonl`, not `total_cost_usd`. A resumed fork reports the parent session's running total.
