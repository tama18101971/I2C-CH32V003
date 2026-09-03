# I2C size benchmarks

Два профиля измерения Flash для одной и той же библиотеки:

| main-файл | Что измеряет |
|---|---|
| `src/main.c` | типовое сенсорное приложение: `i2c_init()`, `i2c_write_register()`, `i2c_read_register()`, `i2c_deinit()` |
| `src/main_allapi.c` | ссылается на **каждую** публичную функцию, поэтому `--gc-sections` не может выбросить ни одну из них |

Оба профиля намеренно не содержат `printf`, инициализации UART и сканирования шины.
Транзакции не выполняются: обращения к API закрыты `volatile`-гейтом, равным нулю,
поэтому устройство на шине для сборки и прошивки не требуется, но линкер обязан
сохранить весь задействованный код.

Зачем два профиля: при ссылке только на регистровое API линкер выбрасывает
приёмный движок (`i2c_read_bytes_rx`), buffer/raw/buffer16-функции и сканер.
Из-за этого выгода от `I2C_DISABLE_BUFFER_API` выглядит равной нулю, хотя в
приложении, реально использующем буферное API, она составляет несколько сотен байт.

## Запуск

Все окружения объявлены в корневом `platformio.ini`; запускать из корня репозитория:

```sh
# типовое приложение
pio run -e benchmark_full -e benchmark_no_recovery -e benchmark_no_error_counter \
        -e benchmark_no_buffer -e benchmark_lite

# все публичные функции задействованы
pio run -e allapi_full -e allapi_no_buffer -e allapi_no_scanner -e allapi_lite

# референс без LTO для сравнения
pio run -e nolto_full -e nolto_lite
```

Профили:

- `benchmark_full` / `allapi_full` — все возможности включены;
- `benchmark_no_recovery` — `I2C_DISABLE_BUS_RECOVERY`;
- `benchmark_no_error_counter` — `I2C_DISABLE_ERROR_COUNTER` (восстановление остаётся);
- `benchmark_no_buffer` / `allapi_no_buffer` — `I2C_DISABLE_BUFFER_API`;
- `allapi_no_scanner` — `I2C_DISABLE_SCANNER`;
- `benchmark_lite` / `allapi_lite` — `I2C_LITE=1`;
- `nolto_full` / `nolto_lite` — те же сборки с `board_build.use_lto = no`.

## Как считать размер

Отчёт PlatformIO (`Flash: ... used`) корректен. При ручном подсчёте суммируйте
секции, а не колонки `text`/`data` из GNU `size`: секция `.vector` имеет атрибут
`ALLOC` без `LOAD` (в `text` не попадает, но в `firmware.bin` присутствует), а
`.stack` размещена в RAM и не входит в `bss`.

```sh
riscv-none-elf-size -A .pio/build/benchmark_full/firmware.elf
# Flash = .init + .vector + .text + .fini + .data (+ .highcode)
# RAM   = .data + .bss + .stack
```

Сумма секций Flash обязана совпадать с размером `firmware.bin`; в CI это
проверяется автоматически.
