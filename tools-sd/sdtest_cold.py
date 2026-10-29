#!/usr/bin/env python3
"""冷机开机 SD 自检 —— 07-27 留下的那一条判据。

背景(07-27 晚间实测走势): 早上 45% -> 连续压测一小时后 100% -> 断电歇一阵后 85%。
"歇过就好一点"这条走势提示是温度/累积状态, 但没验过。

本脚本做两件事:
  1. 设备冷着的时候, 第一轮 `sdtest rounds=20` 就是判据本身:
     回到 ~45% -> 温度坐实, 设备没坏, SPI 修复要重跑矩阵才判得了;
     仍在 85% 上下 -> 设备真退化了, 转硬件(卡槽接触/焊点), 软件层收工。
  2. 连跑若干轮不复位, 把"越跑越坏"的升温曲线画出来 —— 第一轮之后的都是白捡的。

⚠️ 一轮 = 20 次写。别拿它当压测跑一小时: 07-27 那次连续压测把卡的 FAT 写坏过一次。
用法: sdtest_cold.py [轮数=5]
"""
import sys, time, glob, re, serial

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
CMD = "sdtest rounds=20"
BOOT_WAIT = 14

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("❌ 找不到 /dev/cu.usbmodem* —— 设备没插 USB")

ser = serial.Serial()
ser.port = ports[0]; ser.baudrate = 115200; ser.timeout = 0.3
ser.dtr = False; ser.rts = False
ser.open()

SUMMARY = re.compile(r"共 (\d+) 次, 成功 (\d+), 失败 (\d+)")
DETAIL = re.compile(r"写入断裂 (\d+) / \*\*静默损坏 (\d+)\*\* / open 失败 (\d+) / rename 失败 (\d+)")
BOOTERR = re.compile(r"ff_sd_status|sdcard_mount|SelectCard")


def reset_and_capture_boot():
    """硬复位一次, 顺带数开机日志里的 SD 错误行(07-27 猜过它可能是模式的预测信号)。"""
    ser.setDTR(False); ser.setRTS(True);  time.sleep(0.12)
    ser.setRTS(False); ser.setDTR(False)
    ser.reset_input_buffer()
    t0, errs = time.time(), 0
    while time.time() - t0 < BOOT_WAIT:
        line = ser.readline().decode("utf-8", errors="replace")
        if line and BOOTERR.search(line):
            errs += 1
    ser.reset_input_buffer()
    return errs


def run(cmd, timeout=180):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode()); ser.flush()
    out, t0 = [], time.time()
    while time.time() - t0 < timeout:
        line = ser.readline().decode("utf-8", errors="replace")
        if not line:
            continue
        out.append(line)
        if SUMMARY.search(line):
            # 汇总行之后还有一行明细, 多收一点再走
            t_end = time.time() + 2
            while time.time() < t_end:
                more = ser.readline().decode("utf-8", errors="replace")
                if more:
                    out.append(more)
            break
    return "".join(out)


boot_errs = reset_and_capture_boot()
t_boot = time.time()
print(f"端口 {ports[0]} / 开机日志 SD 错误行: {boot_errs}")
print(f"连跑 {ROUNDS} 轮 `{CMD}`, 中间不复位 —— 第 1 轮是判据, 后面看升温\n", flush=True)
print(f"{'轮':>3}  {'开机后':>7}  {'失败/总数':>10}  {'失败率':>7}  {'断裂':>5}  {'静默损坏':>8}", flush=True)
print("-" * 56, flush=True)

results = []
for i in range(1, ROUNDS + 1):
    text = run(CMD)
    m, d = SUMMARY.search(text), DETAIL.search(text)
    if not m:
        print(f"{i:>3}  {'':>7}  ⚠️ 没解析到汇总行(设备可能崩了/在重启, 等 10s+)", flush=True)
        print("   ", text.strip().replace("\n", "\n    ")[-400:], flush=True)
        continue
    total, ok, fail = map(int, m.groups())
    wf, vf, of, rf = map(int, d.groups()) if d else (0, 0, 0, 0)
    elapsed = time.time() - t_boot
    results.append((elapsed, total, fail, wf, vf))
    print(f"{i:>3}  {elapsed:>6.0f}s  {fail:>4}/{total:<5}  "
          f"{100.0*fail/total:>6.1f}%  {wf:>5}  {vf:>8}", flush=True)

ser.close()

if not results:
    sys.exit("\n一轮都没跑成 —— 先确认设备在串口上活着。")

first = results[0]
print(f"\n第 1 轮(最冷): 失败率 {100.0*first[2]/first[1]:.1f}%")
print("  ~45% 上下 -> 温度/累积状态坐实, 设备没坏 -> 重跑 sdtest_matrix.py 才判得了 SPI 修复")
print("  ~85% 上下 -> 设备真退化了 -> 转硬件(卡槽接触/焊点), 软件层收工")

if len(results) > 1:
    tot = sum(r[1] for r in results); bad = sum(r[2] for r in results)
    print(f"\n全程 {len(results)} 轮合计: {bad}/{tot} = {100.0*bad/tot:.1f}%")
    print(f"走势: " + " -> ".join(f"{100.0*r[2]/r[1]:.0f}%" for r in results))
    print("  单调上升 -> 升温假设成立; 平的 -> 跟连续跑无关, 是开机时就定死的那个模式")
