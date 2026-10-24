"""Ask an attached ESP32 to send a UDP packet to its own listening socket."""

import sys
import time

import serial

port_name = sys.argv[1] if len(sys.argv) > 1 else "COM6"
with serial.Serial(port_name, 115200, timeout=0.5) as port:
    started = time.monotonic()
    sent = False
    while time.monotonic() - started < 16:
        if not sent and time.monotonic() - started > 6:
            port.write(b"TEST UDP\n")
            sent = True
        line = port.readline()
        if line:
            print(line.decode("ascii", errors="backslashreplace").strip(), flush=True)
