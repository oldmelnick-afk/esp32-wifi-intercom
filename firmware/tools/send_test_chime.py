"""Send the firmware's three-note test melody to one room over house Wi-Fi.

Usage: py -3.11 tools/send_test_chime.py 1 [repetitions] [destination-ip]
This sends only temporary UDP VOICE frames; it does not change a box's firmware.
"""

import math
import socket
import struct
import sys
import time

room = int(sys.argv[1])
repetitions = int(sys.argv[2]) if len(sys.argv) > 2 else 3
destination = sys.argv[3] if len(sys.argv) > 3 else "192.168.0.255"
if not 0 <= room <= 5 or not 1 <= repetitions <= 10:
    raise SystemExit("room must be 0..5; repetitions must be 1..10")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

sequence = 0
notes = (523, 659, 784)
for repeat in range(repetitions):
    start = time.monotonic()
    for frame in range(60):
        pcm = []
        for sample in range(320):
            offset = frame * 320 + sample
            note_index = offset // 6400
            pos = offset % 6400
            amplitude = 11000 if pos < 4800 else 0
            pcm.append(round(amplitude * math.sin(2 * math.pi * notes[note_index] * offset / 16000)))
        header = struct.pack("<IBBHII", 0x354D4F48, 2, 1, sequence, 0xD1A60001, room)
        sock.sendto(header + struct.pack("<320h", *pcm), (destination, 42042))
        sequence = (sequence + 1) & 0xFFFF
        time.sleep(max(0, start + (frame + 1) * 0.02 - time.monotonic()))
    print(f"sent melody {repeat + 1}/{repetitions} to room {room} via {destination}", flush=True)
    time.sleep(0.5)
