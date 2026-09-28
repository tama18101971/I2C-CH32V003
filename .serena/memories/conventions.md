# Code Conventions

- Naming: `i2c_` prefix for every public function and static helper; static internal
  helpers also `i2c_` (except `handle_critical_error`). Macros `I2C_*` uppercase.
- Types: buffer lengths and counters use `uint16_t` deliberately (RV32EC register usage
  minimization); register values `uint16_t`; clock/speed `uint32_t`.
- Return codes: `I2C_OK`(0), `I2C_NACK`(1), `I2C_ERR_TIMEOUT`(2), `I2C_ERR_CLK`(3),
  `I2C_ERR_BERR`(4), `I2C_ERR_ARLO`(5). Never return raw STAR1 contents.
- Conditional compilation: every feature-guarded block uses
  `#if !defined(I2C_DISABLE_...)` in the .c and matching `#ifndef I2C_DISABLE_...` in the
  header. New toggles must be (a) defined in `i2c.h` config section, (b) folded into
  `I2C_LITE` if aggressive-strippable, (c) documented in both READMEs.
- Hardware access: direct register access via `I2C1->...`, `GPIOC->...`; no WCH EVT
  library calls (driver is a full EVT replacement). Flag clearing by reading
  STAR1 then STAR2.
- Comments/docs: Russian in code comments and README.ru.md; English identifiers and
  README.md. Doc comments use Doxygen `@brief`/`@param`/`@warning` style.
- Error handling: on timeout/BERR/ARLO call the single `i2c_handle_error`/
  `i2c_handle_timeout` path so recovery + counter logic stays centralized.
- Version bumps: bump `library.json`, `src/i2c.h` header comment (both say "Version X.Y"),
  CHANGELOG entry, and ideally both README version lines — together.
- Git: no commit-format hooks in-repo; conventional but plain messages used in history.
