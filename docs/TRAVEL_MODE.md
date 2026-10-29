# TRAVEL_MODE（出国模式）

目标：机器人连手机热点，Mac 留在家里，一切照常。四个支柱：VPS 中继、ssh 反向隧道、双 Wi-Fi、离线自愈。每节末尾是“怎么验证”。

## 1. VPS 中继：家里零公网端口

设备只做 **outbound HTTPS** 到 `https://YOUR_VPS_IP:8446`。VPS 上：

```
socat OPENSSL-LISTEN:8446,cert=...,verify=0,fork TCP:127.0.0.1:18124
```

TLS 在 VPS 终止，明文只走 VPS 本地回环。家里路由器**不需要**端口转发、不需要 DDNS。

**验证**：在 VPS 上 `curl -k https://127.0.0.1:8446/` 应通；从 Mac `curl http://127.0.0.1:8124/health` 应返回 `{"status": "ok"}`；设备端看串口确认连上的是 VPS 地址而非家里 LAN。

## 2. ssh 反向隧道：把 18124 接回家（`extras/stackchan-tunnel.sh`）

```sh
exec /usr/bin/ssh -N \
  -o BatchMode=yes -o ExitOnForwardFailure=yes \
  -o ServerAliveInterval=15 -o ServerAliveCountMax=3 -o ConnectTimeout=15 \
  -R 127.0.0.1:18124:127.0.0.1:8124 your-vps
```

- 由 launchd 常驻（`KeepAlive`）；`exec` 让 ssh 顶替脚本进程成为 launchd 看管的 pid，停任务时直接停到 ssh，不留孤儿占远端端口。
- 保活：`ServerAliveInterval=15 ×3`，约 45 秒判定断线；每次（重）连打一行带时间戳的 `starting`，断线时间看相邻两行 `starting` 的间隔。
- 已知行为：ssh 一退出 launchd 就把整组进程清掉，所以“ssh exited”那行永远写不出来——不要指望它，看 `starting` 行即可。
- Mac 必须设为**接电永不睡眠**（`System Settings → Battery → Power adapter → Prevent automatic sleeping`），否则隧道跟着睡。

**验证**：从 VPS 每分钟 `curl` 一次网关 `/health` 穿过整条链路；2026-10-04 15:1x → 2026-10-05 08:47 约 17 小时零失败。看隧道日志里 `starting` 行的间隔是否异常。

## 3. 双 Wi-Fi：家里 + 手机热点

secret yaml 里配两段：

```yaml
wifi:
  ssid: "YOUR_HOME_SSID"
  password: "YOUR_HOME_PASSWORD"
wifi2:
  ssid: "YOUR_HOTSPOT_SSID"
  password: "YOUR_HOTSPOT_PASSWORD"
```

`wifi_dual.h` 的 `wifi_build_ap_list()`：`wifi` 必选、`wifi2` 可选；`wifi2` 为空或与 `wifi` 同 SSID 则去重；空密码的开放网络照样加入。去重后的列表交给 `WiFiMulti`，**连信号最好的一个**。

**验证**：先连家里 Wi-Fi，确认在线；再关掉家里 AP（或带出门），看设备在 40 秒 boot budget 内切到热点并恢复对话。`wifi_scan_report()` 在连不上时打诊断（只打配置里的两个 SSID，不打邻居——不泄露周围网络）。

## 4. 离线自愈：断网不断魂

| 场景 | 行为（`firmware/src/main.cpp`） |
|---|---|
| 开机 40 秒内没连上任何 AP | 进离线模式；之后**每 20 秒**重试一次 |
| 重试时连上了 | 立刻 `ESP.restart()`——重启后“前回设定”就是刚连上的网，秒连，mods 按在线模式干净初始化（注释原话：半路热切换在线状态“改不干净”） |
| 在线时掉线 | 每 10 秒 `wifiMulti.run(5000)` 重连 |
| 配了 AP | 跳过 SmartConfig 30 秒干等，直接进离线重试循环 |

另：`wifi_register_aps()` 在开机即登记 AP（修过一个 bug：原来只在“前回设定失败”时登记，导致 loop 里断线重连永远连不回去）。

**验证**：2026-10-05 用屏幕照片验证过离线自愈——断网后续网，设备自动回来不需要碰。出门前建议复测一次：热点开关两次，看串口/屏幕状态。

## 5. SD 卡是可选外设：SPIFFS 镜像

开机流程：SD 好 → 照常从 SD 读 `SC_ExConfig.yaml`、CA、触摸语音，**顺手镜像进 SPIFFS**（`SpiffsMirror`）；SD 挂载失败 → 用上次的镜像从 SPIFFS 起机。

背景：之前 SD 挂载失败会 `ESP.restart()` 无限重启——那张卡第三次 FAT 报废，整机就是这么变砖的。现在 SD 卡可以一直拔掉。

**验证**：拔掉 SD 卡冷启动一次，确认正常进系统、配置与 CA 都在（看串口 `[Mirror]` 相关行）。

## 出国前检查单

- [ ] Mac 接电永不睡眠已设置；隧道日志近 24 小时无异常间隔
- [ ] `wifi2` 配的是**出国那台手机**的热点 SSID/密码（注意 iPhone 热点名称含中文/空格也能用，但建议先实测）
- [ ] 拔 SD 卡冷启动一次通过
- [ ] 热点开关两次，离线自愈通过
- [ ] VPS 侧 socat 与 ssh 隧道开机自启（VPS 重启后能自己回来）
- [ ] **家里路由器重启后隧道能自己连回来**：人在国外时这是唯一修不了的故障。出发前拔一次路由器电源，看隧道日志在路由器恢复后出现新的 `starting` 行、从 VPS 能 curl 通
- [ ] MiniMax / mlx-whisper / 网关三个 launchd 任务都是 loaded 状态
- [ ] 在国外第一晚：让朋友帮忙看一眼家里 Mac 是否亮着（或 VPS 探测链路是否通）

## 回国后

什么都不用改：设备回到家里 Wi-Fi 信号范围会自动连回 `wifi`（信号最好的优先）。
