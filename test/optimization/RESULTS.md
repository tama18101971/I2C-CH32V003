# Оптимизация Flash в v7.2.1

17 ноября 2026, Europe/Volgograd. Рабочая ветка `codex/size-optimization`, база `21bece91079e84ff3fa159578b7a67a0a2e48e36` (v7.2.0). Исходные результаты предыдущего релиза сохранены в [прежнем отчёте](../hardware/RESULTS.md).

**Flash уменьшился во всех 14 профилях, RAM не выросла. 396 host-тестов и 101 346 assertions на реальной плате прошли без ошибок.** Число assertions не означает число независимых транзакций или осциллограмм.

## Что изменено

- Проверяемые ожидания STAR1 и BUSY/MSL/STOP используют общий цикл `i2c_wait_status()`: аппаратная ошибка по-прежнему обрабатывается до ready в том же snapshot, затем проверяется тот же счётчик таймаута. `flag=0` означает idle. Raw cleanup остаётся отдельным и не вызывает повторную обработку ошибок.
- Общий хвост TX/RX подтверждает idle и выдерживает прежний tBUF guard.
- 16-битный адрес отправляется через существующий 8-битный адресный префикс, затем младший байт. Порядок START, адреса, ACK и байтов не изменён.
- tBUF использует настроенный бит CKCFGR.FS и явное умножение на константы 2/5. Количество NOP остаётся `(CTLR2.FREQ + 1) * (Fast-mode ? 2 : 5)`.
- Публичный API не изменён; в `i2c.h` при подготовке релиза обновлена только строка версии. Сохранены все API, конфигурационные макросы, PE guards, приоритет ошибок, snapshot, recovery, ограничения частот, ACK/POS и сохранение IRQ. Новых глобальных переменных и динамической памяти нет.

Проверены варианты встраивания и более широкого объединения raw/checked wait. Принудительный noinline увеличивал отдельные сборки; объединение raw/checked давало небольшой выигрыш в одном профиле, но увеличивало HIL и no-LTO. Эти варианты не включены. Сводка измеренных кандидатов: [trials.json](results/trials.json); названия обозначают локальные эксперименты, а не новые режимы API.

## Размеры

PlatformIO Core 6.2.0, GCC 12.2.0, `genericCH32V003F4P6`, закреплённые platform/SDK из корневого `platformio.ini`. База v7.2.0 повторно собрана во всех 14 профилях тем же окружением; размеры совпали с опубликованными. Измерение — сумма секций ELF, сверенная с `.bin`. RAM включает `.data`, `.bss` и резерв стека 256 Б; peak stack не измерялся.

| Профиль | v7.2.0, Б | После, Б | Экономия | RAM, Б |
|---|---:|---:|---:|---:|
| scanner | 8416 | 8324 | 92 | 468 |
| benchmark_full | 2536 | 2432 | 104 (4,10%) | 296 |
| benchmark_no_recovery | 2076 | 1976 | 100 | 292 |
| benchmark_no_error_counter | 2464 | 2364 | 100 | 296 |
| benchmark_no_buffer | 2536 | 2432 | 104 | 296 |
| benchmark_lite | 2076 | 1976 | 100 (4,82%) | 292 |
| allapi_full | 2976 | 2852 | 124 (4,17%) | 296 |
| allapi_no_buffer | 2684 | 2584 | 100 | 296 |
| allapi_no_scanner | 2928 | 2804 | 124 | 296 |
| allapi_lite | 2156 | 2088 | 68 | 292 |
| nolto_full | 2800 | 2672 | 128 | 296 |
| nolto_lite | 2200 | 2152 | 48 | 292 |
| hardware_apds | 12220 | 12108 | 112 | 508 |
| hardware_apds_polling | 11708 | 11588 | 120 | 508 |

[CSV](results/sizes.csv), [оптимизированные сборки](results/build-root.log), [no-LTO](results/build-nolto.log), [повторная сборка базы](results/baseline-root.log), [база no-LTO](results/baseline-nolto.log).

## Проверки

- [Host](results/host-tests.log): 46/46/46/46/45/43/31/43/25/25 = 396 выполнений, 0 ошибок, `-Wall -Wextra -Werror`.
- [Мутации](results/mutations.json): тесты обнаружили все пять намеренных дефектов — ready раньше ошибки, потерю STOP error в probe, округление CCR вниз, отсутствие POS, включение исходно выключенных IRQ. Каждый вариант компилировался; обнаружение означает ненулевой результат тестов. Основной исходник не изменялся при проверке мутаций.
- [Target C/CSR](results/target-compile.log): 16 конфигураций, O0/Og/Os/O2 × default/atomic/fixed48/Lite, с `-Wall -Wextra -Werror`.
- Две реальные прошивки на CH32V003F4P6, WCH-LinkE v2.12, COM6; PC1 SDA / PC2 SCL, APDS-совместимый клон `0x39`, ID `0x9E`.
- Самостоятельные примеры: [scanner](results/standalone-scanner.log) и все пять [benchmark-профилей](results/standalone-benchmark.log) успешно собраны. [Пакет](results/package.log) установлен в независимый проект со всем API: 2852 Б Flash, 296 Б RAM. Локальные Markdown-ссылки в окончательном пакете проверены.

| HIL-профиль | Проверок | Ошибок | Длинных ISR | Журналы |
|---|---:|---:|---:|---|
| atomic | 76 788 | 0 | 4 840 | [UART](results/hardware_apds-uart.log), [JSON](results/hardware_apds-result.json), [upload](results/hardware_apds-upload.log) |
| polling | 24 558 | 0 | 0 | [UART](results/hardware_apds_polling-uart.log), [JSON](results/hardware_apds_polling-result.json), [upload](results/hardware_apds_polling-upload.log) |

Повторены 15 сочетаний CPU 8/24/48 МГц и запросов I2C 10/50/100/200/400 кГц, чтения 1/2/3/4/5/8/16, запись/обратное чтение, NACK, сохранение IRQ, PE-off/reinit и настоящий BTF timeout/recovery. Настройки ENABLE и ALS thresholds восстановлены и проверены чтением. Оба upload содержат `Verified OK`.

Независимый `wlink dump` прочитал 16 КБ Flash: первые 11 588 Б побайтно совпали с оптимизированной polling-прошивкой — [журнал](results/flash-readback.log), [SHA256 и результат](results/flash-readback.json). Бинарный dump хранится локально и исключён из Git/пакета. После проверки на плате находится завершившаяся polling HIL-прошивка.

Добавлен `--output-dir` для сохранения повторного HIL отдельно от исходных результатов:

```powershell
& "$HOME/.platformio/penv/Scripts/python.exe" test/hardware/run_hil.py --env hardware_apds --seconds 120 --output-dir test/optimization/results
& "$HOME/.platformio/penv/Scripts/python.exe" test/hardware/run_hil.py --env hardware_apds_polling --seconds 120 --output-dir test/optimization/results
```

Команды прошивают плату и временно меняют указанные настройки датчика. Хеши исходников (локальные байты и LF-нормализация), прошивок, размеры и результаты собраны в [manifest.json](results/manifest.json).

## Границы результата

Объединённый polling добавляет выбор условия внутри цикла. Таймауты сохраняют прежнее число итераций и остаются приблизительными; равенство wall-clock latency/throughput не утверждается. Различие количества ISR между прогонами само по себе не является измерением производительности.

Осциллограф/логический анализатор, физическое удержание SDA/SCL, второй master, EEPROM с 16-битной адресацией, stack watermark и 24-часовой soak не проверялись. Ограничения исходного аппаратного отчёта сохраняются. Remote CI проверяется отдельно для релизного коммита и тега v7.2.1; прежний CI v7.2.0 относится к исходному коммиту.

При подготовке релиза исходные результаты и manifest сохранены как исторические свидетельства испытаний до релизного коммита. После HIL в `i2c.c`/`i2c.h` обновлены только комментарии с версией 7.2.1; повторная сборка сверяется с сохранёнными хешами обеих испытанных прошивок.
