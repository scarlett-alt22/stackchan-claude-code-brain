# ARCHITECTURE

每一跳、端口、鉴权、跑在哪里。数字与行为均来自快照代码（`gateway/stackchan_gateway.py`、`gateway/cc_brain.py`、`extras/tts_minimax_server.py`、`extras/stackchan-tunnel.sh`、固件 `main.cpp`/`StackchanExConfig.*`），行号指快照内文件。

## 总览

```
StackChan (M5Stack CoreS3)
  │  HTTPS outbound only, port 8446
  │  设备 pin 自签 CA (shim_ca.pem)；无 CA 配置时拒绝 HTTPS
  ▼
VPS :8446
  │  socat: TLS terminate → 127.0.0.1:18124
  ▼  (VPS 本地回环)
ssh reverse tunnel:  -R 127.0.0.1:18124:127.0.0.1:8124
  │  由 Mac 上 launchd 常驻 (extras/stackchan-tunnel.sh)
  ▼
Mac gateway :8124  (gateway/stackchan_gateway.py)
  ├─ POST /v1/audio/transcriptions  →  STT: 本地 SenseVoice :8127（备选 mlx-whisper :8126）
  ├─ POST /v1/chat/completions      →  Brain: cc_brain.py fork Claude Code 会话
  │                                    （失败 → 回落 VPS 透传）
  ├─ POST /v1/audio/speech          →  TTS 代理 → 本地 :8125 (tts_minimax_server.py)
  ├─ POST /v1/photos                →  存 Mac 本地 JPEG
  └─ GET  /health                   →  {"status": "ok", "service": "stackchan-gateway"}
```

**家里不开任何公网端口。设备只做 outbound HTTPS。**

## 端口表

| 端口 | 在哪里 | 谁在听 | 说明 |
|---|---|---|---|
| 8446 | VPS | socat | 公网 HTTPS 入口；TLS 终止后转 127.0.0.1:18124 |
| 18124 | VPS | ssh -R 远端 | 只绑 127.0.0.1；经反向隧道通到 Mac :8124 |
| 8124 | Mac | stackchan_gateway.py | 网关主端口（`STACKCHAN_GATEWAY_HOST` 默认 `0.0.0.0`，`STACKCHAN_GATEWAY_PORT` 默认 `8124`） |
| 8125 | Mac | tts_minimax_server.py | MiniMax 克隆嗓音 TTS（`TTS_PORT`，默认 8125） |
| 8126 | Mac | mlx-whisper | 本地 STT（网关经 `STACKCHAN_STT_UPSTREAM_URL` 调用） |

## 设备 → VPS → Mac：TLS 与鉴权

- 设备侧 HTTPS 固定 pin 自签 CA（`shim_ca.pem`）；固件在没有配置 CA 时**拒绝**发起 HTTPS（`main.cpp` 注释与 `StackchanExConfig` 的 customRootCA 加载逻辑）。
- 网关校验设备 `Authorization: Bearer <token>`：
  - `STACKCHAN_DEVICE_TOKEN_SHA256` 存在时，对提交的 token 取 SHA-256，用 `hmac.compare_digest` 比对（`stackchan_gateway.py:747-763` `authorized()`）；
  - 否则与 secret yaml 里 `apikey` 的明文 token 比对；
  - STT 路由（`authorized_stt()`）**强制** hash 比对；`STACKCHAN_STT_TOKEN_SHA256` 不是 64 位十六进制时，网关**直接拒绝启动**（`from_environment` 抛 ValueError）。
- 网关日志/源码/plist 里**只存 SHA-256 单向值**，不存明文 token；STT 音频与转写正文不记日志。
- TTS 代理**故意不**把设备的 Authorization 头转发给上游 TTS（代码注释原文：`# Deliberately NOT forwarding the device's Authorization header.`）。

## 网关路由细节（`gateway/stackchan_gateway.py`）

| 方法 | 路径 | 行为 |
|---|---|---|
| GET | `/health` | `{"status": "ok", "service": "stackchan-gateway"}` |
| POST | `/v1/audio/transcriptions` | STT 专用鉴权后透传上游；响应末尾追加 `b"\r"`（注释：Whisper legacy reader 读到 CR 终止，防止 ESP32 Stream 超时） |
| POST | `/v1/audio/speech` | TTS 代理：默认上游 `http://127.0.0.1:8125/v1/audio/speech`；`TTS_CONNECT_TIMEOUT=5`，`TTS_READ_TIMEOUT=65`，`TTS_CHUNK_BYTES=4096`；close-delimited 流式 |
| POST | `/v1/photos` | 收 raw `image/jpeg`（上限 1 MB），存 `~/StackChan/photos/YYYY-MM-DD/HHMMSS.jpg`（同秒加 `-2` 后缀），记一行 `{"type": "photo"}` 到 `robot-log.jsonl` |
| POST | `/v1/chat/completions` | `STACKCHAN_BRAIN=cc` 时走 cc brain；任何失败自动回落到 VPS 透传（`DEFAULT_UPSTREAM_URL=https://YOUR_VPS_IP:8443/v1/chat/completions`） |

通用限制：请求体上限 256 KB（`MAX_REQUEST_BYTES`），照片 1 MB（`MAX_PHOTO_BYTES`）。

## Brain：fork 你的 Claude Code 会话（`gateway/cc_brain.py`）

1. **找会话**（`find_window_session()`）：扫描 `~/.claude/projects/` 下 `*.jsonl`，只取 `entrypoint: "cli"` 的交互窗口会话（排除 `sdk-cli` headless），要求 72 小时内活跃。
2. **fork 回答**（`build_argv()`），等价于：
   ```
   claude -p --resume <session> --fork-session --no-session-persistence \
     --model opus --output-format json --tools "" --strict-mcp-config \
     --settings '{"disableAllHooks": true}' \
     --append-system-prompt "<robot_rules>"
   ```
   提问正文走 **stdin**（注释原文：`"List form, never through a shell. The question body goes on stdin because --tools is variadic and would swallow a trailing positional."`）。
3. **说话规则**：每次 fork 前，`robot_rules()` 把一段固定的中文说话规则文本（含身份设定、语言/长度/口语化要求、设备约定、隐私与工具调用禁令）经 `--append-system-prompt` 注入该次 fork；fork 结束即丢弃，**不写回原窗口会话**。规则要求：中文回答、一两句话、**一般 50 个字以内**（上限是 prompt 指令，不是代码硬截断；2026-10-05 前是 18 字）。
4. **并发与失败**：`threading.Lock()` 保证同时只有一个 fork；失败分三类（`CCNoSession`/`CCTimeout`/`CCBadOutput`），网关层捕获后降级回 VPS。返回的 completion 里 `"model": "stackchan-cc-brain"`。
5. 可调：`STACKCHAN_CLAUDE_BIN`（默认 `/opt/homebrew/bin/claude`）、`STACKCHAN_CC_MODEL`（默认 `opus`）、`STACKCHAN_CC_TIMEOUT`（默认 45 秒）。不传自定义 env（继承网关进程环境；launchd plist 里的 `PATH` 负责让子进程找到 brew 的 claude）。
6. 每轮对话追加到 `~/StackChan/robot-log.jsonl`。注意：隔久了（cache 过期）第一轮要把整个窗口写进 cache，连着聊则主要读 cache（见 FIELD_NOTES §7）；看成本请用 robot-log 的 `usage` 字段，不要用 `total_cost_usd`（resumed fork 报的是父会话累计值）。

### 为什么不给它手

fork 出来的分支带着原会话的上下文，但 `--tools ""` + `--strict-mcp-config` + `disableAllHooks` 把工具、MCP、hooks 全关了，`robot_rules()` 里也写明不调用工具。

原因：机器人的麦克风是外放的，谁站在旁边说话它都会听，设备 token 只能证明「这是那台机器人」，证明不了「说话的是主人」。要是这个分支能调工具，路过的人一句话就能让 Mac 执行命令、读文件。所以机器人这条路只给嘴；要动手做事，回到 Claude Code 窗口里。

同理，机器人拍的照片只存到 Mac，这一轮的回答看不到它。

> persona prompt 本人是私人的：公开版只描述机制（上一段），不抄录原文。

## TTS：MiniMax 克隆嗓音（`extras/tts_minimax_server.py`，:8125）

- 纯 stdlib：`POST https://api.minimax.io/v1/t2a_v2`（`HOST`/`MODEL=speech-2.8-hd` 可配）；`MINIMAX_API_KEY` 与 `MINIMAX_VOICE_ID` 只从环境变量读。
- SSE 流式解析：`status=1` 增量包 hex 解码经队列转发；`status=2` 尾包若已收到增量则丢弃。
- 手写 HTTP（`socketserver.ThreadingTCPServer`），**全部 close-delimited**——因为设备不解析 chunked 编码。
- WAV 路径：先缓冲 `HEADSTART_MS`（默认 300ms）再吐，stall 时 100ms 静音片填充；流式 WAV 头 RIFF/data 长度填 `0xFFFFFFFF`。
- MP3 路径（设备 `audioFormat=mp3` 时）：无 WAV 头、无 headstart、无静音填充，hex 解码后直接 `sendall`，`Content-Type: audio/mpeg`；`MINIMAX_MP3_BITRATE` 默认 64000，白名单 `(32000, 64000, 128000, 256000)`。
- `/health` 和启动日志只报 voice ID 有没有设置（`voice_id_set`），不打印值。服务默认只绑 127.0.0.1（`TTS_BIND` 可改），由网关代为鉴权转发。

## 固件侧（`firmware/`，fork of ronron-gh/AI_StackChan_Ex）

- **双 Wi-Fi**：`wifi` 必选、`wifi2` 可选（secret yaml 里 `wifi2: {ssid, password}`），`wifi_dual.h` 的 `wifi_build_ap_list()` 去重排序后交给 `WiFiMulti`，连信号最好的一个；都没配 → SmartConfig。开机 40 秒预算（`WIFI_MULTI_BOOT_BUDGET_MS = 40000`，`main.cpp:231`）。
- **离线自愈**：开机没连上 → 每 20 秒重试，连上立刻 `ESP.restart()`（热切换状态机改不干净，重启最稳）；在线掉线 → 每 10 秒 `wifiMulti.run(5000)` 重连。
- **SD → SPIFFS 镜像**：开机从 SD 读 `SC_ExConfig.yaml`/CA/触摸语音，顺手镜像进 SPIFFS（`SpiffsMirror`）；SD 挂载失败 → 用上次的镜像从 SPIFFS 起机，SD 沦为可选外设。
- **TTS 用 mp3**：8 KB prefill（WAV 24 KB）。原因（实测结论）：ESP32 吞吐 ≈ TCP 窗口（~5.7 KB）/ RTT，24 kHz WAV（48 KB/s）经长中继会断续；mp3 64 kbps 只有 8 KB/s。`audioFormat` 在 `tts:` 段配置（`"mp3"` 或 `"wav"`）。
- **设备端 HTTP API**（`WebAPI.cpp`，+552 行 vs 上游）：`/motion`（摇晃反应开关，默认关，NVS 持久化）、`/volume`、`/servo_origin`，以及触摸/摇头反应用的预录语音片段（`tools-voice/` 离线合成）。
- 拍照经 HTTPS 上传（无 CA 配置时拒绝 HTTPS）。

## 天气工具

网关在用户问天气时才做 geocoding（Open-Meteo）。默认城市读 `location.json`（路径由 `STACKCHAN_LOCATION_FILE` 指定，默认 `~/StackChan/location.json`，内容如 `{"query": "巴黎"}`），每次请求都重读，出门换城市改这个文件即可，不用重启。文件不存在时用 `stackchan_gateway.py` 里的 `DEFAULT_LOCATION`；文件存在但读不了、不是合法 JSON 或没有 `query`，也用它，并打一条警告。
