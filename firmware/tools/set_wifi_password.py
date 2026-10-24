"""Store the 2.4 GHz Wi-Fi password on one ESP32 over its local USB port.

Usage: py -3.11 tools/set_wifi_password.py COM6
The password is read without echo and is never printed or written to a file.
"""

import argparse
from getpass import getpass
import time

import serial


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB serial port, for example COM6")
    args = parser.parse_args()

    password = getpass("Пароль сети YOUR_WIFI_SSID (ввод не отображается): ")
    try:
        encoded = password.encode("ascii")
    except UnicodeEncodeError as exc:
        raise SystemExit("Нужен пароль из печатных символов ASCII.") from exc
    if not 8 <= len(encoded) <= 63 or not all(32 <= ch <= 126 for ch in encoded):
        raise SystemExit("Нужен пароль из 8–63 печатных символов ASCII.")

    with serial.Serial(args.port, 115200, timeout=0.3) as esp:
        # Opening a CP210x serial port resets many ESP32 development boards.
        time.sleep(3)
        esp.reset_input_buffer()
        esp.write(b"WIFI " + encoded + b"\n")
        esp.flush()
        print("Пароль передан по USB. Ждём подтверждения и адреса Wi-Fi...")
        deadline = time.monotonic() + 22
        while time.monotonic() < deadline:
            raw = esp.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                print(line)
            if "UDP ready" in line or "Wi-Fi unavailable" in line or line.startswith("ERROR:"):
                break


if __name__ == "__main__":
    main()
