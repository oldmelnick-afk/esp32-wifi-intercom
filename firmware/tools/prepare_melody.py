"""Prepare a looping alarm for the five ESP32 intercoms.

Usage:
    python tools/prepare_melody.py                 # included short chime
    python tools/prepare_melody.py my_song.mp3     # requires ffmpeg
    python tools/prepare_melody.py my_song.wav     # WAV or ffmpeg

The output is data/alarm.raw: signed 16-bit mono PCM at 16 kHz.
"""

from __future__ import annotations

import argparse
import math
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "data" / "alarm.raw"
RATE = 16_000
MAX_SECONDS = 45  # LittleFS partition is about 1.56 MiB.


def default_chime() -> bytes:
    notes = [(523.25, .34), (659.25, .34), (783.99, .40),
             (1046.50, .60), (0, .20), (783.99, .34),
             (1046.50, .80), (0, .45)]
    samples = []
    for frequency, duration in notes:
        count = int(duration * RATE)
        for n in range(count):
            if not frequency:
                samples.append(0)
                continue
            envelope = min(1.0, n / (RATE * .012),
                           (count - 1 - n) / (RATE * .09))
            envelope = max(0.0, envelope)
            t = n / RATE
            value = (math.sin(2 * math.pi * frequency * t)
                     + .18 * math.sin(4 * math.pi * frequency * t))
            samples.append(int(max(-1, min(1, value * envelope * .48)) * 32767))
    return struct.pack("<" + "h" * len(samples), *samples)


def wav_bytes(path: Path) -> bytes:
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, RATE):
            raise ValueError("WAV должен быть PCM, 16 кГц, 16 бит, моно; "
                             "или установите ffmpeg для автоматического преобразования")
        return source.readframes(source.getnframes())


def converted_bytes(path: Path) -> bytes:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        result = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(path),
             "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-acodec", "pcm_s16le", "-"],
            check=True, capture_output=True,
        )
        return result.stdout
    if path.suffix.lower() == ".wav":
        return wav_bytes(path)
    raise ValueError("Для MP3/M4A/FLAC установите ffmpeg или сохраните WAV 16 кГц, 16 бит, моно")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path)
    args = parser.parse_args()
    try:
        if args.source:
            if not args.source.is_file():
                raise ValueError(f"Файл не найден: {args.source}")
            data = converted_bytes(args.source)
        else:
            data = default_chime()
        if not data or len(data) % 2:
            raise ValueError("Аудио пустое или имеет повреждённый формат")
        if len(data) > MAX_SECONDS * RATE * 2:
            raise ValueError(f"Мелодия длиннее {MAX_SECONDS} секунд; обрежьте исходный файл")
        DEST.parent.mkdir(parents=True, exist_ok=True)
        DEST.write_bytes(data)
        print(f"Готово: {DEST} ({len(data) / (RATE * 2):.1f} с)")
        return 0
    except (ValueError, subprocess.CalledProcessError, wave.Error) as error:
        print(f"Ошибка: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
