"""Copy the saved Windows Wi-Fi password to one ESP32 without printing it.

Usage: py -3.11 tools/provision_from_saved_wifi.py COM6
The SSID is the project's fixed network, YOUR_WIFI_SSID. The password stays in memory
and is sent only over the local USB serial link to the attached station.
"""

import argparse
import ctypes
import subprocess
import time

import serial


def saved_password() -> bytes:
    result = subprocess.run(
        ["netsh", "wlan", "show", "profile", "name=YOUR_WIFI_SSID", "key=clear"],
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise SystemExit("Не удалось прочитать сохранённый профиль YOUR_WIFI_SSID.")

    encoding = f"cp{ctypes.windll.kernel32.GetOEMCP()}"
    output = result.stdout.decode(encoding, errors="replace")
    for line in output.splitlines():
        label, separator, value = line.partition(":")
        if separator and label.strip().casefold() in ("key content", "содержимое ключа"):
            try:
                password = value.strip().encode("ascii")
            except UnicodeEncodeError as exc:
                raise SystemExit("Пароль должен содержать только печатные ASCII-символы.") from exc
            if 8 <= len(password) <= 63 and all(32 <= c <= 126 for c in password):
                return password
            raise SystemExit("Сохранённый пароль не подходит для команды ESP32.")
    raise SystemExit("В профиле YOUR_WIFI_SSID нет сохранённого пароля.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB serial port, for example COM6")
    args = parser.parse_args()
    password = saved_password()

    with serial.Serial(args.port, 115200, timeout=0.3) as esp:
        time.sleep(3)  # opening the CP210x port can reset the ESP32
        esp.reset_input_buffer()
        esp.write(b"WIFI " + password + b"\n")
        esp.flush()
        print("Пароль передан из Windows по USB; ждём подключения...", flush=True)
        deadline = time.monotonic() + 22
        while time.monotonic() < deadline:
            line = esp.readline().decode("utf-8", errors="replace").strip()
            if line.startswith("WIFI SAVED") or (line.startswith("Wi-Fi ") and "UDP ready" in line):
                print(line, flush=True)
            if line.startswith("Wi-Fi ") and "UDP ready" in line:
                return
            if line.startswith("ERROR:") or "Wi-Fi unavailable" in line:
                raise SystemExit("ESP32 не подключилась; проверьте сеть и повторите настройку.")
    raise SystemExit("ESP32 не подтвердила подключение за 22 секунды.")


if __name__ == "__main__":
    main()
