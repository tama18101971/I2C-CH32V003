# Changelog

All notable changes to this project will be documented in this file.

## [6.0.0] - 2026-08-17

### 🚀 Major Flash footprint reduction (~25%)
The core driver (`i2c.o`) was optimized for the RISC-V RV32EC architecture, cutting its
`.text` section from **2920 B to 2194 B** (−726 B). Measured firmware savings on the
size benchmark:

| Profile | Before | After | Saved |
|---|---:|---:|---:|
| Full | 2620 B | 2200 B | **−420 B** |
| No recovery | 2148 B | 1836 B | −312 B |
| No error counter | 2524 B | 2124 B | −400 B |
| No buffer API | 2584 B | 2168 B | −416 B |
| Lite (`I2C_LITE=1`) | 2116 B | 1808 B | −308 B |

### 🔧 Internal refactoring
- **Unified STAR1 wait automaton** — a single `i2c_wait_star1_flag()` helper now handles
  flag polling, `BERR`/`ARLO` detection, `AF` handling and timeout with recovery across
  `i2c_send_byte`, `i2c_wait_ack`, `i2c_wait_start_bit`, `i2c_send_addr` and the read helpers.
  This removed ~250–350 B of duplicated machine code and one central error path.
- **Optimized delays** — `i2c_usleep()` and `i2c_delay()` no longer use stack-backed
  `volatile` counters or a runtime 32-bit division. The frequency is read directly from the
  already-programmed `I2C1->CTLR2` register. `i2c_usleep` dropped from 116 B to 38 B.
- **Simplified clock configuration** — `i2c_configure_registers()` merges the checks and the
  `CCR` divisor branches into a single division path.
- **Refactored `i2c_read_buffer` / `i2c_write_buffer`** — removed heavy in-loop
  `if (i == len - 3)` checks and stack spills; `i2c_read_buffer` dropped from 580 B to 394 B.
- **Compact GPIO pin management** — a new `i2c_set_gpio_mode()` helper removes repeated
  `GPIOC->CFGLR` read-modify-write sequences from `i2c_init`, `i2c_deinit` and
  `i2c_bus_recovery`.
- **Optimized scanner** — `i2c_probe_address()` now reuses `i2c_send_addr` instead of
  duplicating the address state machine (206 B → 126 B).

### 🏗 Build / packaging
- Added a **unified root `platformio.ini`** to build any example or size benchmark directly
  from the repository root with a single `pio run` (envs: `scanner`, `benchmark_full`,
  `benchmark_no_recovery`, `benchmark_no_error_counter`, `benchmark_no_buffer`,
  `benchmark_lite`).
- Improved `library.json`: added `examples`, `headers` and `homepage` fields for correct
  PlatformIO Registry indexing.
- Examples now resolve the library via `lib_deps = symlink://../..` instead of `lib_extra_dirs`
  for robust, name-independent local builds.

### ✅ Behavior preserved
- 100% public API compatibility (`i2c.h` unchanged).
- Full fault tolerance: timeouts, `BERR`/`ARLO` filtering, and 16-pulse GPIO clock recovery
  remain intact.
- All modular configuration macros still work: `I2C_LITE`, `I2C_DISABLE_BUS_RECOVERY`,
  `I2C_DISABLE_SCANNER`, `I2C_DISABLE_BUFFER_API`, `I2C_DISABLE_ERROR_COUNTER`.

## [5.5.1] - 2026-08-17
- Made `I2C_DISABLE_*` feature flags independent and added the size benchmark example.
- Synchronized documentation.

## [5.5.0] - 2026-08-17
- Added aggressive Lite mode (`I2C_LITE=1`), used framework macros, bumped version.
- Introduced instant timeout recovery, safe `i2c_send_byte` API split and `uint16_t` buffer lengths.

## [5.4.3] - 2026-08-17
- Audit fixes and documentation sync.

## [5.4.1] - 2026-08-17
- Improved I2C error handling and clock validation; fixed 2-byte read race condition.
