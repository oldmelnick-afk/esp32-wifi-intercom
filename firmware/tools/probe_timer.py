"""Briefly probe UDP control reception on one box, then cancel its timer.

Usage: py -3.11 tools/probe_timer.py 192.168.0.163 [hold-seconds]
"""

import socket
import struct
import sys
import time

destination = sys.argv[1]
hold = int(sys.argv[2]) if len(sys.argv) > 2 else 30
if not 1 <= hold <= 120:
    raise SystemExit("hold-seconds must be 1..120")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send(kind, value):
    for sequence in range(3):
        packet = struct.pack("<IBBHII", 0x354D4F48, 2, kind, sequence, 0xD1A60001, value)
        sock.sendto(packet, (destination, 42042))
        time.sleep(0.1)

try:
    send(3, 600)
    print(f"TIMER 10:00 sent to {destination}; watch box screen", flush=True)
    time.sleep(hold)
finally:
    send(4, 0)
    print("STOP sent; timer cancelled", flush=True)
