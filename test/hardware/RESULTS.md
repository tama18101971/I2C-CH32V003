# Исправления 7.2.0 и испытания на реальной плате

16 ноября 2026. База изменений: `b43bebf73e4bbd6288c9ce84d1fbc3c0f4bf4bb2`.
Испытания выполнены до релизного коммита 7.2.0; исходные хеши, параметры и результаты сохранены в manifest. При подготовке публикации platform/SDK закреплены на уже использованных ревизиях; код драйвера после испытаний не менялся. `source_sha256` описывает байты локальных файлов Windows на момент испытаний; нормализация CRLF/LF в Git может менять хеш исходника. Binary hashes относятся к прошитым файлам `.bin`.

**Исправленный драйвер реально прошит и испытан на CH32V003F4P6 через WCH-LinkE и COM6. Два финальных прогона: 101 346 программных проверок, 0 ошибок.** Это число assertions прошивки, а не число независимых транзакций или измеренных осциллограмм.

## Оборудование и идентификация

- Программатор: WCH-LinkE, RV firmware 2.12; COM6 — его UART-интерфейс, 115200 baud.
- MCU определён программатором как CH32V003F4P6, ChipID `0x00300500`, 16 КБ Flash, QingKe V2A.
- Устройство I2C: адрес `0x39`, подключение драйвера PC1 SDA / PC2 SCL. UART TX платы — PD5.
- Регистр ID `0x92` стабильно вернул `0x9E` при одиночном и пакетном чтении. Пользователь подтвердил, что это китайский клон. ID оригинала по [datasheet APDS-9960](https://docs.broadcom.com/doc/AV02-4191EN) — `0xAB`; подлинность оригинального Broadcom APDS-9960 этими тестами не утверждается.
- Напряжение, номиналы подтяжек, ёмкость шины и аналоговые фронты физически не измерялись.

Тест меняет только ENABLE `0x80` и четыре ALS threshold bytes `0x84…0x87`. Перед записью сохранены их исходные значения; engines выключены. В конце обоих финальных прогонов исходные настройки записаны обратно и проверены чтением. Никакие неизвестные регистры не записываются, FIFO не вычитывается.

## Какие находки исправлены

| Находка исходного аудита | Изменение | Проверка |
|---|---|---|
| I2C-001 | Error flags раньше ready в одном STAR1 snapshot | Host ready+error, проверка немедленного low-level return; мутация порядка обнаруживается |
| I2C-002/003 | Probe возвращает ошибки START и STOP | Host START/STOP timeout; реальные NACK и следующая транзакция |
| I2C-004/005 | CCR ceiling; PCLK/HCLK 8…48 МГц; проверки compile-time макросов | Host grid, target compile; реальные 15 clock/rate конфигураций |
| I2C-006 | Save/restore mstatus mask 0x88 в коротких RX critical sections | Host четыре исходные маски; на MCU IRQ остаются выключенными после read1/read2; long timer ISR load |
| I2C-007 | RX ожидает BUSY/MSL/STOP clear перед OK | Host failed STOP для 1/2/3/4; реальные postconditions проверяются после каждого buffer read |
| I2C-008 | Checked SCL waits, максимум9 pulses, физическая проверка перед SWRST, проверка idle после него | Host SCL/SDA held; настоящий BTF timeout/recovery на MCU. Физический short на плате не вводился |
| I2C-009 | Модель POS/ARLO/STOP/IRQ и RX-stage injections; max-length guards | 396 host executions; 5 возвратных мутаций обнаружены |
| I2C-010 | ARLO не вызывает STOP/GPIO recovery; single-master contract | Host ownership; второй физический master не подключался |
| I2C-011/012 | GPIO latch preload до GPIO OD; init проверяет линии до SWRST | Host; реальный обычный init/reinit во всех rate cases |
| I2C-013 | Макро-диапазоны проверяются; время честно определено как приблизительный per-wait budget | Target negative compile tests; точный wall-clock deadline не добавлен |
| I2C-014 | Консервативный NOP guard после подтверждённого STOP | Код и реальный back-to-back traffic; tBUF осциллографом не измерялся |
| I2C-015 | Standalone benchmark выбирает только main.c | Все5 отдельных env теперь SUCCESS |

Дополнительно реальное fault-injection испытание показало: при намеренном PE=0 флаги могут давать прежнему low-level API ложный OK. Добавлены PE guards в START/Repeated START/address/data entry points. Возвращается `I2C_ERR_RECOVERY`, затем требуется успешный init. Нормальные алгоритмы ACK/POS для N=2 сохранены, а не заменены последовательностью другого MCU.

Новый статус `I2C_ERR_RECOVERY=6` сообщает о неудачном восстановлении/выключенном PE. В legacy он сворачивается в NACK. При failed recovery PE выключен, GPIO OD отпущены; после устранения причины необходимо повторить init. Первичный STAR1 snapshot сохраняется при неудачном cleanup. Если cleanup также истёк, возвращается timeout/recovery failure, а не ложный успех.

## Финальные аппаратные результаты

| Конфигурация | Проверок | Ошибок | Длинных TIM2 ISR | Raw UART / JSON |
|---|---:|---:|---:|---|
| `hardware_apds`, ATOMIC_CRITICAL=1 | **76 788** | **0** | **4 266** | [UART](results/hardware_apds-uart.log), [JSON](results/hardware_apds-result.json) |
| `hardware_apds_polling`, default ATOMIC_CRITICAL=0 | **24 558** | **0** | 0 (IRQ load выключена) | [UART](results/hardware_apds_polling-uart.log), [JSON](results/hardware_apds_polling-result.json) |

Оба upload журнала содержат `Programming Finished` и `Verified OK`: [atomic upload](results/hardware_apds-upload.log), [polling upload](results/hardware_apds_polling-upload.log). Финальная Flash дополнительно считана обратно независимым `wlink dump` и побайтно совпала с текущим `hardware_apds_polling/firmware.bin`, 11 708 bytes: [readback verification](results/flash-readback.json).

На плате сейчас находится **завершившаяся polling HIL-прошивка**, ожидающая в idle loop. Датчик восстановлен; прежней прикладной прошивки нет — пользователь сообщил о предварительном стирании MCU. Новая library сама по себе не заменяет пользовательское приложение.

### Матрица clock/rate

В каждом из двух прогонов проверены CPU/HCLK **8, 24, 48 МГц** и requested I2C **10, 50, 100, 200, 400 кГц**, всего15 сочетаний. По32 раунда traffic на сочетание, затем дополнительные1024 раунда с ISR при48М/400к в atomic-профиле.

CCR в реальном UART соответствует расчёту:

| HCLK MHz | 10k | 50k | 100k | 200k (Fm) | 400k (Fm) |
|---:|---:|---:|---:|---:|---:|
| 8 | 400 | 80 | 40 | 14 | 7 |
| 24 | 1200 | 240 | 120 | 40 | 20 |
| 48 | 2400 | 480 | 240 | 80 | 40 |

Это **заданные режимы и прочитанные регистры**, а не измерение SCL. Например8М/400к с CCR7 имеет расчётную частоту около380.95кГц;8М/200к с CCR14 —190.48кГц. Значения не округляются в отчёте до «измеренных400к».

### Реально выполненные сценарии

- Одиночное ID-чтение и packet reads через reg90 для проверки согласованности чтения1/2/3/4.
- Запись четырёх изменяющихся pattern bytes в thresholds и обратное чтение1/2/3/4; дополнительно чтение5/8/16 с проверкой известных threshold bytes и buffer guards.
- Short writes1/2/3, raw write5, raw pointer positioning и raw read4, обычные register read/write. `read_buffer` использует Repeated START.
- После каждого buffer read проверяются software-visible BUSY=0, MSL=0, STOP=0, POS=0, ACK=1.
- Address NACK на отсутствующем0x38 и немедленное успешное ID-чтение0x39 после него; zero-length buffer read с NULL.
- На atomic-прошивке read1/read2 при уже запрещённых caller IRQ и подтверждение неизменной маски. Затем TIM2 ISR с2000 NOP на каждом вызове во время1024 раундов I2C.
- Намеренное выключение PE: low-level address API возвращает ERR_RECOVERY, reinit и следующий ID-read успешны.
- Настоящий software timeout BTF на аппаратном I2C: START+AW без записи data, затем `i2c_wait_ack()`. BTF не появляется, возвращён `I2C_ERR_TIMEOUT=2`, GPIO/controller recovery выполнен, следующая транзакция успешна. Это **не** short-circuit и **не** искусственный NACK данных от slave.
- Восстановление ENABLE и всех четырёх thresholds с проверкой readback.

Счётчик `max_ticks` в UART — сырой SysTick counter delta отдельных чтений; clock менялся между этапами. Его нельзя интерпретировать как единый измеренный максимальный timeout всей библиотеки.

### Первоначальные отрицательные результаты сохранены

1. Первое подключение MCU не удалось; после сообщения пользователя об очистке MCU соединение и прошивка заработали. [Первый connect/backup attempt](results/backup.log).
2. Первичная проверка ожидала оригинальный ID0xAB и отказала на0x9E. Последующие single/block reads подтвердили стабильный0x9E; пользователь подтвердил clone. [Первоначальная идентификация](results/identity-initial-uart.log). Идентификация оригинального чипа не объявлена успешной задним числом.
3. Первый transport run прервался на кодировке Windows serial logger; её обработка исправлена. MCU завершил тот прогон, его RAM counters сохранены в `interrupted-run-ram.bin`.
4. Первоначальная гипотеза «PE-off обязательно вызовет timeout» не подтвердилась: low-level address вернул0. Этот отрицательный прогон сохранён: [UART](results/pe-off-injection-initial-uart.log), [JSON](results/pe-off-injection-initial-result.json). После добавления PE guard отдельно проверены PE-off rejection и действительный BTF timeout. Финальные нулевые результаты относятся к исправленному коду.

## Host, compiler и сборки

- [Host log](results/host-tests.log):46/46/46/46/45/43/31/43/25/25 тестовых функций, сумма396, failures0. Runner самостоятельно добавляет MinGW bin в PATH и использует `-Wall -Wextra -Werror`.
- Буферы/ACK проверены до65535 bytes на host, с одним дополнительным byte у slave и guards. На MCU такие RAM buffers не выделялись.
- [Mutation log](results/mutations.json): возвращены ready-before-error, ignored probe STOP, floor CCR, missing POS, unconditional IRQ enable — все5 вариаций вызывают ошибки тестов. Production source при mutation-проверке не менялся.
- Target C/CSR compilation: O0/Og/Os/O2 и fixed48М — успешно, [diagnostics](results/gcc-final.log). Fixed2М, timeout0, stretch0 и error-threshold256 отвергнуты на compile time: [negative config log](results/gcc-invalid-configs.log).
- [Cppcheck](results/cppcheck-final.log): только style о локальном scope; функциональных diagnostics не выдано. Это не формальное доказательство hardware correctness.
- [Корневая сборка](results/build-root.log): **14/14 SUCCESS**; [standalone benchmark](results/build-standalone-benchmark.log): **5/5 SUCCESS**; [standalone scanner](results/build-standalone-scanner.log): SUCCESS. Это локальные журналы испытаний; актуальный remote CI относится к релизному коммиту и проверяется отдельно в GitHub Actions.

### Flash / RAM

[CSV](results/build-sizes.csv), суммы ELF совпали с `.bin` для всех14 корневых env. RAM включает256-byte stack reserve, а не измеренный peak stack.

| Env | Flash B | RAM B |
|---|---:|---:|
| scanner | 8416 | 468 |
| benchmark_full | 2536 | 296 |
| benchmark_no_recovery | 2076 | 292 |
| benchmark_no_error_counter | 2464 | 296 |
| benchmark_no_buffer | 2536 | 296 |
| benchmark_lite | 2076 | 292 |
| allapi_full | 2976 | 296 |
| allapi_no_buffer | 2684 | 296 |
| allapi_no_scanner | 2928 | 296 |
| allapi_lite | 2156 | 292 |
| nolto_full | 2800 | 296 |
| nolto_lite | 2200 | 292 |
| hardware_apds | 12220 | 508 |
| hardware_apds_polling | 11708 | 508 |

Цена исправлений относительно7.1.0: register full +364B, all API full +380B, register Lite +372B; статическая RAM соответствующих библиотечных профилей не выросла. Большие host model buffers не входят в MCU driver. Размеры — конкретные LTO-приложения, не верхняя граница любого пользовательского firmware.

## Что этими испытаниями не доказано

Не подключались логический анализатор/осциллограф и второй master. Не измерялись SCL, tBUF, rise/fall/setup/hold и аналоговый запас; отсутствие лишнего физического byte/clock не квалифицировано осциллограммой. Не вводились внешние held SDA/SCL, BERR/ARLO/OVR аппаратным fault injector или data NACK у программируемого slave. Эти реакции покрыты host-моделью в указанном объёме.

APDS-клон не поддерживает настоящий16-bit register address API; его аппаратная проверка требует EEPROM/другого target. Чтения255/65535 проверены на host, не на этой плате. 24-часовой soak и stack-watermark не выполнялись. Проверенные короткие нагрузочные прогоны не заменяют квалификацию длительной непрерывной эксплуатации.

Точные source/binary hashes, база git, результаты и размеры: [manifest.json](results/manifest.json). Сценарий повторного сбора доказательств: `python test/hardware/summarize_results.py`; прошивка/запуск платы: `python test/hardware/run_hil.py --env hardware_apds` из PlatformIO Python. Повторный HIL меняет и затем восстанавливает указанные настройки датчика.
