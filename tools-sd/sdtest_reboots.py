#!/usr/bin/env python3
"""跨开机的 SD 失败率 —— 口径跟 sdtest_matrix.py 第一个配置完全一致, 数字可直接对比。

为什么要单独一个: 07-27 证实「模式在开机时定死」(20 次开机 13 坏/5 好/2 混合),
所以失败率这个数只有**跨多次开机**才有意义。单次开机连跑 N 轮测的是同一个模式,
n 再大也还是 n=1。sdtest_cold.py 那种连跑不复位的口径不能拿来跟基线比。

基线(07-27, 同口径 5 轮 x 20 次): 修 SPI 前 45% / 修 SPI 后 83%。

用法: sdtest_reboots.py [开机次数=5]
"""
import sys, time, glob, re, serial

BOOTS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
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
            t_end = time.time() + 2
            while time.time() < t_end:
                more = ser.readline().decode("utf-8", errors="replace")
                if more:
                    out.append(more)
            break
    return "".join(out)


print(f"端口 {ports[0]} / 每次开机跑一组 `{CMD}`, 共 {BOOTS} 次开机")
print("口径 = sdtest_matrix.py 第一个配置 (基线: 修 SPI 前 45% / 后 83%)\n", flush=True)
print(f"{'开机':>4}  {'开机SD错':>7}  {'失败/总数':>10}  {'失败率':>7}  {'断裂':>5}  {'静默损坏':>8}  判读", flush=True)
print("-" * 72, flush=True)

rows = []
for b in range(1, BOOTS + 1):
    errs = reset_and_capture_boot()
    text = run(CMD)
    m, d = SUMMARY.search(text), DETAIL.search(text)
    if not m:
        print(f"{b:>4}  {errs:>7}  ⚠️ 没解析到汇总行(设备可能在重启, 等 10s+)", flush=True)
        print("   ", text.strip().replace("\n", "\n    ")[-400:], flush=True)
        continue
    total, ok, fail = map(int, m.groups())
    wf, vf, of, rf = map(int, d.groups()) if d else (0, 0, 0, 0)
    rate = 100.0 * fail / total
    verdict = "好模式" if rate <= 10 else ("坏模式" if rate >= 60 else "★中间态")
    rows.append((errs, total, fail, wf, vf))
    print(f"{b:>4}  {errs:>7}  {fail:>4}/{total:<5}  {rate:>6.1f}%  "
          f"{wf:>5}  {vf:>8}  {verdict}", flush=True)

ser.close()

if not rows:
    sys.exit("\n一轮都没跑成 —— 先确认设备在串口上活着。")

tot = sum(r[1] for r in rows); bad = sum(r[2] for r in rows)
print(f"\n合计 {len(rows)} 次开机: {bad}/{tot} = {100.0*bad/tot:.1f}%  (基线 修前45% / 修后83%)")
good_boots = sum(1 for r in rows if 100.0 * r[2] / r[1] <= 10)
bad_boots = sum(1 for r in rows if 100.0 * r[2] / r[1] >= 60)
print(f"开机模式分布: 好 {good_boots} / 坏 {bad_boots} / 中间 {len(rows)-good_boots-bad_boots}"
      f"   (07-27 基线分布: 好 5 / 坏 13 / 混合 2, 共 20 次开机)")
print("⚠️ n=5 次开机, 坏模式基线占比 65% —— 5 次全坏的概率约 12%, 不算罕见。别拿它单独定案。")
