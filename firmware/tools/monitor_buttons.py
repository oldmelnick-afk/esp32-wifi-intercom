"""Show ESP32 button states and reboot errors from its USB serial port.

Usage: py -3.11 tools/monitor_buttons.py COM6
The port may reset the ESP32 when opened. Stop with Ctrl+C.
"""

import sys

import serial


if len(sys.argv) != 2:
    raise SystemExit("Usage: py -3.11 tools/monitor_buttons.py COM6")

prefixes = (
    "BUTTONS ",
    "CHIME ",
    "Node ",
    "Wi-Fi ",
    "Ready:",
    "Guru Meditation",
    "Debug exception",
    "***ERROR***",
    "ERROR:",
    "Rebooting",
    "rst:",
)

try:
    with serial.Serial(sys.argv[1], 115200, timeout=1) as port:
        print(f"Listening on {sys.argv[1]}; 1 means pressed. Stop with Ctrl+C.", flush=True)
        while True:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line.startswith(prefixes) or "endPacket()" in line:
                print(line, flush=True)
except KeyboardInterrupt:
    print("Stopped.")
