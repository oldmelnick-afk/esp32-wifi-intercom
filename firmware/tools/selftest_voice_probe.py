"""Exercise the real sender and packet probe over local UDP without hardware."""

from pathlib import Path
import re
import subprocess
import sys


tools = Path(__file__).resolve().parent
listener = subprocess.Popen(
    [sys.executable, str(tools / "check_voice_stream.py"), "D1A60001", "3"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
)
try:
    assert listener.stdout is not None
    ready = listener.stdout.readline()
    if "Listening for room node D1A60001" not in ready:
        raise RuntimeError(f"Probe did not start: {ready}")
    sender = subprocess.run(
        [sys.executable, str(tools / "send_test_chime.py"), "1", "1", "127.0.0.1"],
        capture_output=True,
        text=True,
        timeout=5,
        check=True,
    )
    output, _ = listener.communicate(timeout=6)
    rows = [row for row in output.splitlines() if "packets=" in row]
    packet_count = sum(int(value) for value in re.findall(r"packets=(\d+)", output))
    gap_count = sum(int(value) for value in re.findall(r"seq_gaps=(\d+)", output))
    reorder_count = sum(int(value) for value in re.findall(r"reorder=(\d+)", output))
    if listener.returncode != 0 or packet_count != 60 or gap_count or reorder_count:
        raise AssertionError(f"Unexpected UDP probe: {sender.stdout}\n{output}")
    print(ready.strip())
    print("\n".join(rows))
    print("PASS: 60/60 local UDP chime packets with no sequence gaps")
finally:
    if listener.poll() is None:
        listener.terminate()
        listener.wait(timeout=3)
