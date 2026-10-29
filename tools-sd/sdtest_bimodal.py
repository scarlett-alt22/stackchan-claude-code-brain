#!/usr/bin/env python3
"""测「好/坏模式」是不是开机时定死的。

每次开机连跑 3 组 sdtest(中间不复位)。若模式在挂载时定死, 三组应当同向;
若模式会自己翻, 会看到 好-坏-好 这类混合。
"""
import sys, time, glob, re, serial

BOOTS = int(sys.argv[1]) if len(sys.argv) > 1 else 20
SUBRUNS = 3
CMD = "sdtest rounds=6"
BOOT_WAIT = 14

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("❌ 找不到 /dev/cu.usbmodem*")

ser = serial.Serial()
ser.port = ports[0]; ser.baudrate = 115200; ser.timeout = 0.3
ser.dtr = False; ser.rts = False
ser.open()

SUMMARY = re.compile(r"共 (\d+) 次, 成功 (\d+), 失败 (\d+)")
# 开机那行 SD 错误可能是模式的预测信号 —— 一并记下来看看能不能提前判死
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


def run_once():
    ser.reset_input_buffer()
    ser.write((CMD + "\n").encode()); ser.flush()
    t0 = time.time()
    while time.time() - t0 < 120:
        line = ser.readline().decode("utf-8", errors="replace")
        if not line:
            continue
        m = SUMMARY.search(line)
        if m:
            return int(m.group(3))          # 失败数
    return None


rows = []
print(f"每次开机连跑 {SUBRUNS} 组 `{CMD}`(组间不复位), 共 {BOOTS} 次开机\n", flush=True)
print(f"{'开机':>4}  {'开机SD错':>7}  {'三组失败数(每组6次)':<22}  判读", flush=True)
print("-" * 62, flush=True)
for b in range(1, BOOTS + 1):
    errs = reset_and_capture_boot()
    fails = [run_once() for _ in range(SUBRUNS)]
    rows.append((errs, fails))
    if any(f is None for f in fails):
        verdict = "⚠️ 有组没解析到"
    elif all(f <= 1 for f in fails):
        verdict = "好模式(一贯)"
    elif all(f >= 4 for f in fails):
        verdict = "坏模式(一贯)"
    else:
        verdict = "★混合 —— 模式会翻"
    print(f"{b:>4}  {errs:>7}  {str(fails):<22}  {verdict}", flush=True)

ser.close()

good = sum(1 for _, f in rows if all(x is not None and x <= 1 for x in f))
bad = sum(1 for _, f in rows if all(x is not None and x >= 4 for x in f))
mixed = len(rows) - good - bad
print(f"\n共 {len(rows)} 次开机: 一贯好 {good} / 一贯坏 {bad} / 混合 {mixed}")
print("混合占比高 -> 模式在一次开机内会翻, 不是挂载时定死的")
print("混合几乎没有 -> 模式在挂载时(或更早)就定了, 该往 SD 初始化那段查")

# 开机 SD 错误数能不能预测?
g = [e for e, f in rows if all(x is not None and x <= 1 for x in f)]
d = [e for e, f in rows if all(x is not None and x >= 4 for x in f)]
if g and d:
    print(f"\n开机 SD 错误行数: 好模式均值 {sum(g)/len(g):.1f}, 坏模式均值 {sum(d)/len(d):.1f}")
    print("两者差得开 -> 开机日志就能提前判死, 是个便宜的预测信号")
