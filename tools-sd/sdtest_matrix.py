#!/usr/bin/env python3
"""跑 SD 自检矩阵: 对每个配置重复若干轮, 汇总失败率。

用法: sdtest_matrix.py [重复次数]
"""
import sys, time, glob, re, serial, collections

REPEATS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
CONFIGS = [
    "sdtest rounds=20",
    "sdtest rounds=20 wifi=off",
    "sdtest rounds=20 gif=off",
    "sdtest rounds=20 wifi=off gif=off",
    "sdtest rounds=20 norename",
]
# 🔴 wifi=off 之后固件不自己重连(故意的, 免得把重连过程混进下一轮测量),
#    所以每个**配置**跑完都硬复位一次, 保证各配置起点一致。
BOOT_WAIT = 14

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("❌ 找不到 /dev/cu.usbmodem*")

ser = serial.Serial()
ser.port = ports[0]; ser.baudrate = 115200; ser.timeout = 0.3
ser.dtr = False; ser.rts = False
ser.open()


def reset_and_wait():
    ser.setDTR(False); ser.setRTS(True);  time.sleep(0.12)
    ser.setRTS(False); ser.setDTR(False)
    ser.reset_input_buffer()
    time.sleep(BOOT_WAIT)
    ser.reset_input_buffer()


def run(cmd, timeout=180):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    out, t0 = [], time.time()
    while time.time() - t0 < timeout:
        line = ser.readline().decode("utf-8", errors="replace")
        if not line:
            continue
        out.append(line)
        if "本测试**不碰网络**" in line or "本测试" in line and "不碰网络" in line:
            break
    return "".join(out)


SUMMARY = re.compile(r"共 (\d+) 次, 成功 (\d+), 失败 (\d+)")
DETAIL = re.compile(r"写入断裂 (\d+) / \*\*静默损坏 (\d+)\*\* / open 失败 (\d+) / rename 失败 (\d+)")

agg = collections.OrderedDict((c, collections.Counter()) for c in CONFIGS)

for cfg in CONFIGS:
    print(f"\n{'='*66}\n配置: {cfg}\n{'='*66}", flush=True)
    for i in range(REPEATS):
        reset_and_wait()
        text = run(cfg)
        m, d = SUMMARY.search(text), DETAIL.search(text)
        if not m:
            print(f"  第{i+1}轮: ⚠️ 没解析到汇总行(设备可能崩了)", flush=True)
            print("   ", text.strip().replace("\n", "\n    ")[-500:], flush=True)
            continue
        total, ok, fail = map(int, m.groups())
        wf, vf, of, rf = map(int, d.groups()) if d else (0, 0, 0, 0)
        a = agg[cfg]
        a["total"] += total; a["ok"] += ok; a["fail"] += fail
        a["writefail"] += wf; a["verifyfail"] += vf
        a["openfail"] += of; a["renamefail"] += rf
        print(f"  第{i+1}轮: {total} 次 / 失败 {fail} "
              f"(断裂{wf} 静默损坏{vf} open{of} rename{rf})", flush=True)

ser.close()

print(f"\n\n{'='*78}\n汇总 (每个配置 {REPEATS} 轮 x 20 次)\n{'='*78}")
print(f"{'配置':<36} {'总数':>5} {'失败':>5} {'失败率':>8} {'断裂':>5} {'静默损坏':>9}")
print("-" * 78)
for cfg, a in agg.items():
    if not a["total"]:
        print(f"{cfg:<36} {'—— 无数据 ——':>28}")
        continue
    rate = 100.0 * a["fail"] / a["total"]
    print(f"{cfg:<36} {a['total']:>5} {a['fail']:>5} {rate:>7.1f}% "
          f"{a['writefail']:>5} {a['verifyfail']:>9}")
