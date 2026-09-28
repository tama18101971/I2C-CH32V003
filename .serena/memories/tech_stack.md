# Tech Stack

- Language: C (C99-ish), `extern "C"` guarded header for C++/Arduino consumers.
- Target: WCH CH32V003 (RISC-V RV32EC), board `genericCH32V003F4P6`, 2 KB RAM / 16 KB Flash.
- Framework: `noneos-sdk` (bare-metal WCH SDK, `ch32v00x.h` registers; NOT Arduino).
- PlatformIO platform: `https://github.com/Community-PIO-CH32V/platform-ch32v.git`
  (Community-PIO-CH32V). Toolchain: riscv-none-embed-gcc via PlatformIO.
- Build system: PlatformIO only. Two config layers:
  - root `platformio.ini` — all example/benchmark envs, `lib_extra_dirs = .` so the repo
    root acts as the library source;
  - per-example `platformio.ini` with `lib_deps = symlink://../..`.
- No host-side tests, no linter/formatter configs in-repo. Validation = compilation of the
  6 PlatformIO envs (see `mem:task_completion`).
- CI: GitHub Actions on ubuntu, Python 3.11 + pip-installed PlatformIO, per-env build +
  Flash/RAM size parsing (`.github/workflows/ci.yml`).
- Version pins: none (no package manager dependencies beyond PlatformIO itself).
