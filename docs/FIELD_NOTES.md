# FIELD_NOTES（踩坑记录）

每条格式：症状 / 根因 / 修法 / 怎么验证。素材来自代码注释与 BRIEF；标了 `inferred` 的是推断，未经实测。

---

## 1. SD 卡第三次 FAT 报废，整机变砖

- **症状**：SD 卡挂载失败 → `System reset after 5 seconds` → `ESP.restart()` → 还是失败 → 无限重启循环，机器人变砖。
- **根因**：SD 卡被当成“开机的人质”——配置、CA、触摸语音全在 SD 上，没有 fallback。（`firmware/src/main.cpp` 注释）
- **修法**：`SpiffsMirror`——SD 好时照常从 SD 读，顺手把配置+CA+触摸语音镜像进内部 SPIFFS；SD 坏时用上次的镜像从 SPIFFS 起机。SD 沦为可选外设。
- **验证**：拔卡冷启动，串口出现 `[Mirror] SD 挂不上, 从 SPIFFS 镜像起机`，机器人照常工作。

## 2. 长中继下 WAV 断续，mp3 丝滑

- **症状**：24 kHz WAV 经 VPS 中继播放一卡一卡。
- **根因**：ESP32 吞吐 ≈ TCP 窗口（~5.7 KB）/ RTT；24 kHz WAV 要 48 KB/s，窗口/RTT 撑不住。mp3 64 kbps 只要 8 KB/s。
- **修法**：TTS 切 mp3，8 KB prefill（WAV 保留 24 KB prefill）；`SC_ExConfig.yaml` 里 `tts.audioFormat: "mp3"`。
- **验证**：同一中继下 mp3 连续播放无断续。prefill 在 `firmware/src/driver/PlayMP3.cpp` 的 `prefillBuffer()`（mp3 与 WAV 两条路各调用一次），串口打 `[audio] prefill … B in … ms`。

## 3. STT 从云端搬回本机：7.2s → 1.0s，顺手治好乱码

- **症状**：STT 走远程 SiliconFlow 要 7.2 秒；且“说的话被识别成乱码、机器人照着乱码答”。
- **根因**：`inferred`——跨国链路延迟 + 远端模型对短中文指令不稳。
- **修法**：STT 搬回本机 mlx-whisper（`:8126`），网关代码零改动——上游就是个环境变量 `STACKCHAN_STT_UPSTREAM_URL`。
- **验证**：同一句话 1.0 秒出结果，乱码消失。（`com.example.stackchan-gateway.plist` 注释；那是本机直测一句短话。网关日志里的 STT 耗时含整段录音，2026-10-05 实测每次 3.1–3.7 s，README 表格里的 3.7 s 就是这一种。）

## 4. 回环流量被系统代理“拽走”

- **症状**：网关调本机 `:8126`/`:8125` 间歇失败。
- **根因**：机器上 Clash 常年开着系统代理；没有 `NO_PROXY`/`no_proxy`，网关到本机的请求会被拽进代理链路。
- **修法**：launchd plist 里同时设 `NO_PROXY` 与 `no_proxy` 为 `127.0.0.1,localhost`（大小写两份都要：Python 的 urllib 认小写，别的库认大写）。
- **验证**：设置后本机链路稳定。

## 5. 隧道断了，但日志里看不出几点断的

- **症状**：ssh 本身的输出不带时间戳；万一出门在外断线，事后查不出是几点断的。
- **根因**：`stackchan-tunnel.sh` 直接 exec ssh，裸输出无时间。
- **修法**：输出逐行加时间戳；每次（重）连打一行 `starting`；断线时间看相邻两行 `starting` 的间隔（`ServerAliveInterval=15 ×3`，约 45 秒判定 + 10 秒节流）。
- **验证**：2026-10-05 kill 实测。另外已知：ssh 一退出 launchd 就把整组进程清掉，“ssh exited”那行永远写不出来——不要指望它。

## 6. 回复长度上限：18 字 → 一两句/50 字

- **症状**：`inferred`——早期回复太长，TTS 等太久。
- **根因**：Qwen3-TTS 时代 RTF≈4，30 个字要等 22 秒才出声。（`test_stackchan_gateway.py:171-173` 注释）
- **修法**：说话规则里写死“只说一句话、18 个字以内”（prompt 指令，非代码截断）。
- **验证**：`test_stackchan_gateway.py:175` 断言规则文本。
- **后来**（2026-10-05）：TTS 换成 MiniMax 流式后，首包约 1.6 s（README 实测表）且与长度无关，18 字的理由不在了；放宽为"一两句、一般 50 字以内"（外放场合仍不宜长）。设备回复缓冲 2000 B、历史 50 KB，一百多字都放得下。

## 7. prompt cache：隔久了整段重写，连着聊会命中

- **症状**：`inferred`——窗口越大，每轮 brain 越慢、越贵。
- **根因**：每轮都 fork 一份完整会话；prompt cache 只保留几分钟，过期后 fork 要把整个会话重新写进 cache。早先记录过 fork 的 `cache_read` 只有 ~3.9k，据此以为 fork 根本不继承 cache；10-05 连着聊的实测推翻了这一点（见下方更正）。
- **修法**：无——这是架构代价。看成本用 robot-log 的 `usage` 字段，别用 `total_cost_usd`（resumed fork 报的是父会话累计值，会虚高）。
- **更正**（2026-10-05 实测）：上面"不继承 cache"只在隔得久、缓存过期时成立。连续 7 轮（11:10–11:14）第一轮 cache_write≈203k，之后每轮 cache_read≈195k、cache_write≈8k。

## 8. 断线重连永远连不回去

- **症状**：`inferred`——Wi-Fi 掉线后，loop 里重连一直失败，只能重启。
- **根因**：AP 只在“前回设定失败”时登记；正常启动走“前回设定”直连，`wifiMulti` 的 AP 列表是空的，loop 重连无 AP 可连。（`main.cpp:189-193` 注释）
- **修法**：`wifi_register_aps()` 改为开机即登记；loop 断线重连直接用。
- **验证**：2026-10-05 真机验证（热点关着开机→离线→开热点→自己重启连上）。`firmware/test/wifi_dual/wifi_dual_test.cpp` 只测 AP 列表怎么拼（`g++ -std=c++17` 直接编译运行），不覆盖「开机即登记」这一步。

## 9. 设备不解析 chunked 编码

- **症状**：`inferred`——标准 HTTP chunked 流式响应设备端播不出声。
- **根因**：设备 HTTP 客户端不处理 `Transfer-Encoding: chunked`。（`tts_minimax_server.py` 手写 HTTP 的注释）
- **修法**：TTS 服务手写 HTTP，全部 close-delimited；流式 WAV 头 RIFF/data 长度填 `0xFFFFFFFF`。
- **验证**：设备正常播完流式 WAV/mp3。

## 10. Whisper legacy reader 读不到流结尾

- **症状**：`inferred`——ESP32 侧 STT 响应收完后 Stream 超时。
- **根因**：设备用的 Whisper legacy reader 以 CR 为终止符；网关透传的上游响应末尾没有它。（`stackchan_gateway.py` STT 路由注释）
- **修法**：网关在 STT 响应末尾追加 `b"\r"`。
- **验证**：`gateway/tests/test_stackchan_gateway.py` 断言 STT 响应体以 `\r` 结尾。

## 11. TTS 代理不转发设备 Authorization 头

- **症状**：无（设计决策）。
- **根因**：设备的 bearer token 只用于网关鉴权，不应泄露给 TTS 上游。
- **修法**：代码注释原文：`# Deliberately NOT forwarding the device's Authorization header.`
- **验证**：`test_tts_proxy.py` 覆盖“密钥不转发”。
