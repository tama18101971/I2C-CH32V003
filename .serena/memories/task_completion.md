# Task Completion Checklist

There is no lint/typecheck/test suite. "Done" for a code change means:

1. `pio run` — all 6 envs compile (or at minimum the envs affected by the change plus
   `benchmark_full` and `benchmark_lite`, since these cover both feature-macro extremes).
2. `pio run -e benchmark_full -t size` and same for `benchmark_lite -t size` — compare
   Flash against CHANGELOG budgets (2268 B Full / 1844 B Lite at 7.0.1). If size changed
   noticeably, record it in CHANGELOG.
3. If public API or config macros changed: update BOTH `README.md` and `README.ru.md`
   (mirrored), `src/i2c.h` doc comments, and `CHANGELOG.md`.
4. If a new build env/profile was added: update root `platformio.ini` `default_envs` AND
   the CI matrix in `.github/workflows/ci.yml`.
5. CI on push runs the same builds — treat green CI as the final gate.
