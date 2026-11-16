"""Collect final build/test evidence; does not operate the board."""
import csv
import hashlib
import json
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent / "results"
size = Path.home() / ".platformio/packages/toolchain-riscv/bin/riscv-wch-elf-size.exe"
envs = ("scanner", "benchmark_full", "benchmark_no_recovery", "benchmark_no_error_counter",
        "benchmark_no_buffer", "benchmark_lite", "allapi_full", "allapi_no_buffer",
        "allapi_no_scanner", "allapi_lite", "nolto_full", "nolto_lite",
        "hardware_apds", "hardware_apds_polling")
sizes = []
for env in envs:
    build = root / ".pio/build" / env
    txt = subprocess.check_output([str(size), "-A", str(build / "firmware.elf")], text=True)
    sections = {m[1]: int(m[2]) for line in txt.splitlines()
                if (m := re.match(r"^(\.\S+)\s+(\d+)\s+", line))}
    flash = sum(sections.get(k, 0) for k in (".init", ".vector", ".text", ".fini", ".data", ".highcode"))
    ram = sum(sections.get(k, 0) for k in (".data", ".bss", ".stack"))
    assert flash == (build / "firmware.bin").stat().st_size, env
    sizes.append({"environment": env, "flash_bytes": flash, "RAM_data_bss_stack": ram})
with (out / "build-sizes.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=sizes[0].keys())
    w.writeheader()
    w.writerows(sizes)

host = (out / "host-tests.log").read_text(encoding="utf-8-sig")
host_counts = [int(x) for x in re.findall(r"=== (\d+) tests, 0 failures ===", host)]
assert len(host_counts) == 10 and sum(host_counts) == 396
hardware = [json.loads((out / (env + "-result.json")).read_text())
            for env in ("hardware_apds", "hardware_apds_polling")]
assert all(r["upload_exit"] == 0 and r["done_received"] and r["failures"] == 0 for r in hardware)
mutations = json.loads((out / "mutations.json").read_text())
assert len(mutations) == 5 and all(r["detected"] for r in mutations)
readback = json.loads((out / "flash-readback.json").read_text())
assert readback["flash_matches_current_polling_build"]
files = ["src/i2c.c", "src/i2c.h", "platformio.ini", "library.json",
         "test/i2c_mock.cpp", "test/test_i2c.cpp", "test/hardware/apds9960.c",
         "test/hardware/run_hil.py"]
manifest = {
    "library_version": "7.2.0",
    "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    "uncommitted_changes": True,
    "hardware": hardware,
    "hardware_checks_total": sum(r["checks"] for r in hardware),
    "host_counts": host_counts,
    "host_test_functions_executed": sum(host_counts),
    "mutations_detected": len(mutations),
    "builds": sizes,
    "flash_readback": readback,
    "source_sha256": {f: hashlib.sha256((root / f).read_bytes()).hexdigest() for f in files},
    "firmware_sha256": {env: hashlib.sha256((root / ".pio/build" / env / "firmware.bin").read_bytes()).hexdigest()
                        for env in ("hardware_apds", "hardware_apds_polling")},
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print(json.dumps({"host_executions": sum(host_counts), "hardware_checks": manifest["hardware_checks_total"],
                  "builds": sizes, "mutations_detected": len(mutations)}, indent=2))
