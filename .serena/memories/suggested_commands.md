# Suggested Commands (Windows / PowerShell)

All commands run from repo root with PlatformIO (`pio`) on PATH.

- Build everything: `pio run` (uses `default_envs` = scanner + 5 benchmark profiles).
- Build one env: `pio run -e scanner` (also: benchmark_full, benchmark_no_recovery,
  benchmark_no_error_counter, benchmark_no_buffer, benchmark_lite).
- Size report for an env: `pio run -e benchmark_full -t size`.
- Upload/monitor (scanner demo): `pio run -e scanner -t upload` then
  `pio device monitor -e scanner` (115200 baud).
- Benchmark from its own directory instead of root:
  `pio run` in `examples/size_benchmark/` (envs: full, no_recovery, no_error_counter,
  no_buffer, lite).

Windows notes (PowerShell):
- Use `Get-ChildItem` / `Select-String` instead of `ls`/`grep` when shelling out; Serena
  tool calls handle search natively.
- Paths use backslashes; quote anything containing spaces.
- `.pio/` build output lives at repo root: `.pio/build/<env>/firmware.elf`.
