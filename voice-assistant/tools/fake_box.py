"""Pretends to be an intercom box: asks the gateway a spoken question and
records the answer, to test the assistant without hardware.

    py -3.11 tools/fake_box.py "Какое сегодня число?"
"""
import socket
import struct
import sys
import time
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from assistant import synthesize  # noqa: E402

HEADER = struct.Struct("<IBBHII")
MAGIC, VOICE, HELLO = 0x354D4F48, 1, 6
NODE, ROOM, GATEWAY = 0xFA4EB0C5, 3, ("127.0.0.1", 42042)

question = sys.argv[1] if len(sys.argv) > 1 else "Какое сегодня число?"
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.2", 42042))  # gateway replies to port 42042, like a box
sock.settimeout(0.2)

def send(kind, value, seq=0, payload=b""):
    sock.sendto(HEADER.pack(MAGIC, 2, kind, seq, NODE, value) + payload, GATEWAY)

send(HELLO, ROOM)       # introduce ourselves as room 3
time.sleep(0.3)
pcm = synthesize(question)
pcm = np.concatenate([pcm, np.zeros((-len(pcm)) % 320, np.int16)])
for i, start in enumerate(range(0, len(pcm), 320)):
    send(VOICE, 6, i + 1, pcm[start:start + 320].tobytes())
    time.sleep(0.02)
sent_at = time.time()
print(f"question sent ({len(pcm) / 16000:.1f} s): {question}", flush=True)

frames, first = [], None
deadline = time.time() + 40
while time.time() < deadline:
    try:
        data, _ = sock.recvfrom(2048)
    except socket.timeout:
        if frames and time.time() - last > 1.5:
            break
        continue
    magic, _, kind, _, _, value = HEADER.unpack_from(data)
    if kind == VOICE and value == ROOM:
        if first is None:
            first = time.time()
        frames.append(data[HEADER.size:])
        last = time.time()

if not frames:
    sys.exit("no answer")
audio = b"".join(frames)
print(f"first answer audio after {first - sent_at:.1f} s; answer {len(audio) / 32000:.1f} s long")
with wave.open("fake_box_answer.wav", "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(audio)
