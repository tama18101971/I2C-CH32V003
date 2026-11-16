"""Upload one HIL environment and capture the real UART transcript (COM6).

Run with the PlatformIO Python: python test/hardware/run_hil.py --env hardware_apds
No synthetic PASS output is produced. A missing DONE, upload failure or nonzero
firmware failures is an error. UART is opened before reset/upload to retain boot.
"""
import argparse
import datetime
import json
import re
import subprocess
import sys
import time
from pathlib import Path
import serial

sys.stdout.reconfigure(errors="replace")

p = argparse.ArgumentParser()
p.add_argument("--env", default="hardware_apds")
p.add_argument("--port", default="COM6")
p.add_argument("--seconds", type=float, default=90)
args = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent / "results"
out.mkdir(exist_ok=True)
stamp = datetime.datetime.now(datetime.timezone.utc).isoformat()
uart = serial.Serial(args.port, 115200, timeout=0.1)
uart.reset_input_buffer()
with (out / (args.env + "-upload.log")).open("w", encoding="utf-8") as log:
    proc = subprocess.Popen([str(Path(sys.executable).with_name("pio.exe")), "run", "-e", args.env, "-t", "upload"],
                            cwd=root, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    data = bytearray()
    until = time.monotonic() + args.seconds
    while time.monotonic() < until:
        chunk = uart.read(4096)
        if chunk:
            data.extend(chunk)
            print(chunk.decode("utf-8", errors="replace"), end="", flush=True)
        if b"HIL DONE" in data and data.endswith(b"\n"):
            break
        if proc.poll() is not None and proc.returncode != 0:
            break
    uart.close()
    if proc.poll() is None:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=5)
text = data.decode("utf-8", errors="replace")
(out / (args.env + "-uart.log")).write_text(text, encoding="utf-8")
match = re.search(r"HIL DONE checks=(\d+) failures=(\d+) id=([0-9a-fA-F]+) irq=(\d+) max_ticks=(\d+)", text)
result = {"started_utc": stamp, "environment": args.env, "port": args.port,
          "upload_exit": proc.returncode, "done_received": bool(match)}
if match:
    result.update(dict(zip(("checks", "failures", "id", "irq", "max_ticks"),
                           (int(match[1]), int(match[2]), int(match[3], 16), int(match[4]), int(match[5])))))
    result["datasheet_identity_match"] = result["id"] == 0xAB
    result["transport_pass"] = result["failures"] == 0
(out / (args.env + "-result.json")).write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result))
sys.exit(0 if proc.returncode == 0 and match and int(match[2]) == 0 else 1)
