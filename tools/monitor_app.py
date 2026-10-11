#!/usr/bin/env python3
"""修正版串口监视器 —— 抓 ESP8266 应用输出。

用法: monitor_app.py [port] [baud] [seconds] [outfile] [--no-reset]

【为什么必须自己写】
pyserial 在 Linux 上打开串口时会默认 assert DTR/RTS。在 ESP8266 的自动
复位电路上, 这会把芯片按在复位里、或直接拉进下载模式, 于是你只能读到
零星 NUL 字节 —— 昨天的所有串口抓取脚本都栽在这里。

本脚本在打开串口后立刻 setDTR(False) / setRTS(False), 让芯片处于正常运行
状态, 再用一次 RTS 脉冲复位, 从而抓到完整的应用日志。
"""
import sys
import time

import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
port = args[0] if len(args) > 0 else "/dev/ttyUSB0"
baud = int(args[1]) if len(args) > 1 else 115200
secs = float(args[2]) if len(args) > 2 else 20.0
out = args[3] if len(args) > 3 else None
do_reset = "--no-reset" not in sys.argv

s = serial.Serial(port, baud, timeout=0.2)
s.setDTR(False)          # ★ 关键: 不 assert, 否则芯片进下载模式
s.setRTS(False)
time.sleep(0.3)
s.reset_input_buffer()

if do_reset:
    s.setRTS(True)       # EN=LOW, 芯片复位
    time.sleep(0.1)
    s.setRTS(False)      # EN=HIGH, 松复位 -> 正常启动

t0 = time.time()
buf = bytearray()
while time.time() < t0 + secs:
    d = s.read(4096)
    if d:
        buf.extend(d)
s.close()

if out:
    with open(out, "wb") as f:
        f.write(buf)

sys.stdout.write(buf.decode("utf-8", "replace"))
sys.stdout.write(f"\n--- {len(buf)} bytes captured in {secs}s ---\n")
