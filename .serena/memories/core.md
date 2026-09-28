# I2C-CH32V003 — Project Core

Fault-tolerant bare-metal I2C1 driver library for CH32V003 (WCH RISC-V, RV32EC).
Distributed as a PlatformIO library (`library.json`, srcDir `src/`), versioned in
`src/i2c.h` + `library.json` + `CHANGELOG.md` (keep all three in sync on version bumps).

## Source map
- `src/i2c.h` — public API + config macros (`I2C_LITE`, `I2C_DISABLE_*`, `I2C_TIMEOUT_MS`,
  `I2C_LEGACY_STATUS`, `I2C_INTER_FRAME_DELAY_US`). All conditional compilation is driven
  from here.
- `src/i2c.c` — single-file implementation (~670 lines): all functions are `i2c_*`-prefixed;
  internal static helpers (`i2c_handle_error`, `i2c_bus_recovery`, `i2c_configure_registers`,
  `i2c_wait_*`...). Comments in Russian, identifiers in English.
- `examples/i2c_scanner/` — bus scanner demo (own `platformio.ini`, uses `symlink://../..`
  as lib_deps).
- `examples/size_benchmark/` — minimal firmware for Flash/RAM footprint measurement.
- `platformio.ini` (repo root) — unified config building every example/benchmark env from
  the root. `default_envs` = scanner + 5 benchmark profiles.
- `.github/workflows/ci.yml` — CI builds all 6 envs, parses Flash/RAM usage, publishes
  size reports. Any new env in root `platformio.ini` should also be added to the CI matrix
  and to `default_envs`.
- `README.md` (EN) / `README.ru.md` (RU) — kept in parallel; content changes must be
  mirrored in both.

## Invariants
- Driver targets ONLY I2C1 on fixed pins PC1 (SDA) / PC2 (SCL); no pin remapping.
- No infinite `while` polling of hardware flags anywhere — every wait is timeout-bounded
  (`I2C_TIMEOUT_MS` scaled by `SystemCoreClock` at `i2c_init()` into `i2c_timeout_loops`).
- Feature removal is compile-time via `I2C_DISABLE_*` macros; `I2C_LITE=1` sets all of them.
- Footprint budgets tracked in CHANGELOG (2268 B Full / 1844 B Lite at 7.0.1) — size
  regressions matter; benchmark envs exist to catch them.
- Read APIs guarantee `ACK=1` restoration on all failure paths; `i2c_send_byte()` requires
  a follow-up `i2c_wait_ack()` (use inline `i2c_write_byte()` for atomicity).

See `mem:tech_stack`, `mem:conventions`, `mem:suggested_commands`, `mem:task_completion`.
