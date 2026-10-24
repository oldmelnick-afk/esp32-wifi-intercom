"""Send one USB command to a box and print what it answers.

Opening the port resets the ESP32, so the script first waits for boot.
Examples:
  py -3.11 tools/usb_cmd.py COM6 STATS
  py -3.11 tools/usb_cmd.py COM6 "TEST CHIME 2" --listen 8
"""
import argparse
import sys
import time

import serial

parser = argparse.ArgumentParser()
parser.add_argument("port")
parser.add_argument("command")
parser.add_argument("--boot", type=float, default=9.0, help="seconds to wait for boot and peer discovery")
parser.add_argument("--listen", type=float, default=3.0, help="seconds to read after the command")
args = parser.parse_args()

with serial.Serial(args.port, 115200, timeout=0.2) as port:
    output = b""
    deadline = time.time() + args.boot
    while time.time() < deadline:
        output += port.read(4096)
    port.write(args.command.encode() + b"\n")
    deadline = time.time() + args.listen
    while time.time() < deadline:
        output += port.read(4096)
sys.stdout.buffer.write(output.decode(errors="replace").encode("utf-8", errors="replace"))
