"""Run the firmware's ten-second microphone diagnostic on a USB serial port.

Start this script, wait for READY FOR TEST, then press Enter to begin speaking.
Usage: py -3.11 tools/test_mic_serial.py COM6
"""

import sys
import time

import serial


with serial.Serial(sys.argv[1], 115200, timeout=0.5) as port:
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line.startswith(("Node ", "Wi-Fi ", "Ready:")):
            print(line, flush=True)
        if line.startswith("Ready:"):
            break
    else:
        raise SystemExit("Board did not become ready in 35 seconds")

    print("READY FOR TEST: press Enter to send TEST MIC", flush=True)
    input()
    port.write(b"TEST MIC\n")
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line.startswith(("TEST MIC", "MIC ", "BUTTONS ", "Guru Meditation", "ERROR:")):
            print(line, flush=True)
