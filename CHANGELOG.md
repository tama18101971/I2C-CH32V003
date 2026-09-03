# Changelog

All notable changes to this project will be documented in this file.

## [7.1.0] - 2026-09-03

### 🛡️ Robustness & Safety
- **Single bus recovery per fault** — the flag-wait state machine now processes
  a hardware error *before* issuing `i2c_stop()`. Previously a `BERR`/`ARLO` was
  observed twice (once inside `i2c_stop()` → `i2c_wait_busy_clear()`, once in the
  caller), double-counting `consecutive_errors` and doubling worst-case fault
  latency to `2 × I2C_TIMEOUT_MS`. Timeout paths now program `STOP` directly
  without a second full-length `BUSY` wait. Worst-case fault latency is halved.
- **Optional critical sections (`I2C_ATOMIC_CRITICAL`, default off)** — the
  hardware requires the `ADDR`-clear → `STOP`/`ACK=0` sequences (RM events
  EV6_3) to complete before the current byte finishes on the wire. With
  `-DI2C_ATOMIC_CRITICAL=1` those windows run with interrupts disabled,
  preventing lost/duplicated bytes when an ISR is longer than one byte time
  (≈90 µs at 100 kHz, ≈23 µs at 400 kHz). Cost when enabled: 16 B (profile B, LTO).
- **Clock-scaled clock-stretching timeout** — new `I2C_STRETCH_TIMEOUT_US`
  (default 1000 µs) is scaled by `PCLK1` in `i2c_init()`. The previous fixed
  loop counter (`I2C_STRETCH_TIMEOUT=1000`) produced ≈135 µs at 48 MHz versus
  ≈3.2 ms at 2 MHz, so slow slaves could be overridden by recovery clocks at
  high core frequencies.
- **Recovery fails safe** — if register re-configuration inside
  `i2c_bus_recovery()` returns an error (core clock changed at runtime), the
  peripheral is left disabled instead of being enabled with invalid
  `CTLR2`/`CKCFGR` (`FREQ = 0` is not a valid value).
- **`i2c_deinit()` waits for a free bus** before dropping `PE`, so a slave is
  not left mid-transaction when the peripheral is switched off.
- **7-bit address masking** — `i2c_send_addr()` now applies `addr & 0x7F`,
  mirroring the existing `direction & 1` masking. Previously `addr >= 0x80`
  silently corrupted the transmitted byte.

### 🎁 New API
- **`i2c_get_last_star1()`** — returns the `STAR1` snapshot captured at the
  moment the last error was detected, *before* the driver clears
  `AF`/`BERR`/`ARLO`. Reading `I2C1->STAR1` after a failed call yields a clean
  register; the snapshot preserves the real cause. `i2c_probe_address()` now
  reports this snapshot through `p_star1` as well (previously it reported a
  register that had already been cleared, making the diagnostics unusable).
  Compiled out by `-DI2C_DISABLE_LAST_ERROR` / `I2C_LITE=1`.

### 🐛 Fixed
- **Unreachable bus speed is an error** — if the computed `CCR` divisor does not
  fit into 12 bits, `i2c_init()` returns `I2C_ERR_CLK` instead of silently
  clamping and running the bus at a different frequency than requested.
- **`i2c_deinit()` no longer disables `PE` mid-transaction** — it waits for the
  bus to become free first (bounded by the regular timeout and recovery).
- **`stddef.h` is included by `i2c.h`** — the header documents `NULL` for the
  optional `i2c_probe_address()` outputs but did not actually provide it;
  compiling `i2c_probe_address(0x50, NULL, NULL)` in a TU that includes only
  `i2c.h` failed. Found by the new host test suite.
- **`reg &= ~BIT` narrowing warnings** — status-register bit clears use explicit
  `uint16_t` casts; the driver now compiles warning-free under
  `-Wall -Wextra`.

### ⚡ Performance & Size
- **LTO enabled in all build environments** (`board_build.use_lto = yes`) —
  measured −212 B (Full) to −672 B (full-API build). On the `ch32v` platform
  LTO only works via `board_build.use_lto`; `-flto` in `build_flags` does not
  reach the linker.
- **Single STAR1 read per wait iteration** — `i2c_wait_star1_flag()` previously
  performed two MMIO reads per poll loop iteration; now one (−16 B, ~⅓ less
  APB traffic in the hottest loop).
- **Static driver state moved to `.bss`** — `i2c_speed` / `i2c_timeout_loops`
  no longer carry `.data` initializers (−12 B).
- **Deduplicated 16-bit address phase** — `i2c_write_buffer16()` and
  `i2c_read_buffer16()` share `i2c_start_reg16_write()`.
- **`I2C_FIXED_PCLK_HZ`** — new opt-in macro compiles the driver against a
  compile-time `PCLK1`: all divisors fold to constants and the
  `SystemCoreClock` range check is skipped (−56 B; `I2C_ERR_CLK` for the clock
  itself is then not returned).
- **`I2C_ERR_CLK` instead of silent clamp** for unreachable speeds (see above).

### 🏗️ CI & Quality
- **Host unit tests** — 36 state-machine tests executed against a software
  model of the peripheral and slave (`test/`), 10 build configurations, no
  hardware required: `pwsh test/run_tests.ps1`. The model reacts to individual
  register accesses, so it enforces real hardware semantics (`ADDR` cleared by
  `STAR1`+`STAR2` reads, `RXNE`/`BTF` by `DATAR` reads, `ACK`/`NACK` sampled
  from `CTLR1` when each byte arrives). Verified: canonical read sequences for
  `len` = 1/2/3/8 per RM0008 §26.3.3, error/timeout/recovery paths, `ACK=1` /
  `POS=0` invariants on all failure paths, critical-section depth, `len == 0`
  semantics.
- **CI memory arithmetic fixed** — Flash/RAM are now computed by summing ELF
  sections instead of GNU `size` columns. The previous fallback under-counted
  Flash by 32 B (the `ALLOC`-only `.vector` section) and RAM by 256 B (the
  `.stack` section), reporting 36 B of RAM where 292 B are actually used. A
  consistency check now asserts the section sum equals the `firmware.bin` size.
- **New size profiles** — `allapi_full` / `allapi_no_buffer` / `allapi_no_scanner`
  / `allapi_lite` link *every* public function, so `--gc-sections` cannot hide
  their cost. The old benchmark referenced only the register API, which made
  `I2C_DISABLE_BUFFER_API` look free even though it saves 336 B in an
  application that actually uses the buffer API. A `nolto_full` / `nolto_lite`
  reference pair documents the LTO delta.
- **Unit-test job added to CI** — the full 10-configuration test matrix runs on
  every push/PR alongside the build matrix.

### 📚 Documentation
- READMEs document the usage contracts explicitly: single execution context
  (not ISR-safe), interrupt-length caveat with `I2C_ATOMIC_CRITICAL`, default
  I2C1 pin mapping without AFIO remap, no NULL-checking of buffers, `len == 0`
  semantics, and `i2c_init()` call order.
- New build-configuration table covering all `I2C_*` macros, including the new
  `I2C_STRETCH_TIMEOUT_US`, `I2C_MAX_ERROR_COUNT`, `I2C_ATOMIC_CRITICAL`,
  `I2C_FIXED_PCLK_HZ` and `I2C_DISABLE_LAST_ERROR`.
- `I2C_TIMEOUT` and `I2C_STRETCH_TIMEOUT` marked deprecated (unused since 7.0.0;
  removal scheduled for 8.0.0).
- `examples/size_benchmark/README.md` rewritten for the two-profile methodology,
  including the correct way to sum ELF sections.

### 📦 Footprint (genericCH32V003F4P6, LTO enabled)

| Profile | 7.0.1 | 7.1.0 | Delta |
|---|---:|---:|---:|
| Full, register API (profile A) | 2268 B | 2172 B | **−96 B** |
| Lite, register API (profile A) | 1844 B | 1704 B | −140 B |
| Full, all API referenced (profile B) | 3144 B¹ | 2596 B | −548 B |
| Lite, all API referenced (profile B) | 1876 B¹ | 1724 B | −152 B |

¹ measured with the new `allapi` methodology; there was no equivalent profile in 7.0.1.
RAM is unchanged at 292–296 B. The new features (error snapshot, clock-scaled
stretch timeout, `deinit` bus wait, critical-section plumbing) add ~116 B without
LTO; LTO more than compensates.

## [7.0.1] - 2026-08-26

### 🐛 Fixed
- **Empty-length reads** — `i2c_read_buffer()`, `i2c_read_raw()` and `i2c_read_buffer16()`
  with `len == 0` now return `I2C_OK` immediately without touching the bus.
  Previously such a call emitted an unspecific START→STOP sequence without addressing
  any slave (v6 behavior for `i2c_read_buffer` was "no bus activity at all").
  The internal RX engine now documents its `len >= 1` precondition explicitly.
- **Legacy status switch documented** — the `I2C_LEGACY_STATUS` opt-out is now declared
  and explained in `i2c.h` configuration section (defaults to `0`; define
  `-DI2C_LEGACY_STATUS=1` to restore v6 behavior where all errors collapse into
  a single `I2C_NACK`). The runtime check was simplified accordingly.

### 📚 Documentation
- Restored the complete DAC7571 examples in both READMEs: write protocol description,
  step-by-step low-level variant (`dac7571_set_voltage_ll`) and `dac7571_power_down()`.
  Added a note that the low-level path is the only option when building in Lite mode
  (`I2C_DISABLE_BUFFER_API`), since raw/buffer APIs are compiled out there.

Flash/RAM footprints are unchanged: 2268 B Full / 1844 B Lite on `genericCH32V003F4P6`.

## [7.0.0] - 2026-08-26

### 🛡️ Robustness & Safety
- **C++ Support** — wrapped `i2c.h` in `extern "C"` guards for clean integration into C++ / Arduino projects.
- **Direction Bit Masking** — `i2c_send_addr()` now enforces `(direction & 1)` to prevent corrupting the 7-bit device address.
- **Read-Only Workload Error Reset** — `consecutive_errors` counter is now safely cleared upon successful `ADDR` phase ACK in `i2c_send_addr()`, preventing sporadic errors across long read sessions from triggering unintended bus recovery.
- **Dynamic Clock-Scaled Timeout** — replaced static loop counter with `I2C_TIMEOUT_MS` (default 40 ms) dynamically scaled by `SystemCoreClock` during `i2c_init()`.
- **Clock & Speed Validation** — `i2c_init()` now rejects invalid speeds `> 400000 Hz` returning `I2C_ERR_CLK`.

### ⚡ Performance
- **Zero-Delay `i2c_stop()`** — removed mandatory 50 µs inter-frame delay from `i2c_stop()`; moved delay exclusively to `i2c_probe_address()` where inter-probe spacing is needed. This doubles transaction throughput for high-frequency streaming (such as DAC7571 wave generation).

### 🎁 New API
- **Raw Transfer API** (`i2c_write_raw()`, `i2c_read_raw()`) — direct multi-byte read and write operations without register address preamble for register-less chips (DAC7571, streaming ADCs/DACs).
- **16-Bit Register Buffer API** (`i2c_write_buffer16()`, `i2c_read_buffer16()`) — multi-byte read/write support with 16-bit big-endian register/word addresses for EEPROMs (24LC32..24LC1025) and advanced sensors.
- **Granular Return Codes** — distinct return codes `I2C_ERR_TIMEOUT` (2), `I2C_ERR_BERR` (4), `I2C_ERR_ARLO` (5) for better runtime fault diagnostics. Legacy behavior (all errors mapped to `I2C_NACK`) can be selected via `-DI2C_LEGACY_STATUS=1`.

### 🏗️ CI & Quality
- Added **GitHub Actions CI** (`.github/workflows/ci.yml`) to automatically compile and verify all 6 build configurations on every push and PR.

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
