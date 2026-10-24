"""Read the intercom BLE status from a local Windows Bluetooth adapter."""

import asyncio
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / ".pio" / "bleak-probe"))

from bleak import BleakClient, BleakScanner


STATUS_UUID = "77a5d8a2-7bb2-4b6a-9bf9-3336a781db11"
COMMAND_UUID = "77a5d8a1-7bb2-4b6a-9bf9-3336a781db11"


async def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else "HOUSE-1F"
    devices = await BleakScanner.discover(timeout=8)
    found = [device for device in devices if (device.name or "").startswith(prefix)]
    print(f"BLE devices matching {prefix}: {len(found)}")
    for device in found:
        print(device.name, device.address)
    if len(found) != 1:
        return
    async with BleakClient(found[0], timeout=15) as client:
        print("connected:", client.is_connected)
        value = await client.read_gatt_char(STATUS_UUID)
        print("status:", repr(bytes(value)))
        if "--timer-test" in sys.argv:
            config = (Path(__file__).resolve().parents[1] / "include" / "config.h").read_text(
                encoding="utf-8"
            )
            match = re.search(r'^#define HOUSE_CONTROL_CODE "([^"]+)"', config, re.M)
            if not match:
                raise RuntimeError("Local BLE control code was not found")
            prefix = match.group(1)
            try:
                await client.write_gatt_char(
                    COMMAND_UUID, f"{prefix} TIMER 30".encode(), response=True
                )
                await asyncio.sleep(0.5)
                value = await client.read_gatt_char(STATUS_UUID)
                print("timer status:", repr(bytes(value)))
            finally:
                await client.write_gatt_char(
                    COMMAND_UUID, f"{prefix} STOP".encode(), response=True
                )
                await asyncio.sleep(0.5)
                value = await client.read_gatt_char(STATUS_UUID)
                print("stop status:", repr(bytes(value)))


if __name__ == "__main__":
    asyncio.run(main())
