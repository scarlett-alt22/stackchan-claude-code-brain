#!/usr/bin/env python3
"""抓 StackChan 串口。可选先用 DTR/RTS 软复位拿一份干净的开机日志。

用法: serial_capture.py <秒数> [--reset] [--out 文件]
"""
import sys, time, glob, serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 15
do_reset = "--reset" in sys.argv
out = None
if "--out" in sys.argv:
    out = sys.argv[sys.argv.index("--out") + 1]

ports = sorted(glob.glob("/dev/cu.usbmodem*"))  # 端口号会变, 别写死(docs/JOURNEY.md「SD 卡」一节)
if not ports:
    sys.exit("❌ 找不到 /dev/cu.usbmodem*")
port = ports[0]

# 🔴 不能直接 serial.Serial(port,...): 开端口那一下会拉 DTR 把 ESP32 复位掉,
#    实验跑到一半设备重启就全废了。先建对象不开、把 DTR/RTS 摁住, 再 open()。
ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()
if do_reset:
    # 跟 esptool 同一套: DTR/RTS 一拉一放 = 硬复位, 不用主人动手按键
    ser.setDTR(False); ser.setRTS(True);  time.sleep(0.12)
    ser.setRTS(False); ser.setDTR(False); time.sleep(0.05)
    ser.reset_input_buffer()

print(f"# {port} @115200  抓 {secs}s" + ("  (已复位)" if do_reset else ""), flush=True)
buf, t0 = [], time.time()
while time.time() - t0 < secs:
    data = ser.read(4096)
    if data:
        text = data.decode("utf-8", errors="replace")
        buf.append(text)
        sys.stdout.write(text); sys.stdout.flush()
ser.close()

if out:
    with open(out, "w") as f:
        f.write("".join(buf))
    print(f"\n# 已存 {out}", flush=True)
