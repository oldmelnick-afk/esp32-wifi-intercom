"""Intercom gateway: makes the voice assistant look like room 6 to the boxes.

Boxes stream a question as VOICE packets addressed to room 6; when the stream
pauses, the gateway transcribes it, asks the assistant and streams the spoken
answer back to that box as ordinary VOICE packets addressed to its room.
"""
import os
import queue
import socket
import struct
import threading
import time
from pathlib import Path

import numpy as np

from assistant import CONFIG, Assistant

MAGIC = 0x354D4F48          # "HOM5", little-endian, same as the firmware
VERSION = 2
VOICE, HELLO, ASSIST = 1, 6, 7
ASSIST_DONE, ASSIST_CANCEL = 1, 2   # box: question over / user pressed a button
HEADER = struct.Struct("<IBBHII")   # magic, version, type, sequence, origin, value
FRAME_SAMPLES = 320                 # 20 ms at 16 kHz
NODE_ID = 0xA5515A06                # gateway origin id, never a real ESP32
ROOM = CONFIG["assistant_room"]
PORT = CONFIG["udp_port"]
END_OF_QUESTION_S = 0.6             # stream silent this long = question finished
                                    # (boxes since 2026-10-23 also say so at once)
MIN_QUESTION_S = 0.4

boxes = {}          # room -> (ip, last seen)
node_rooms = {}     # box origin id -> room
sessions = {}       # origin id -> {"frames": [...], "last": t, "ip": ip}
lock = threading.Lock()
jobs = queue.Queue()
answering = threading.Event()   # set while a question is being answered
cancelled = {}      # box ip -> time the user cancelled the answer with a button

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
sock.bind(("", PORT))


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def send(ip, kind, value, sequence=0, payload=b""):
    sock.sendto(HEADER.pack(MAGIC, VERSION, kind, sequence, NODE_ID, value) + payload, (ip, PORT))


def broadcast_address():
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.168.0.1", 80))
        ip = probe.getsockname()[0]
    finally:
        probe.close()
    return ip.rsplit(".", 1)[0] + ".255"


def hello_loop():
    target = broadcast_address()
    while True:
        send(target, HELLO, ROOM)
        with lock:
            known = [ip for ip, seen in boxes.values() if time.time() - seen < 15]
        for ip in known:          # unicast too: broadcasts get lost on Wi-Fi
            send(ip, HELLO, ROOM)
        time.sleep(2)


def receive_loop():
    while True:
        data, (ip, _) = sock.recvfrom(2048)
        if len(data) < HEADER.size:
            continue
        magic, version, kind, sequence, origin, value = HEADER.unpack_from(data)
        if magic != MAGIC or version != VERSION or origin == NODE_ID:
            continue
        now = time.time()
        if kind == HELLO and 1 <= value <= 5:
            with lock:
                if boxes.get(value, (None,))[0] != ip:
                    log(f"box room {value} at {ip}")
                boxes[value] = (ip, now)
                node_rooms[origin] = value
        elif kind == VOICE and value == ROOM and len(data) == HEADER.size + FRAME_SAMPLES * 2:
            with lock:
                session = sessions.setdefault(origin, {"frames": [], "ip": ip})
                session["frames"].append(data[HEADER.size:])
                session["last"] = now
        elif kind == ASSIST and len(data) == HEADER.size:
            with lock:
                session = sessions.pop(origin, None)
                if value == ASSIST_DONE and session:
                    jobs.put((origin, session["ip"], b"".join(session["frames"])))
                elif value == ASSIST_CANCEL:
                    if ip not in cancelled or now - cancelled[ip] > 1:
                        log(f"box {ip}: answer cancelled by a button")
                    cancelled[ip] = now


def watch_loop():
    """A question is complete once its box stops sending for a moment."""
    while True:
        time.sleep(0.05)
        now = time.time()
        with lock:
            done = [o for o, s in sessions.items() if now - s["last"] > END_OF_QUESTION_S]
            for origin in done:
                session = sessions.pop(origin)
                jobs.put((origin, session["ip"], b"".join(session["frames"])))


def reload_loop():
    """Exit when the code or settings change; start_gateway.bat starts the new
    version 10 s later. Lets updates be copied in without touching the server."""
    files = [Path(__file__).with_name(n) for n in ("gateway.py", "assistant.py", "config.json")]
    stamp = lambda: [f.stat().st_mtime for f in files]
    original = stamp()
    while True:
        time.sleep(5)
        try:
            if stamp() == original:
                continue
            time.sleep(5)            # let a copy finish
            with lock:
                busy = bool(sessions) or not jobs.empty() or answering.is_set()
            if not busy:
                log("files changed, restarting to load the new version")
                os._exit(0)
        except OSError:
            pass                     # a file is being replaced right now


def play(ip, room, phrases, on_first=None, asked=0.0):
    """Stream answer phrases to a box in real time, 20 ms per packet; stops if
    the user cancels on that box."""
    sequence = 0
    next_time = time.perf_counter()
    for pcm in phrases:
        if cancelled.get(ip, 0) > asked:
            return
        if on_first:
            on_first()
            on_first = None
            next_time = time.perf_counter()
        pcm = np.concatenate([pcm, np.zeros((-len(pcm)) % FRAME_SAMPLES, np.int16)])
        for start in range(0, len(pcm), FRAME_SAMPLES):
            sequence = (sequence + 1) & 0xFFFF
            if cancelled.get(ip, 0) > asked:
                return
            send(ip, VOICE, room, sequence, pcm[start:start + FRAME_SAMPLES].tobytes())
            next_time += FRAME_SAMPLES / 16000
            delay = next_time - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:               # synthesis of the next phrase made us late:
                next_time = time.perf_counter()   # restart the clock, do not burst


def work_loop(assistant):
    while True:
        job = jobs.get()
        answering.set()
        try:
            answer_job(assistant, *job)
        except Exception as error:      # one bad question must not stop the gateway
            log("error while answering:", repr(error))
        finally:
            answering.clear()


def answer_job(assistant, origin, ip, raw):
    pcm = np.frombuffer(raw, np.int16)
    seconds = len(pcm) / 16000
    with lock:
        room = node_rooms.get(origin)
    if room is None or seconds < MIN_QUESTION_S:
        log(f"skip {seconds:.1f} s from {ip} (room {room})")
        return
    started = time.time()
    question, text, phrases = assistant.handle_audio(pcm, room, ip)
    log(f"room {room}: «{question}» -> «{text}» ({assistant.timing})")
    if cancelled.get(ip, 0) > started:
        log("  not played: cancelled")
        return
    first = time.time()
    play(ip, room, phrases, asked=started, on_first=lambda: log(
        f"  first audio {time.time() - started:.1f} s after the question ended"))
    log(f"  answer played in {time.time() - first:.1f} s")


def main():
    assistant = Assistant()
    log("loading speech recognition...")
    from assistant import transcribe
    transcribe(np.zeros(16000, np.int16))      # warm up so the first question is fast
    from assistant import synthesize
    synthesize("Готово.")
    def weather_loop():   # keep the forecast fresh so weather answers are instant
        from assistant import get_weather
        while True:
            try:
                get_weather()
            except Exception as error:
                log("weather refresh failed:", error)
            time.sleep(540)

    def net_check():   # which web services this machine reaches; logged once at start
        hosts = ("api.open-meteo.com", "api.met.no", "wttr.in", "api.weather.yandex.ru",
                 "nominatim.openstreetmap.org", "router.project-osrm.org", "ya.ru")
        for host in hosts:
            try:
                addrs = sorted({a[4][0] for a in socket.getaddrinfo(host, 443, socket.AF_INET)})
            except OSError as error:
                log(f"net: {host}: {error}")
                continue
            for addr in addrs:
                started = time.time()
                try:
                    socket.create_connection((addr, 443), timeout=5).close()
                    result = f"connect {time.time() - started:.2f} s"
                except OSError as error:
                    result = f"connect failed: {error}"
                log(f"net: {host} = {addr}: {result}")

    for target in (hello_loop, receive_loop, watch_loop, weather_loop, reload_loop, net_check):
        threading.Thread(target=target, daemon=True).start()
    log(f"assistant is room {ROOM} on UDP {PORT}; waiting for boxes")
    work_loop(assistant)


if __name__ == "__main__":
    main()
