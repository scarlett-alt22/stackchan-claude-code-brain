# StackChan × Claude Code Brain

> 对桌上的机器人说话，回答你的是你每天一起工作的那个 Claude Code 会话，用克隆的嗓音说出来。在家局域网直连就能用；想带出门，再加一层中转，连手机热点照样能聊。
>
> Talk to the desk robot. The answer comes from the same long-running Claude Code session you work with every day, in a cloned voice. Works on your home network out of the box; add a relay and it goes out with you on a phone hotspot.

[English version below](#english)

---

## 这是什么

一台 **StackChan 桌面机器人（M5Stack CoreS3）**，它的脑子是 **你 Mac 上长期开着的那个 Claude Code 会话**。

- 你对机器人说话 → Mac 本地 SenseVoice 转写（顺便听出语气，一起告诉大脑） → 从 **你最近在用的 Claude Code 窗口** 复制（fork）一个分支来回答，原会话不受影响 → MiniMax 克隆嗓音流式合成 → 机器人开口。
- **在家**：机器人走局域网直连 Mac 上的网关，不需要 VPS、证书或隧道。
- **想带出门**：再加一层 VPS 中转。家里仍然不开任何公网端口，由 Mac 主动打 ssh 反向隧道出去。家里 Wi-Fi 和手机热点自动切换，断网后自己恢复，SD 卡坏了也能开机。作者带去英国 8 天、家里无人值守，见下方实测。
- 它还能：说「你看看」时拍一张照片让 Claude 看了再回答；你用英文问就用英文答；连不上家里的 Mac 时播一句内置的提示音，不会一声不吭。

一句话：**StackChan 做身体，Mac 做中转和本地能力，你自己的 Claude Code 会话做大脑，但只给它嘴，不给它手。**（为什么不给手，见下文。）

## 怎么用这个仓库

- **想照着搭**：把仓库交给你的 AI（比如 Claude Code），让它读 [BUILD.md](BUILD.md) 按步骤做。那份是写给 AI 的：要你准备什么、按什么顺序、每步怎么验证、哪些地方不能改。
- **想知道为什么这么选**：看 [docs/JOURNEY.md](docs/JOURNEY.md)。写的是每一处为什么走这条路、在哪儿卡住过、怎么过去的。
- 每个坑更细的「症状 / 根因 / 修法 / 怎么验证的」在 [docs/FIELD_NOTES.md](docs/FIELD_NOTES.md)。

**诚实声明**：这条路我走通了一遍，是在一台机器上，由一个人走的。不保证你照抄就能跑通，但每个坑都写下来了。

## 架构

```
你 ──说话──▶ StackChan（身体：麦克风、喇叭、屏幕、摄像头、舵机、触摸、灯带）
               │ 在家：局域网 HTTP 直连
               │ 出门（可选）：HTTPS → VPS 中转 :8446（socat 终止 TLS）
               │              → ssh 反向隧道（Mac 主动连出去）
               ▼
          家里的 Mac：网关 :8124
           ├ 听：本地 SenseVoice :8127（带语气；mlx-whisper :8126 作备选）
           ├ 想：fork 一份你的 Claude Code 会话（不带工具）
           ├ 说：MiniMax 克隆嗓音 :8125，流式；设备要 mp3 就给 64 kbps
           └ 存：照片存到 Mac 本地
```

出门那条路上，机器人从来不直接连 Mac，它能离开家就是靠 VPS 和隧道这一层。端口、鉴权、每一跳的细节见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 大脑：用你的会话，但不给它手

机器人每说一句话，网关就从你的 Claude Code 会话 fork 出一个分支来回答，答完就丢，不写回原会话。这个分支：

- **带着**：这个会话的上下文。你们刚聊过什么、在做什么项目、会话里加载的设定，它都知道。所以它不是一个「新的 AI」，是那个窗口里的同一个。
- **不带**：任何工具。`--tools "" --strict-mcp-config`，hooks 全关。它不能读文件、不能跑命令、不能调 MCP。

为什么故意不给手？因为机器人的麦克风是外放的，**谁站在旁边说话它都会听**。要是这个分支能调工具，路过的人一句话就能让你的 Mac 执行命令。想让它真的动手做事，回到 Claude Code 窗口里说。

代价：
- 每轮要等 **几秒到十几秒**：经海外中转约 14 秒；在家局域网时听写不到 1 秒、大脑 3.9–11 秒（整轮没单独量过，见下方实测）。
- 隔久了（prompt cache 过期）第一句要把整个会话写进 cache，实测约 166k–203k tokens；之后几分钟内连着聊，每轮主要是读 cache，写入约 8k。
- 回复刻意简短：一两句，一般 50 字以内（这是 prompt 里的规则，不是代码截断）。

## 实测数据（2026-10-05，机器人连手机热点，经 VPS 中转）

| 环节 | 耗时 |
|---|---|
| STT（本地 mlx-whisper，含整段录音上传） | 3.7 s |
| Brain（fork Claude Code 会话） | 8.3 s |
| TTS 首字节（MiniMax 流式） | 1.6 s |
| **一轮总计（说完话 → 第一声）** | **约 14 s** |

- prompt cache：2026-10-05 连续 7 轮，第一轮 cache_write ≈ 203k，之后每轮 cache_read ≈ 195k、cache_write ≈ 8k。
- 隧道：Mac 设成接电永不睡眠后，VPS 每分钟探测一次，约 17 小时零失败（2026-10-04 15:1x → 2026-10-05 08:47）。
- 出门：2026-10-05 在公司用手机热点真机验证过断网 → 离线启动 → 开热点 → 自动恢复。
- 出国：2026-10-18 起带去英国 8 天，家里的 Mac 全程无人值守，没有一次需要回去修。中转隧道 8 天里只断过一次、约 1 分钟，自己重连；网关零报错。量不大（72 张照片、11 轮对话），照实写。路上发现的三个问题（相机偏色、照片上传中途卡住、连不上时不出声）回来已修。
- 在家局域网直连（换成 SenseVoice 之后，2026-10-28/29）：录音上传＋转写 0.52–0.73 s，大脑 3.9–11 s。

## 适合谁 / 不适合谁

适合：
- 手里有 StackChan（CoreS3 + 舵机套件），刷过 [ronron-gh/AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex) 系固件，或者愿意从它开始；
- Mac 上天天用 Claude Code，希望机器人的脑子就是那个会话；
- 能接受每轮几秒到十几秒的等待、短回复，以及隔久了第一句的 cache 成本。

不适合：想开箱即用、想要低延迟、电脑不能长期开着的人。

**如果你刚拿到 StackChan、还没让它说过一句话**：先照上游 [AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex) 的文档让它开口，再回来。这个仓库是从那之后开始的。

## 两段路，按顺序走

逐步做法、每步的验证在 [BUILD.md](BUILD.md)，这里只是概览。

**第一段：在家，让它用你的会话说话**
1. Mac 上起三个服务：SenseVoice（:8127，或 mlx-whisper :8126）、MiniMax TTS（:8125）、网关（:8124，`config/gateway.env.example`）。
2. 刷固件，按 `config/SC_ExConfig.yaml.example` / `config/SC_SecConfig.yaml.example` 配好，三个地址都填 `http://Mac的局域网IP:8124/...`。
3. 对机器人说话。2026-10-27 起作者在家就是这么跑的（现在的固件）。

**第二段（可选）：让它出门**
VPS 上起 socat TLS 终止（:8446 → 127.0.0.1:18124），Mac 上用 launchd 常驻 ssh 反向隧道，设备三个地址改成走 VPS；再加双 Wi-Fi（家里 + 热点）、离线自愈、SD → 内部存储镜像、mp3 音频、Mac 永不睡眠。逐项怎么配、怎么验证，见 [BUILD.md](BUILD.md) 的 Stage B 和 [docs/TRAVEL_MODE.md](docs/TRAVEL_MODE.md)。

## 已知限制

- 家里的电脑必须 **开着且不睡眠**，它就是脑子。
- 每轮要等几秒到十几秒；回复按设计很短。
- 需要 Claude Code 订阅；长时间没说话后的第一句比较贵。
- 中文优先（prompt、TTS 都是中文）；代码默认连 MiniMax 海外区（`api.minimax.io`），国内区账号要改 `MINIMAX_HOST`。
- 偶尔播放会卡一下（n=1，原因没查清）。
- 录音长度固定 3.75 秒，说长了后半句会被截掉。
- 天气默认城市由 `location.json`（`STACKCHAN_LOCATION_FILE`）决定，出门换城市改这个文件，不用重启。

## 和同类项目的区别

| 项目 | 一句话 | 和本方案的区别 |
|---|---|---|
| [migratorywhale/stackchan-mcp](https://github.com/migratorywhale/stackchan-mcp) | 机器人是一组 MCP 工具（say/listen/see/face/move/sense…），AI 客户端调用它 | 那边是「AI 长出手脚」；这边反过来，**机器人借你现成的 Claude Code 会话当脑子**，对话从设备发起 |
| [SerenQi/stackchan-claude-code](https://github.com/SerenQi/stackchan-claude-code) | 终端里的 Claude Code 通过 MCP 直接调用机器人硬件 | 那边是 **你在终端说话、AI 调设备**；这边是 **你对设备说话、设备调你的 Claude Code 会话** |
| [heavenchenggong/stackchan-claude-bridge](https://github.com/heavenchenggong/stackchan-claude-bridge) | 小智固件 + DeepSeek（~1 s）日常对话，Claude Code 作为慢速 MCP 工具被调用 | 那边日常对话是 DeepSeek；这边 **每一轮都是 Claude Code 本人回答**（代价是 ~14 s） |
| [tianyupaipai-cmd/stackchan-cloud-mcp](https://github.com/tianyupaipai-cmd/stackchan-cloud-mcp) | claude.ai 经 VPS + OAuth 2.1 驱动机器人，家里电脑可以关 | 那边大脑在 claude.ai 云端；这边是 **你 Mac 上那个有你全部上下文的 Claude Code 会话** |
| [BrettKinny/dotty-stackchan](https://github.com/BrettKinny/dotty-stackchan) | 全自托管语音栈：本地 ASR/TTS、可插拔 LLM、默认儿童模式 | 那边追求不经过云；这边接受云 TTS，换来 **克隆嗓音** 和 **完整 Claude Code 会话的上下文** |

以上描述逐条对照过各仓库 README（2026-10-05）。

## 仓库结构

```
README.md
BUILD.md            # 给 AI 的开工说明：准备、顺序、每步验证、不能改的地方
docs/
  JOURNEY.md        # 给人看的：为什么这么选、卡在哪、怎么过去的
  ARCHITECTURE.md   # 每一跳、端口、鉴权、brain 怎么 fork
  TRAVEL_MODE.md    # 出门要的五样东西，每样怎么验证
  FIELD_NOTES.md    # 踩坑：症状 / 根因 / 修法 / 验证
  CERTS.md          # VPS 证书怎么签（含设备 TLS 的坑）
config/             # 全是占位符的配置模板与 launchd plist
firmware/           # CoreS3 固件（fork of ronron-gh/AI_StackChan_Ex，上游 MIT）
gateway/            # Mac 网关 + cc_brain + 测试
extras/             # 语音识别服务（sensevoice/、Whisper 备选）、TTS 服务、隧道脚本
tools-voice/        # 触摸台词离线合成
tools-gif/          # GIF 表情素材打包、推送
tools-sd/           # SD 写入故障的排查脚本和原始数据
```

文档只列已经写好的。哪部分长大了再拆新文件。

## Credits

- 固件基于 [ronron-gh/AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex)（MIT），上游可追溯至 robo8080 的 AI Stack-chan；机器人本体是 [stack-chan](https://github.com/stack-chan/stack-chan)。
- 许可证：本仓库自己的代码用 MIT（见 [LICENSE](LICENSE)）。固件里有 GPLv3 的音频库代码，编译出的固件整体受 GPL-3.0 约束，详见 LICENSE 末尾。
- 大脑是 Anthropic Claude Code；嗓音由 MiniMax 合成；STT 为本地 SenseVoice（FunAudioLLM，经 sherpa-onnx），备选 mlx-whisper。

---

<a id="english"></a>
# English

## What this is

A **StackChan desk robot (M5Stack CoreS3)** whose brain is **the long-running Claude Code session on your Mac**.

- You speak → local SenseVoice transcribes (and hears the tone of voice, which is passed to the brain) → a fork of **your most recent Claude Code window** answers (the original session is untouched) → MiniMax cloned voice streams back → the robot speaks.
- **At home**: the robot talks to the gateway on your Mac over the LAN. No VPS, certificates or tunnel.
- **To take it out** (optional): add a VPS relay. Still no inbound ports at home: the Mac dials out an ssh reverse tunnel. Home Wi-Fi and phone hotspot switch automatically, it recovers by itself after losing the network, and boots even with a dead SD card. It ran 8 days in the UK with nobody home; see the numbers below.
- It can also take a photo and let Claude look before answering ("take a look"), answers in English when asked in English, and plays a built-in alert when it cannot reach the Mac instead of going silent.

In one line: **StackChan is the body, the Mac is the relay and local services, your own Claude Code session is the brain — with a mouth but no hands.**

## How to use this repo

- **To build it**: hand the repo to your AI agent (e.g. Claude Code) and have it follow [BUILD.md](BUILD.md) — prerequisites, order, a check for every step, and what must not be changed.
- **To see why it is built this way**: [docs/JOURNEY.md](docs/JOURNEY.md) (Chinese) — why each choice was made, where we got stuck, and how we got past it.
- Per-pitfall detail (symptom / root cause / fix / verification): [docs/FIELD_NOTES.md](docs/FIELD_NOTES.md).

**Honest disclaimer**: this path was walked once, on one robot. Copying it is not guaranteed to work, but every pitfall is written down.

## Architecture

```
you ──speak──▶ StackChan (body: mic, speaker, screen, camera, servos, touch, LEDs)
                 │ at home: plain HTTP over the LAN
                 │ outside (optional): HTTPS → VPS relay :8446 (socat terminates TLS)
                 │                     → ssh reverse tunnel (dialed out by the Mac)
                 ▼
            Mac at home: gateway :8124
             ├ hear:  local SenseVoice :8127 (with tone; mlx-whisper :8126 as fallback)
             ├ think: fork of your Claude Code session (no tools)
             ├ speak: MiniMax cloned voice :8125, streaming; 64 kbps mp3 on request
             └ keep:  photos saved on the Mac
```

On the outside path the robot never talks to the Mac directly; the VPS layer is what lets it leave home. Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## The brain: your session, without hands

Each turn, the gateway forks your Claude Code session, gets an answer, and throws the fork away. The fork **carries** the session's context — what you've been talking about and working on — so it is the same Claude as in that window, not a fresh one. It **does not carry** tools: `--tools "" --strict-mcp-config`, all hooks disabled. No file reads, no commands, no MCP.

Why no hands: the robot's microphone is open to the room. **Anyone standing next to it can talk to it.** If the fork could use tools, a passer-by could make your Mac run commands. To get real work done, go back to the Claude Code window.

Costs: several seconds to ~14 s per turn (~14 s through an overseas relay; on the home LAN, transcription under 1 s and the brain 3.9–11 s, full turn not separately measured); after a pause the first turn writes the whole session into the prompt cache (measured ~166k–203k tokens), follow-up turns mostly read it (~8k written per turn); replies are kept to one or two short sentences by prompt rule.

## Measured (2026-10-05, robot on a phone hotspot via the VPS relay)

| Stage | Time |
|---|---|
| STT (local mlx-whisper, incl. upload of the recording) | 3.7 s |
| Brain (forked Claude Code session) | 8.3 s |
| TTS first byte (MiniMax streaming) | 1.6 s |
| **End of speech → first sound** | **~14 s** |

- Cache: 7 turns in a row on 2026-10-05 — first turn cache_write ≈ 203k, then cache_read ≈ 195k and cache_write ≈ 8k per turn.
- Tunnel: with the Mac set to never sleep on AC, a 1-minute probe from the VPS saw zero failures over ~17 h.
- Out of the house: offline boot → hotspot on → automatic recovery verified at an office on 2026-10-05.
- Abroad: taken to the UK for 8 days from 2026-10-18 with the Mac at home unattended; nothing needed fixing on site. The tunnel dropped once for about a minute and reconnected by itself; zero gateway errors. Light use (72 photos, 11 voice turns). Three issues found on the road (camera colour cast, stalled photo uploads, silence when the Mac was unreachable) were fixed afterwards.
- At home on LAN with SenseVoice (2026-10-28/29): upload + transcription 0.52–0.73 s, brain 3.9–11 s.

## Who it's for

You own a StackChan on an [AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex)-family firmware, live in Claude Code on a Mac, and accept turns of several seconds to ~14 s. Not for plug-and-play, low latency, or a computer that can't stay on. **If your StackChan hasn't spoken yet**, start with the upstream docs and come back.

## Two stages, in order

Step-by-step with checks: [BUILD.md](BUILD.md).

1. **At home, make it speak with your session**: run SenseVoice (:8127, or mlx-whisper :8126), MiniMax TTS (:8125) and the gateway (:8124) on the Mac; flash firmware with the `config/` templates, all three endpoints at `http://<Mac LAN IP>:8124/...`. This is how the author runs it at home since 2026-10-27, on the current firmware.
2. **Take it outside (optional)**: socat TLS on a VPS (:8446 → 127.0.0.1:18124) plus a launchd-managed ssh reverse tunnel, device endpoints switched to the relay; then dual Wi-Fi, offline self-heal, SD → internal-flash mirror, mp3 audio, Mac never sleeps. See Stage B in [BUILD.md](BUILD.md) and [docs/TRAVEL_MODE.md](docs/TRAVEL_MODE.md).

## Layout

```
README.md
BUILD.md   instructions for the AI agent doing the build
docs/      JOURNEY.md (why it's built this way) · ARCHITECTURE.md · TRAVEL_MODE.md · FIELD_NOTES.md
config/    placeholder-only config templates and launchd plists
docs/      CERTS.md (VPS certificate, incl. the device TLS pitfall)
firmware/  CoreS3 firmware (fork of ronron-gh/AI_StackChan_Ex, upstream MIT)
gateway/   Mac gateway + cc_brain + tests
extras/    STT servers (sensevoice/, Whisper fallback), TTS server, tunnel script
tools-*/   touch-voice synthesis, GIF packing, SD diagnostics
```

## Known limits

Home computer must stay on and awake; several seconds to ~14 s per turn; Claude Code subscription required and the first turn after a pause is the expensive one; Chinese-first; MiniMax overseas host by default (`MINIMAX_HOST` for mainland accounts); an occasional playback stutter (seen once, cause unknown); recording is a fixed 3.75 s window, so long sentences get cut; the default weather city comes from `location.json` (`STACKCHAN_LOCATION_FILE`), editable without a restart.

## Related projects

See the comparison table in the Chinese section above — migratorywhale/stackchan-mcp, SerenQi/stackchan-claude-code, heavenchenggong/stackchan-claude-bridge, tianyupaipai-cmd/stackchan-cloud-mcp, BrettKinny/dotty-stackchan. In short: most of them let an AI drive the robot as a tool; here the robot calls *your* Claude Code session as its brain, and every turn is answered by it.

## Credits

Firmware based on [ronron-gh/AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex) (MIT), descending from robo8080's AI Stack-chan; robot body: [stack-chan](https://github.com/stack-chan/stack-chan). License: MIT for this repo's own code ([LICENSE](LICENSE)); the firmware includes GPL-3.0 audio code, so a compiled firmware image is GPL-3.0 — see the end of LICENSE. Brain: Anthropic Claude Code; voice: MiniMax; STT: local SenseVoice (FunAudioLLM, via sherpa-onnx), mlx-whisper as fallback.
