"""Measure live VOICE packets from one room without playing or saving audio.

Usage: py -3.11 tools/check_voice_stream.py 59513E38
Press a talk button and speak while this script listens (default 20 seconds).
"""

import math
import socket
import struct
import sys
import time

origin = int(sys.argv[1], 16)
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 20
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", 42042))
sock.settimeout(0.5)

print(f"Listening for room node {origin:08X} on UDP 42042 ({duration} seconds)", flush=True)
started = time.monotonic()
bucket_start = started
count = 0
sum_squares = 0
peak = 0
gaps = 0
reordered = 0
max_gap_ms = 0
previous_sequence = None
previous_time = None
target = None
source_ip = None
while time.monotonic() - started < duration:
    try:
        data, address = sock.recvfrom(1000)
    except socket.timeout:
        data = b""
    if len(data) == 656:
        magic, version, kind, sequence, sender, recipient = struct.unpack_from("<IBBHII", data)
        if magic == 0x354D4F48 and version == 2 and kind == 1 and sender == origin:
            arrived = time.monotonic()
            if previous_time is not None and arrived - previous_time <= 0.4:
                delta = (sequence - previous_sequence) & 0xFFFF
                if delta == 0 or delta > 100:
                    reordered += 1
                elif delta > 1:
                    gaps += delta - 1
                max_gap_ms = max(max_gap_ms, round((arrived - previous_time) * 1000))
            previous_sequence = sequence
            previous_time = arrived
            samples = struct.unpack_from("<320h", data, 16)
            count += 1
            sum_squares += sum(sample * sample for sample in samples)
            peak = max(peak, max(abs(sample) for sample in samples))
            target = recipient
            source_ip = address[0]
    now = time.monotonic()
    if now - bucket_start >= 1:
        rms = math.isqrt(sum_squares // (count * 320)) if count else 0
        print(
            f"{int(now-started):02d}s packets={count:02d} source={source_ip} "
            f"target={target} rms={rms} peak={peak} seq_gaps={gaps} "
            f"reorder={reordered} max_gap_ms={max_gap_ms}",
            flush=True,
        )
        bucket_start = now
        count = 0
        sum_squares = 0
        peak = 0
        gaps = 0
        reordered = 0
        max_gap_ms = 0
        target = None
        source_ip = None
