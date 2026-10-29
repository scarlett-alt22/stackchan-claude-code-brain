#!/usr/bin/env python3
"""量"重挂 SD 能不能重新摇出好模式"。

每次开机跑一次 sdreroll。只统计**开机就是坏模式**的那些(好模式的没什么可摇的)。
用法: sdreroll_trial.py [开机次数] [clk_MHz]
"""
import sys, time, glob, re, serial

BOOTS = int(sys.argv[1]) if len(sys.argv) > 1 else 14
CLK = sys.argv[2] if len(sys.argv) > 2 else "10"
BOOT_WAIT = 14

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("❌ 找不到 /dev/cu.usbmodem*")
ser = serial.Serial()
ser.port = ports[0]; ser.baudrate = 115200; ser.timeout = 0.3
ser.dtr = False; ser.rts = False
ser.open()

PROBE = re.compile(r"第(\d+)次探测: (\d+)/(\d+) 失败")
WON = re.compile(r"摇到好模式, 用了 (\d+) 次重挂")
LOST = re.compile(r"(\d+) 次重挂都没摇出好模式")

print(f"每次开机跑一次 `sdreroll tries=8 clk={CLK}`, 共 {BOOTS} 次开机\n", flush=True)
print(f"{'开机':>4}  {'逐次探测失败数':<26}  结果", flush=True)
print("-" * 60, flush=True)

born_good = 0
rescued = []
failed = 0

for b in range(1, BOOTS + 1):
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.12)
    ser.setRTS(False); ser.setDTR(False)
    ser.reset_input_buffer(); time.sleep(BOOT_WAIT); ser.reset_input_buffer()

    ser.write(f"sdreroll tries=8 clk={CLK}\n".encode()); ser.flush()
    probes, verdict, t0 = [], None, time.time()
    while time.time() - t0 < 240:
        line = ser.readline().decode("utf-8", errors="replace")
        if not line:
            continue
        m = PROBE.search(line)
        if m:
            probes.append(int(m.group(2)))
        if WON.search(line):
            verdict = ("won", int(WON.search(line).group(1))); break
        if LOST.search(line):
            verdict = ("lost", 8); break
        if "挂不上了" in line:
            verdict = ("dead", 0); break

    if verdict is None:
        print(f"{b:>4}  {str(probes):<26}  ⚠️ 超时/没解析到", flush=True)
        continue
    kind, n = verdict
    if kind == "won" and n == 0:
        born_good += 1
        note = "开机就是好模式(不计入)"
    elif kind == "won":
        rescued.append(n)
        note = f"✅ 重挂 {n} 次后转好"
    elif kind == "dead":
        note = "❌ 重挂后挂不上了"
        failed += 1
    else:
        failed += 1
        note = "❌ 8 次重挂都没救回来"
    print(f"{b:>4}  {str(probes):<26}  {note}", flush=True)

ser.close()

tried = len(rescued) + failed
print(f"\n共 {BOOTS} 次开机: 开机即好 {born_good}, 开机即坏 {tried}")
if tried:
    print(f"坏模式中被重挂救回: {len(rescued)}/{tried} = {100*len(rescued)/tried:.0f}%")
    if rescued:
        print(f"救回时用的重挂次数: {rescued}  (中位数 {sorted(rescued)[len(rescued)//2]})")
    print("\n救回率高 -> 开机自检+重挂是条真出路, 可以落进固件")
    print("救回率低 -> 模式不由挂载决定, 该往供电/温度查")
