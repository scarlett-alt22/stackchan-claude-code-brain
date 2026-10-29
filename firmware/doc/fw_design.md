# FW Design Information
Notes on FW design, etc.

- [Task](#task)
- [Battery Status Overlay](#battery-status-overlay)
- [ESP-NOW Remote Control Mod](#esp-now-remote-control-mod)
- [Realtime API Function Calling](#realtime-api-function-calling)
  - [Avatar Expression](#avatar-expression)
- [Web App](#web-app)
  - [Personalize page](#personalize-page)
  - [Dizzy GIF upload](#dizzy-gif-upload)
- [Head Touch Sensor](#head-touch-sensor)
- [Local Wake Word](#local-wake-word)
- [Mac Tool Gateway](#mac-tool-gateway)


## Task

| Task name | function | Stack size [bytes] | Priority |
| --- | --- | --- | --- |
| loopTask | Arduino loop task | 8192 | 1 |
| drawLoop | Avatar control | 4 * 1024 | 2 |
| facialLoop | Avatar control | 1024 | 2 |
| lipSync | Lip Sync for avatar| 2048 | 2 |
| servo | Servo control synchronized with the avatar | 2048 | 1 |
| battery_check | Battery level check | 2048 | 1 |
| asyncTtsStreamTask | TTS streaming play | 5 * 1024 | 2 |
| webSocketLoopTask | WebSocket processing for LLM Realtime API | 6 * 1024 | 3 |

## Battery Status Overlay

`battery_check` polls `M5.Power` once per minute, clamps the reported level to
`0..100`, and keeps the battery icon enabled for every level. Charging adds the
lightning glyph; discharging keeps the same level icon without the glyph.

- `m5stack-avatar/src/Face.cpp` composites the overlay at screen position
  `(280, 5)` (top-right) into the completed avatar frame before its single
  display push, so face breathing, scale, and rotation do not move it and it
  does not flicker.
- `GifPlayer.cpp` redraws the clipped battery row immediately whenever the GIF
  decoder overwrites a row in the overlay area, then refreshes the full icon at
  frame completion. It uses the state cached by `battery_check` instead of
  polling the power device from the GIF task.
- GIF playback pauses avatar drawing, so the two paths do not write the display
  concurrently.
- A `0` reading is an empty outline and `100` is a full icon; neither is hidden.

## ESP-NOW Remote Control Mod

初期実装では `src/mod/EspNowRemote` に Receiver 固定の Mod を追加する。

- Wi-Fi channel は `1` 固定。
- ESP-NOW 受信コールバックでは payload を固定バッファへコピーするだけにし、Serial 出力は Mod の `idle()` で行う。
- 標準 firmware の Sender から受けた場合は Arduino `esp_now` の受信 data でアプリ payload が offset `20` から始まるため、offset `20` を優先して標準 8 byte 形式として decode 表示する。8 byte だけ届く場合は offset `0` から decode する。
- ESP-NOW Remote Mod 実行中は `main.cpp` の Avatar gaze 連動 servo task を抑止し、受信した yaw/pitch を `ServoCustom::moveTo()` に渡す。
- yaw `-1280..1280` は servo x `45..-45` 度へ、pitch `0..900` は servo y `0..-30` 度へ変換する。
- payload の speed は `ServoCustom::moveTo()` の移動時間 ms として渡す。
- laser にはまだ反映しない。
- ESP-NOW Mod 実行中は Wi-Fi channel 変更により Web/FTP/Realtime API と干渉する可能性がある。
- ESP-NOW Mod 離脱時は ESP-NOW を停止し、offline mode でなければ Wi-Fi STA の再接続を最大 5 秒待つ。

## Realtime API Function Calling

### Avatar Expression

Realtime API ビルドでは、Function Calling により AI が会話中の感情に合わせて Avatar の表情を変更できる。

- 対象ビルド
  - `REALTIME_API` が定義される PlatformIO 環境。
- 関数名
  - `set_avatar_expression`
- 引数
  - `expression`: `neutral`, `happy`, `angry`, `sad`, `doubt`, `sleepy`
- 実装
  - `src/llm/ChatGPT/FunctionCall.cpp`
  - `m5avatar::Expression` に変換して `Avatar::setExpression()` を呼び出す。
  - `LLMBase.cpp` の `systemRole_realtimeAvatarExpression` で Realtime API の system instructions に利用方針を明示する。
- スコープ
  - `json_Functions` は通常 ChatGPT や Gemini Live からも参照されるため、この関数の schema と実行処理は `#if defined(REALTIME_API)` で限定する。
  - `systemRole_realtimeAvatarExpression` は Realtime 系 LLM の `load_role()` で `systemRole_memory` または `systemRole_noMemory` に追加する。

## Web App
WebAPI.cpp のインラインアセンブラ(マクロ：IMPORT_FILE)で incbinフォルダ内のhtmlファイルやjsファイルをプログラム領域に埋め込む。

### Personalize page
- ファイル構成
  - incbin/personalize.html
  - incbin/personalize.js
- 言語
  - ページやダイアログ内の言語は英語版のみ。
- 画面構成
  - Role (Custom Instructions)

#### Role (Custom Instructions)
- 構成
  - フォーム
    - ロール（カスタム指示）の入出力。
  - 設定ボタン
    - API /role_set をPOSTし、フォームの内容を設定する。
- 画面更新時の動作
  - API /role_get をPOSTし、現在設定されているカスタム指示を取得してフォームに表示する。 

#### Memory
- 構成
  - フォーム
    - 取得した記憶内容を表示。
  - Clearボタン
    - API /memory_clear をPOSTし、記憶内容を消去する。
    - 消去を実行する前に、OK/Cancelのダイアログを表示して本当に消去してよいかを確認する。
- 画面更新時の動作
  - API /memory_get をPOSTし、記憶内容を取得してフォームに表示する。
 
### Dizzy GIF upload

`POST /dizzy_gif_upload` accepts one multipart upload and writes it atomically to
`/app/AiStackChanEx/gif/dizzy/stackchan-dizzy.gif` on the SD card.

- The target path is fixed, so a client cannot choose an SD path.
- Uploads are limited to 200 KiB and are written to a temporary file before
  replacement, keeping the prior scene intact on interruption.
- This is separate from the legacy FTP server, which only manages SPIFFS and
  cannot reach the SD-resident GIF theatre assets.

## Head Touch Sensor

`src/driver/HeadTouchSensor.*` provides the shared polling driver for the official CoreS3 head touch sensor.

- The driver owns Si12T initialization, 3ch sampling, and gesture detection.
- `AiStackChanMod` and `RealtimeAiMod` call `HeadTouchSensor::update()` from `idle()`.
- `SwipeForward` and `SwipeBackward` are treated as pet gestures.
- Each Mod keeps its own 3 second Happy expression timeout so the driver does not depend on Avatar.
- On non-CoreS3 builds, or when Si12T is not found, the driver is a no-op and normal Mod behavior continues.

## Local Wake Word

CoreS3 uses the existing SimpleVox VAD/MFCC/DTW path for a local acoustic wake
word. Because StackChan CoreS3 has no A/B/C hardware keys, holding the lower
center of the screen for two seconds starts one enrollment by reusing the
existing B-hold action. After the tones, the user says the chosen phrase and the
captured feature is saved as `/wakewordN.bin` in SPIFFS.
The feature is local to the device and does not use STT, an API, or a text
keyword. Recording the same phrase two or three times improves tolerance for
distance and speaking variation.

At AiStackChan mode construction, `wakeword_init()` loads all valid samples.
If at least one exists, listening is enabled automatically after every reboot;
with no samples it remains disabled so the microphone is not continuously
processed without a usable template. A continues to toggle listening for the
current boot on boards that expose A, while touch-to-talk remains available in
either state. The CoreS3 enrollment area is `x=100..220, y=170..239`; it does
not overlap the normal top touch-to-talk area.

## STT DNS Recovery

`src/stt/Whisper.cpp` resolves the configured transcription host before opening
TLS. After a successful lookup, it uses the standard hostname-based
`WiFiClientSecure` connection so the ESP32 Arduino 2.0.9 stack follows its
known-good SNI and certificate-verification path. The immediately repeated
lookup is served by the lwIP DNS cache. CoreS3 device testing showed that this
framework's resolved-IP-plus-hostname overload is reset by the SiliconFlow
endpoint during the TLS handshake, so that overload is intentionally avoided.

If the ESP32 DNS resolver fails while Wi-Fi still appears connected, Whisper
restarts only the Wi-Fi subsystem, waits up to eight seconds for association,
and performs one bounded lookup retry. TLS or server failures do not trigger
the Wi-Fi recovery path, and a second DNS failure retains the existing visible
Error behavior instead of looping or rebooting the device.

Whisper selects its transport from the configured endpoint scheme. HTTPS uses
`WiFiClientSecure` with the configured CA; HTTP uses `WiFiClient` and is
intended only for the trusted LAN Mac gateway. The default deployed STT route
is `http://YOUR_MAC_LAN_IP:8124/v1/audio/transcriptions` because the current Mac
proxy returns Fake-IP addresses that only its own `utun` interface can map.
The gateway verifies the STT token hash, forwards the multipart body unchanged
to SiliconFlow over certificate-verified HTTPS, and never logs audio, tokens,
or transcript content. Direct SiliconFlow HTTPS remains a configuration-only
rollback path.

## Mac Tool Gateway

Type 4 (`LLM_TYPE_CUSTOM_OPENAI`) intentionally omits the firmware's legacy
Function Calling schemas because arbitrary OpenAI-compatible endpoints may not
support them. Tool-backed requests therefore pass through the lightweight Mac
gateway in `gateway/stackchan_gateway.py` before reaching the existing VPS
shim and API-hosted Claude model.

- The gateway is an OpenAI-compatible proxy on LAN port `8124`; it is not a
  model and does not use the Claude session running on the Mac.
- Ordinary chat requests are forwarded without weather data. Explicit weather
  questions and immediate weather follow-ups fetch Open-Meteo data, defaulting
  to Tokyo in the `Asia/Tokyo` timezone.
- Only a compact three-day, four-period summary and likely rain intervals are
  injected into the existing system prompt. Forecast data is cached in memory
  for ten minutes and is not written to long-term memory.
- The incoming Bearer token is checked against a SHA-256 verifier created from
  the ignored SD secret config during installation, then forwarded unchanged
  to the existing VPS shim. Source, runtime files, plist, and logs do not
  contain the plaintext token or complete user messages.
- If weather lookup fails, the gateway adds a failure marker instructing the
  model to report that live weather is unavailable instead of guessing.
- The Mac TTS service remains on port `8123`. Restoring the commented VPS
  `customEndpoint` in the SD configuration bypasses the tool gateway without
  changing the model, device token, or VPS service.
