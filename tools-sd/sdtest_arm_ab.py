#!/usr/bin/env python3
"""A/B 那一行 SPI.begin() —— 07-27 那个修复到底是修好了还是修坏了。

为什么不能靠"回滚代码"对照: 07-27 那一刀绑了两件事(加 SPI.begin() + 五处漏改的
25MHz 降到 10MHz), 回滚会一次动两个变量。所以做成 NVS 里的 arm 开关, 一次烧录,
两臂时钟都锁 10MHz, 唯一变量就是那一行。

为什么必须**跨开机交替**而不是"先跑 6 次 on 再跑 6 次 off":
  · 07-27 证实模式在开机时定死 -> 失败率只有跨多次开机才有意义;
  · 07-27 还观察到设备状态会随时间漂(45% -> 压测后 100%) -> 分段跑会把漂移
    整个记到后半段那个 arm 头上。交替能把线性漂移抵消掉。

每次试验: 设 arm -> 硬复位 -> 从开机日志**读回** arm 真实值(不信自己刚设过) ->
          跑一组 sdtest rounds=20。

用法: sdtest_arm_ab.py [每臂次数=6]
"""
import sys, time, glob, re, serial

PER_ARM = int(sys.argv[1]) if len(sys.argv) > 1 else 6
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
ARMLINE = re.compile(r"\[SDcfg\] arm=(on|off)")
BOOTERR = re.compile(r"ff_sd_status|sdcard_mount|SelectCard")


def set_arm(arm):
    """设 NVS。设备必须活着。复位后才生效。"""
    ser.reset_input_buffer()
    ser.write(f"sdspi {arm}\n".encode()); ser.flush()
    t0 = time.time()
    while time.time() - t0 < 5:
        line = ser.readline().decode("utf-8", errors="replace")
        if "[SDcfg] SPI.begin() 已设为" in line:
            return True
    return False


def reset_and_read_arm():
    """硬复位, 从开机日志里读回真实生效的 arm —— 判据不落在"我刚设过"上。"""
    ser.setDTR(False); ser.setRTS(True);  time.sleep(0.12)
    ser.setRTS(False); ser.setDTR(False)
    ser.reset_input_buffer()
    t0, errs, seen = time.time(), 0, None
    while time.time() - t0 < BOOT_WAIT:
        line = ser.readline().decode("utf-8", errors="replace")
        if not line:
            continue
        if BOOTERR.search(line):
            errs += 1
        m = ARMLINE.search(line)
        if m and seen is None:
            seen = m.group(1)
    ser.reset_input_buffer()
    return seen, errs


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


order = []
for i in range(PER_ARM):
    order += ["on", "off"]          # 交替, 不是分段

print(f"端口 {ports[0]} / 每臂 {PER_ARM} 次开机, 交替顺序: {' '.join(order)}")
print(f"每次开机跑一组 `{CMD}`。基线(07-27 同口径 5 开机): 修前 45% / 修后 83%;"
      f" 07-28 冷机 8 开机 76.2%\n", flush=True)
print(f"{'#':>3}  {'设定':>4}  {'实测arm':>7}  {'开机SD错':>7}  {'失败/总数':>10}  {'失败率':>7}  {'断裂':>5}  {'静默':>5}", flush=True)
print("-" * 72, flush=True)

rows = []
for i, want in enumerate(order, 1):
    if not set_arm(want):
        print(f"{i:>3}  {want:>4}  ⚠️ 设 arm 没收到回执(设备可能没在跑), 跳过", flush=True)
        continue
    got, errs = reset_and_read_arm()
    if got is None:
        print(f"{i:>3}  {want:>4}  ⚠️ 开机日志里没读到 [SDcfg] arm= —— 这一次作废", flush=True)
        continue
    if got != want:
        print(f"{i:>3}  {want:>4}  {got:>7}  ❌ arm 没生效, 这一次作废", flush=True)
        continue
    text = run(CMD)
    m, d = SUMMARY.search(text), DETAIL.search(text)
    if not m:
        print(f"{i:>3}  {want:>4}  {got:>7}  {errs:>7}  ⚠️ 没解析到汇总行", flush=True)
        continue
    total, ok, fail = map(int, m.groups())
    wf, vf, of, rf = map(int, d.groups()) if d else (0, 0, 0, 0)
    rows.append((got, total, fail, wf, vf))
    print(f"{i:>3}  {want:>4}  {got:>7}  {errs:>7}  {fail:>4}/{total:<5}  "
          f"{100.0*fail/total:>6.1f}%  {wf:>5}  {vf:>5}", flush=True)

ser.close()

if not rows:
    sys.exit("\n一次有效试验都没有 —— 先确认设备在串口上活着。")

print(f"\n{'='*72}")
for arm in ("on", "off"):
    sel = [r for r in rows if r[0] == arm]
    if not sel:
        print(f"arm={arm:<4} —— 无有效数据")
        continue
    tot = sum(r[1] for r in sel); bad = sum(r[2] for r in sel)
    each = " ".join(f"{100.0*r[2]/r[1]:.0f}%" for r in sel)
    label = "07-27 修复(加 SPI.begin)" if arm == "on" else "07-27 之前的行为"
    print(f"arm={arm:<4} {len(sel)} 次开机  {bad:>3}/{tot:<4} = {100.0*bad/tot:>5.1f}%   "
          f"[{each}]   {label}")

on = [r for r in rows if r[0] == "on"]; off = [r for r in rows if r[0] == "off"]
if on and off:
    r_on = 100.0 * sum(r[2] for r in on) / sum(r[1] for r in on)
    r_off = 100.0 * sum(r[2] for r in off) / sum(r[1] for r in off)
    print(f"\n差值 on - off = {r_on - r_off:+.1f} 个百分点")
    print("  明显为正 -> 07-27 那一刀是负面的, 该撤(至少 SPI.begin 这半边)")
    print("  明显为负 -> 那一刀有用, 涨到 83% 得另找解释")
    print("  差不多平 -> SPI.begin 无关, 两臂都坏 -> 转硬件(卡槽接触/焊点/供电)")
    print(f"\n⚠️ 每臂只有 {len(on)}/{len(off)} 次开机。07-27 实测开机间方差很大"
          f"(一贯好 25% / 一贯坏 65%), 小差值(<15 点)别当结论。")
