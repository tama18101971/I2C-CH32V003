# Надёжный драйвер шины I2C (I2C1) для CH32V003 (RISC-V) — Версия 7.2.0

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

[🇬🇧 English](README.md)

Драйвер аппаратного I2C1 для **CH32V003** с ограниченными polling-ожиданиями, диагностикой ошибок и восстановлением шины с одним мастером. Использует регистры и заголовки WCH NoneOS SDK. Проверенные сценарии и ограничения приведены ниже и в отчёте аппаратных испытаний.

---

## Что изменилось в 7.2.0

- Аппаратные ошибки имеют приоритет над ready; probe передаёт ошибки START и STOP.
- Чтение дожидается завершения STOP; короткие критические секции восстанавливают исходную маску IRQ.
- CCR округляется вверх, номинальная частота не превышает запрос. Поддерживаемый PCLK/HCLK консервативно ограничен 8–48 МГц.
- Recovery проверяет SCL/SDA, выдаёт максимум девять импульсов и выполняет SWRST только после физического освобождения линий. При отказе возвращается `I2C_ERR_RECOVERY` (6), PE выключен до успешного init.
- ARLO возвращается без STOP и GPIO recovery. Автоматическое восстановление поддерживается только на шине с одним мастером.
- После успешного STOP добавлен консервативный NOP-интервал. Таймауты остаются приблизительными бюджетами отдельных ожиданий, а не всей транзакции.
- Модель проверяет ready+error, POS, ошибки RX, незавершённый STOP и маску IRQ. Отдельный benchmark выбирает ровно один main-файл.
- Реальные результаты CH32V003F4P6 + совместимого клона APDS (ID 0x9E) и ограничения приведены в [отчёте испытаний](test/hardware/RESULTS.md).

## Архитектурные особенности

1. **Совместимость с C++:** Заголовочный файл `i2c.h` обёрнут в `extern "C"` блоки для линковки с C++. Интеграция требует совместимых заголовков и окружения WCH NoneOS SDK; наличие `extern "C"` само по себе не обеспечивает поддержку любого Arduino core.
2. **Подтверждённый STOP:** write и read ожидают idle; затем выполняется консервативная пауза tBUF. Время всей транзакции не ограничивается одним polling budget.
3. **Ограниченные polling wait:** число итераций масштабируется по HCLK 8–48 МГц; реальное время зависит от сборки и IRQ. Это не точный wall-clock deadline.
4. **Защита в read-only сценариях:** Счётчик критических ошибок `consecutive_errors` сбрасывается при успешном ACK на этапе адресации `ADDR`. Это предотвращает накопление спорадических редких ошибок в ходе длительных сессий чтения.
5. **Raw API и 16-битные адреса регистров:** Функции `i2c_write_raw()` / `i2c_read_raw()` для чипов без внутренней адресации регистров (ЦАП DAC7571) и `i2c_write_buffer16()` / `i2c_read_buffer16()` для памяти с 16-битным адресом слова (например, 24LC64; выбор банка выполняет вызывающий код).
6. **Детализированные коды возврата:** Явные коды ошибок (`I2C_ERR_TIMEOUT`, `I2C_ERR_BERR`, `I2C_ERR_ARLO`, `I2C_ERR_CLK`, `I2C_ERR_RECOVERY`, `I2C_NACK`, `I2C_OK`). Для совместимости со старым кодом доступен флаг `-DI2C_LEGACY_STATUS=1`.
7. **Валидация скорости шины:** Функция `i2c_init()` проверяет скорость (до 400 кГц) и возвращает `I2C_ERR_CLK` при некорректных параметрах.
8. **Изоляция и инкапсуляция:** Функция восстановления шины `i2c_bus_recovery` объявлена `static` и изолирована внутри `i2c.c`.
9. **Мгновенное восстановление:** При застревании шины в `BUSY` или на этапе `START`/`ADDR` драйвер немедленно запускает процедуру восстановления шины.
10. **Разделение передачи и ACK:** `i2c_write_byte()` объединяет `i2c_send_byte()` и `i2c_wait_ack()`; сама функция не запрещает прерывания и не обеспечивает реентерабельность.
11. **Состояние RX после ошибки:** ACK/POS восстанавливаются в receive cleanup; при неудачном recovery PE выключен и нужен успешный init.
12. **Длины буферов:** API принимает `uint16_t` (0–65535); буфер предоставляет вызывающий код, выделения памяти внутри драйвера нет.

---

## Контракты использования

Только шина с одним мастером. PCLK/HCLK 8–48 МГц; после смены частоты повторите init. При `I2C_ERR_RECOVERY` PE выключен, GPIO отпущены. Любой ненулевой return делает результат чтения недостоверным, даже если часть буфера уже заполнена. В legacy recovery failures также сворачиваются в NACK.


Это жёсткие требования, а не рекомендации. Они же продублированы в шапке `i2c.h`.

1. **Один контекст исполнения.** Драйвер хранит состояние в статических переменных и монопольно владеет блоком I2C1. Функции **не реентерабельны** и **не предназначены для вызова из ISR** во время другой активной транзакции. Синхронизация, если она нужна, — задача вызывающего кода.
2. **Прерывания во время чтения.** Последовательности «сброс ADDR → STOP / ACK=0» обязаны завершиться до конца передачи текущего байта. По умолчанию они не защищены — это безопасно для чистого polling-режима. Если в системе есть прерывания длиннее одной байтовой посылки (≈90 мкс при 100 кГц, ≈23 мкс при 400 кГц), соберите драйвер с `-DI2C_ATOMIC_CRITICAL=1`.
3. **Пины.** Предполагается штатная раскладка I2C1 (`AFIO_PCFR1.I2C1_RM[22,1] = 00`, SDA = PC1, SCL = PC2). Драйвер не читает и не пишет регистры ремапа AFIO; при включённом ремапе он настроит не те пины.
4. **Указатели не проверяются на NULL** (кроме явно опциональных выходов `i2c_probe_address`). Передача невалидного буфера при `len > 0` — неопределённое поведение.
5. **Порядок вызовов.** Перед операциями на шине нужен успешный `i2c_init()` — в нём вычисляются внутренние таймауты и задержки.
6. **Семантика `len == 0`.** Чтение (`i2c_read_buffer` / `i2c_read_raw` / `i2c_read_buffer16`) возвращает `I2C_OK`, не задействуя шину. Запись выполняет полную транзакцию `START` + адрес устройства `[+ адрес регистра]` + `STOP` — штатный способ спозиционировать указатель регистра или проверить наличие устройства.

---

## Физический уровень и подключение

Драйвер работает с аппаратным блоком **I2C1** на штатных выводах контроллера:

* **PC1 — SDA** (Режим: Alternate Function Open-Drain, 30MHz)
* **PC2 — SCL** (Режим: Alternate Function Open-Drain, 30MHz)

> **Подтяжки:** нужны внешние pull-up к допустимому для всех устройств напряжению. Номинал выбирают по ёмкости шины, допустимому току и времени нарастания; 2.2–4.7 кОм не гарантируют Fast-mode на любой нагрузке. AF_OD не включает внутренний pull-up.

---

## Коды возврата

| Код | Значение | Описание |
|---|:---:|---|
| `I2C_OK` | `0` | Успешно / Подтверждено (ACK) |
| `I2C_NACK` | `1` | Не подтверждено (NACK от ведомого) или общая ошибка |
| `I2C_ERR_TIMEOUT` | `2` | Программный таймаут ожидания шины или аппаратного флага |
| `I2C_ERR_CLK` | `3` | Некорректная частота (PCLK1 вне 8..48 МГц, speed > 400 кГц, speed == 0 или скорость недостижима) |
| `I2C_ERR_BERR` | `4` | Аппаратная ошибка шины (Bus Error — некорректный START/STOP) |
| `I2C_ERR_ARLO` | `5` | Потеря арбитража (Arbitration Lost в мультимастерной сети) |
| `I2C_ERR_RECOVERY` | `6` | Восстановление не удалось или PE выключен; после устранения причины нужен успешный init |

*Все ненулевые коды корректно отрабатывают при проверке `if (err != I2C_OK)`.* Для сворачивания `TIMEOUT`, `BERR`, `ARLO` и `RECOVERY` в `I2C_NACK` (`ERR_CLK` остаётся отдельным) задайте флаг компиляции `-DI2C_LEGACY_STATUS=1`.

Для точной аппаратной причины сбоя используйте `i2c_get_last_star1()`: она возвращает снимок `STAR1`, сделанный *до* очистки драйвером флагов ошибок. Прямое чтение `I2C1->STAR1` после сбойного вызова даёт чистый регистр, так как `AF`/`BERR`/`ARLO` уже сняты.

```c
uint8_t value = 0;
uint8_t res = i2c_read_register(0x39, 0x92, &value);
if (res != I2C_OK) {
    uint16_t star1 = i2c_get_last_star1();
    if (star1 & I2C_STAR1_AF)   { /* ведомый не подтвердил */ }
    if (star1 & I2C_STAR1_BERR) { /* некорректный START/STOP */ }
    if (star1 & I2C_STAR1_ARLO) { /* потеря арбитража */ }
}
```

---

## Конфигурация на этапе компиляции

| Макрос | По умолчанию | Действие |
|---|:---:|---|
| `I2C_TIMEOUT_MS` | `40` | Приблизительный бюджет одного polling wait, мс; зависит от сборки/IRQ |
| `I2C_STRETCH_TIMEOUT_US` | `1000` | Приблизительный polling budget SCL-high в recovery, мкс |
| `I2C_MAX_ERROR_COUNT` | `2` | Число BERR между успешными фазами до recovery; ARLO обрабатывается отдельно |
| `I2C_ATOMIC_CRITICAL` | `0` | `1` — выполнять окна сброса ADDR с запрещёнными прерываниями |
| `I2C_INTER_FRAME_DELAY_US` | `50` | Пауза между пробами адресов в `i2c_probe_address()` |
| `I2C_LEGACY_STATUS` | `0` | `1` — сворачивать timeout/BERR/ARLO/recovery в NACK; ERR_CLK остаётся отдельным |
| `I2C_FIXED_PCLK_HZ` | не задан | Компиляция под фиксированную `PCLK1`; пропускает чтение `SystemCoreClock`; диапазон проверяется при компиляции |
| `I2C_LITE` | `0` | `1` — агрессивный режим экономии, см. [Lite Mode](#lite-mode-экономия-flash-памяти) |
| `I2C_DISABLE_BUS_RECOVERY` | не задан | Убрать GPIO-восстановление шины |
| `I2C_DISABLE_SCANNER` | не задан | Убрать `i2c_probe_address()` |
| `I2C_DISABLE_BUFFER_API` | не задан | Убрать buffer/raw/buffer16 API |
| `I2C_DISABLE_ERROR_COUNTER` | не задан | Убрать `consecutive_errors` (recovery на каждом отказе) |
| `I2C_DISABLE_LAST_ERROR` | не задан | Убрать `i2c_get_last_star1()` и её хранилище |

`I2C_TIMEOUT` и `I2C_STRETCH_TIMEOUT` остаются объявленными для совместимости, но драйвером не используются и будут удалены в 8.0.0.

---

## Полное описание функций API

### Низкоуровневые функции управления и инициализации
* `uint8_t i2c_init(uint32_t bound);`
  Включает RCC/GPIO, проверяет физические линии, выполняет SWRST только на освобождённой шине и настраивает I2C1. PCLK/HCLK 8–48 МГц, скорость до 400 кГц с округлением CCR вверх. Возвращает OK, ERR_CLK или ошибку ожидания/восстановления.
* `void i2c_deinit(void);`
  Вызывайте на свободной шине. Отключает I2C1 и его тактирование APB1, переводит PC1/PC2 в высокоимпедансное состояние. Возвращаемого статуса нет; результат внутреннего ожидания не сообщается.
* `uint8_t i2c_wait_bus_free(void);`
  Ожидает BUSY/MSL/STOP clear с приоритетом аппаратных ошибок; ARLO возвращается без STOP/recovery. При timeout в full вызывается проверяемый recovery; в Lite возвращается ошибка.
* `uint8_t i2c_start(void);`
  Генерирует СТАРТ-условие на шине с проверкой доступности шины. Ограничено таймаутом.
* `uint8_t i2c_repeated_start(void);`
  Генерирует Повторный СТАРТ (Repeated START) без проверки флага `BUSY`.
* `uint8_t i2c_stop(void);`
  Выставляет бит `STOP` и ожидает освобождения шины. Возвращает `I2C_OK` при успехе.
* `uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2);`
  Проверяет 7-битный адрес; возвращает OK только после успешного STOP, иначе NACK или ошибку START/STOP/recovery. Опциональные output pointers могут быть NULL. В конце применяется интервал сканера.
* `uint16_t i2c_get_last_star1(void);`
  Возвращает snapshot последнего обнаруженного STAR1/timeout отказа до очистки флагов; при clock/lifecycle ошибке без STAR1 snapshot не обновляется. Исключается I2C_DISABLE_LAST_ERROR/Lite.

### Функции передачи данных
* `uint8_t i2c_send_addr(uint8_t addr, uint8_t direction);`
  Отправляет 7-битный адрес устройства с битом направления (`I2C_DIR_TX` или `I2C_DIR_RX`). Накладывает маску `direction & 1`. Очищает флаг `ADDR` и сбрасывает счётчик `consecutive_errors`.
  Для чтения одного/двух байтов используйте высокоуровневые read-функции: им нужен специальный порядок ACK/POS относительно сброса ADDR.
* `uint8_t i2c_send_byte(uint8_t data);`
  *Низкоуровневая функция.* Записывает байт в `DATAR` и ожидает только освобождения буфера (`TXE`). **Не проверяет физический прием байта слейвом!**
* `uint8_t i2c_wait_ack(void);`
  *Низкоуровневая функция.* Ожидает флаг завершения передачи (`BTF`) или флаг отсутствия подтверждения (`AF`).
* `static inline uint8_t i2c_write_byte(uint8_t data);`
  *Инлайновая обёртка.* Объединяет `i2c_send_byte` и `i2c_wait_ack`, не запрещая прерывания.

### Высокоуровневое прикладное API
* `uint8_t i2c_write_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);`
  Запись одного байта `value` в 8-битный регистр `reg_addr` устройства `dev_addr`.
* `uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value);`
  Чтение одного байта из 8-битного регистра `reg_addr`.
* `uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len);`
  Последовательная запись массива данных `p_buf` длины `len` в 8-битный регистр `reg_addr`.
* `uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len);`
  Потоковое многобайтовое чтение данных из 8-битного регистра `reg_addr`.
* `uint8_t i2c_write_raw(uint8_t dev_addr, const uint8_t *p_buf, uint16_t len);`
  Прямая многобайтовая запись в `dev_addr` без отправки адреса регистра. Идеально для ЦАП, потокового вывода и безадресных чипов.
* `uint8_t i2c_read_raw(uint8_t dev_addr, uint8_t *p_buf, uint16_t len);`
  Прямое многобайтовое чтение из `dev_addr` без предварительной записи регистра.
* `uint8_t i2c_write_buffer16(uint8_t dev_addr, uint16_t reg_addr, const uint8_t *p_buf, uint16_t len);`
  Пакетная запись с 16-битным адресом памяти/регистра `reg_addr` (big-endian, например для 24LC64).
* `uint8_t i2c_read_buffer16(uint8_t dev_addr, uint16_t reg_addr, uint8_t *p_buf, uint16_t len);`
  Пакетное чтение с 16-битным адресом памяти/регистра `reg_addr` (big-endian).

---

## Полные примеры практического применения

### Пример 1: Сканирование шины I2C (I2C Bus Scanner)

Для видимого вывода `printf()` настройте перенаправление stdout на UART (например, `_write` или `putchar`).
Высокоуровневая функция `i2c_probe_address()` инкапсулирует START, отправку адреса и STOP в один безопасный вызов:

```c
#include "i2c.h"
#include <stdio.h>

void i2c_scan_bus(void) {
    printf("--- I2C1 scanner ---\n");
    printf("     0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");

    uint8_t found = 0;

    for (uint8_t i = 0; i < 128; i += 16) {
        printf("%02X: ", i);
        for (uint8_t j = 0; j < 16; j++) {
            uint8_t addr = i + j;
            
            // Пропускаем зарезервированные адреса
            if (addr < 0x08 || addr > 0x77) {
                printf("   ");
                continue;
            }
            
            // i2c_probe_address() сам выполняет START, адрес, STOP и паузу
            if (i2c_probe_address(addr, NULL, NULL) == I2C_OK) {
                printf("%02X ", addr); // Устройство ответило ACK!
                found++;
            } else {
                printf("-- "); // NACK or another error; inspect the status for diagnostics
            }
        }
        printf("\n");
    }
    printf("--- found: %d ---\n", found);
}
```

### Пример 2: Чтение FIFO буфера жестов датчика APDS-9960

Предполагается, что gesture engine и FIFO уже настроены; один набор содержит четыре байта. Чтение FIFO и распознавание жестов не входили в опубликованные аппаратные испытания клона.

```c
#include "i2c.h"

#define APDS9960_I2C_ADDR    0x39
#define APDS9960_REG_GFLVL   0xAE   // Регистр уровня заполнения FIFO
#define APDS9960_REG_GFIFO_R 0xFC   // Начальный регистр чтения FIFO

static uint8_t raw_gesture_data[128];

void handle_gesture_sensor(void) {
    uint8_t datasets_count = 0;
    
    // Читаем количество доступных наборов данных
    if (i2c_read_register(APDS9960_I2C_ADDR, APDS9960_REG_GFLVL, &datasets_count) != I2C_OK) {
        return;
    }
    
    if (datasets_count == 0) return;
    if (datasets_count > 32) datasets_count = 32;
    
    uint16_t total_bytes = datasets_count * 4;
    
    // Пакетное чтение FIFO буфера данных
    if (i2c_read_buffer(APDS9960_I2C_ADDR, APDS9960_REG_GFIFO_R, raw_gesture_data, total_bytes) == I2C_OK) {
        for (uint16_t i = 0; i < datasets_count; i++) {
            uint16_t base = i * 4;
            uint8_t up    = raw_gesture_data[base + 0];
            uint8_t down  = raw_gesture_data[base + 1];
            uint8_t left  = raw_gesture_data[base + 2];
            uint8_t right = raw_gesture_data[base + 3];
            
            // Обработка жестов...
        }
    }
}
```

### Пример 3: Запись и чтение энергонезависимой памяти EEPROM (24LC64 / 24LCxx)

```c
#include "i2c.h"

#define EEPROM_I2C_ADDR    0x50     // Базовый адрес 24LC64
#define PAGE_START_ADDR    0x0020   // 16-битный адрес ячейки памяти внутри EEPROM

static const uint8_t calibration_table[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};

uint8_t save_calibration(void) {
    // START -> 0x50(TX) -> Addr_High -> Addr_Low -> Data[0..7] -> STOP
    return i2c_write_buffer16(EEPROM_I2C_ADDR, PAGE_START_ADDR, calibration_table, 8);
}

uint8_t load_calibration(uint8_t *p_buf) {
    // START -> 0x50(TX) -> Addr_High -> Addr_Low -> Repeated START -> 0x50(RX) -> Read[0..7] -> STOP
    return i2c_read_buffer16(EEPROM_I2C_ADDR, PAGE_START_ADDR, p_buf, 8);
}
```

EEPROM: вызывающий код должен учитывать границы страниц, банковые биты адреса и время внутренней записи конкретной модели. Успешный STOP не означает завершения программирования памяти; перед чтением/следующей записью дождитесь готовности (например, ACK polling с общим таймаутом). Драйвер не разбивает запись на страницы. Этот пример аппаратно не проверялся.

### Пример 4: Работа с DAC7571

У ЦАП DAC7571 (Texas Instruments) есть важная особенность: в отличие от большинства датчиков, у него нет внутренних адресов регистров. Этот 12-битный одноканальный ЦАП ожидает, что сразу после своего I2C-адреса мастер передаст ровно 2 байта данных. Поэтому стандартная `i2c_write_register()` здесь не подойдёт (она отправляет 3 байта: адрес → регистр → данные).

#### Протокол записи в DAC7571

После отправки адреса устройства (0x4C или 0x4D) ЦАП ждёт два байта:

- **Byte 1 (MSB):** `[ 0 | 0 | PD1 | PD0 | D11 | D10 | D9 | D8 ]` — биты управления питанием + 4 старших бита данных.
- **Byte 2 (LSB):** `[ D7  | D6  | D5 | D4 | D3  | D2  | D1  | D0 ]` — 8 младших бит данных.

Для нормальной работы биты Power-Down (PD1, PD0) должны быть равны 00. Размещение битов: [TI DAC7571, таблица 1](https://www.ti.com/lit/ds/symlink/dac7571.pdf). Примеры DAC аппаратно не проверялись.

#### Приоритетный вариант: Raw API (v7.0.0)

```c
#include "i2c.h"

#define DAC7571_I2C_ADDR    0x4C

/**
 * @brief Установка выходного напряжения на ЦАП DAC7571
 * @param data_12bit: значение от 0 до 4095 (12 бит)
 * @return I2C_OK или код ошибки
 */
uint8_t dac7571_set_voltage(uint16_t data_12bit) {
    if (data_12bit > 4095) data_12bit = 4095;

    uint8_t buf[2];
    buf[0] = (uint8_t)((data_12bit >> 8) & 0x0F); // Normal Mode + 4 старших бита
    buf[1] = (uint8_t)(data_12bit & 0xFF);        // 8 младших бит

    return i2c_write_raw(DAC7571_I2C_ADDR, buf, 2);
}
```

#### Альтернатива: Низкоуровневое API (единственный вариант в Lite-режиме)

Lite-сборки (`I2C_LITE=1` / `I2C_DISABLE_BUFFER_API`) исключают raw- и buffer-API из компиляции. Открытая низкоуровневая последовательность остаётся доступной и является там рекомендуемым путём:

```c
#include "i2c.h"

// Адрес DAC7571 зависит от пина A0:
// Если A0 подключен к GND, 7-битный адрес = 0x4C (бинарно: 1001100)
// Если A0 подключен к VDD, 7-битный адрес = 0x4D (бинарно: 1001101)
#define DAC7571_I2C_ADDR    0x4C

/**
 * @brief Установка выходного напряжения на ЦАП DAC7571
 * @param data_12bit: значение от 0 до 4095 (12 бит)
 * @return I2C_OK или код ошибки
 */
uint8_t dac7571_set_voltage_ll(uint16_t data_12bit) {
    if (data_12bit > 4095) {
        data_12bit = 4095;
    }

    uint8_t byte_msb = (uint8_t)((data_12bit >> 8) & 0x0F);
    uint8_t byte_lsb = (uint8_t)(data_12bit & 0xFF);

    uint8_t res = i2c_start();
    if (res != I2C_OK) return res;

    if ((res = i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte(byte_msb)) != I2C_OK ||
        (res = i2c_write_byte(byte_lsb)) != I2C_OK) return res;

    return i2c_stop();
}

/**
 * @brief Перевод ЦАП в режим сна (Power-Down) для экономии энергии
 * @param mode: 1 - подтяжка 1 кОм к GND, 2 - 100 кОм к GND, 3 - High-Z
 */
uint8_t dac7571_power_down(uint8_t mode) {
    if (mode < 1 || mode > 3) return I2C_NACK;

    // PD1:PD0 находятся в битах 5:4 первого байта
    uint8_t byte_msb = (uint8_t)(mode << 4);

    uint8_t res = i2c_start();
    if (res != I2C_OK) return res;
    if ((res = i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte(byte_msb)) != I2C_OK ||
        (res = i2c_write_byte(0x00)) != I2C_OK) return res;

    return i2c_stop();
}
```

#### Генерация пилообразного сигнала (Sawtooth Wave)

```c
#include "i2c.h"

int main(void) {
    SystemCoreClockUpdate();
    if (i2c_init(400000) != I2C_OK) {
        while (1) { /* initialization failed */ }
    }
    
    uint16_t dac_value = 0;

    while(1) {
        if (dac7571_set_voltage(dac_value) != I2C_OK) {
            /* Handle the error; a failed recovery requires reinitialization. */
            break;
        }
        
        dac_value += 4;
        if (dac_value >= 4096) {
            dac_value = 0;
        }
    }
}
```

---

## Алгоритм восстановления шины (Как это устроено внутри)

Только для шины с одним мастером. Драйвер выключает PE, заранее отпускает выходные GPIO-защёлки и переключает пины в open-drain GPIO. Ожидание SCL-high ограничено и проверяется. Пока SDA low, выдаётся не более девяти импульсов; GPIO STOP и SWRST выполняются только после освобождения обеих линий. Затем восстанавливаются настройки, PE и проверяется idle.

Если линии не отпущены, конфигурация или проверка idle не удалась, GPIO остаются отпущенными, PE выключен, возвращается `I2C_ERR_RECOVERY` (legacy: `I2C_NACK`). Устраните физическую причину и повторите `i2c_init()`. Reset не устраняет внешнее удержание линии. Lite/no-recovery возвращает ошибку без GPIO-восстановления и может требовать повторной инициализации. Snapshot STAR1 сохраняет первичную ошибку, даже если завершение тоже отказало.

## Установка и интеграция

### Вариант 1: PlatformIO (рекомендуется)

```ini
lib_deps =
    https://github.com/tama18101971/I2C-CH32V003.git#v7.2.0
```

Проверенное окружение: PlatformIO Core 6.2.0, `framework = noneos-sdk`, плата `genericCH32V003F4P6`. Ревизии platform и SDK закреплены в [корневом platformio.ini](platformio.ini) и конфигурациях примеров.

### Вариант 2: Ручная интеграция

Скопируйте `i2c.h` и `i2c.c` из папки `src/` в ваш проект.

```c
#include "i2c.h"
```

Инициализируйте шину:

```c
if (i2c_init(400000) != I2C_OK) {
    /* Handle initialization failure before using the bus. */
}
```

И включите LTO в секции окружения `platformio.ini` — это экономит Flash (точные значения см. в таблице размеров):

```ini
board_build.use_lto = yes
```

---

## Lite Mode (экономия Flash-памяти)

Для приложений с жёстким бюджетом Flash режим **Aggressive Lite** условно исключает части драйвера:

```ini
build_flags = -DI2C_LITE=1
```

Или точечно отключайте модули:
```ini
build_flags =
    -DI2C_DISABLE_BUS_RECOVERY    ; убрать i2c_bus_recovery()
    -DI2C_DISABLE_SCANNER         ; убрать i2c_probe_address()
    -DI2C_DISABLE_BUFFER_API      ; убрать buffer/raw/buffer16 API
    -DI2C_DISABLE_ERROR_COUNTER   ; убрать consecutive_errors
    -DI2C_DISABLE_LAST_ERROR      ; убрать i2c_get_last_star1()
```

Независимо от этих флагов включите LTO — это ничего не стоит и экономит Flash в проверенных сборках (264 B для Register full и 124 B для Register Lite):

```ini
board_build.use_lto = yes
```

> Примечание: на платформе `ch32v` LTO запрашивается только через `board_build.use_lto`. Передача `-flto` через `build_flags` не даёт эффекта: флаг доходит до компилятора, но не до линковщика.

### Что остаётся в Lite

| API | Lite | Full |
|---|:-:|:-:|
| `i2c_init`, `i2c_deinit` | + | + |
| `i2c_start`, `i2c_repeated_start`, `i2c_stop` | + | + |
| `i2c_send_addr`, `i2c_send_byte`, `i2c_wait_ack`, `i2c_write_byte` | + | + |
| `i2c_wait_bus_free` | + | + |
| `i2c_write_register`, `i2c_read_register` | + | + |
| `i2c_probe_address` (сканер) | **−** | + |
| `i2c_write_buffer`, `i2c_read_buffer` | **−** | + |
| `i2c_write_raw`, `i2c_read_raw` | **−** | + |
| `i2c_write_buffer16`, `i2c_read_buffer16` | **−** | + |
| `i2c_get_last_star1` | **−** | + |
| `i2c_bus_recovery` (Clock Recovery) | **−** | + |
| Счётчик `consecutive_errors` | **−** | + |

### Измеренный размер Flash (v7.2.0)

GCC 12.2.0, CH32V003F4P6, LTO, суммы ELF сверены с binary. Полная таблица и условия: [результаты](test/hardware/RESULTS.md).

| Профиль | Flash | RAM, включая резерв стека 256 байт |
|---|---:|---:|
| Register API full | 2536 B | 296 B |
| Register API no recovery | 2076 B | 292 B |
| Register API no error counter | 2464 B | 296 B |
| Register API no buffer | 2536 B | 296 B |
| Register API Lite | 2076 B | 292 B |
| All API full | 2976 B | 296 B |
| All API no buffer | 2684 B | 296 B |
| All API no scanner | 2928 B | 296 B |
| All API Lite | 2156 B | 292 B |
| Register full without LTO | 2800 B | 296 B |
| Register Lite without LTO | 2200 B | 292 B |

## Тестирование

Запуск `pwsh test/run_tests.ps1`: до 46 тестов в конфигурации, 396 выполнений тестовых функций в десяти профилях, с `-Wall -Wextra -Werror`. Проверены чтения 0/1/2/3/4/5/8/16/255/65535 байтов с лишним байтом у slave и защитными байтами буфера. Добавлены ready+error, ошибки START/STOP, сбои в RX, неудачное восстановление SCL/SDA, потеря владения при ARLO и сохранение IRQ. Модель дискретная, без точной эмуляции тактов и электрических параметров.

Команды аппаратных тестов ниже предназначены для Windows, PlatformIO и pyserial (Python из окружения PlatformIO).

Реальные испытания используют `test/hardware/apds9960.c`, WCH-Link UART COM6 (TX платы PD5), модуль APDS на PC1 SDA / PC2 SCL:

```powershell
# Команды прошивают плату; ENABLE и ALS-пороги временно меняются и восстанавливаются.
& "$HOME/.platformio/penv/Scripts/python.exe" test/hardware/run_hil.py --env hardware_apds
& "$HOME/.platformio/penv/Scripts/python.exe" test/hardware/run_hil.py --env hardware_apds_polling
```

[Отчёт и первичные журналы](test/hardware/RESULTS.md) отделяют программные проверки от квалификации осциллограмм. На плате проверены 15 сочетаний CPU/скорости, чтения 1/2/3/4/5/8/16, запись с обратным чтением, нагрузка ISR, NACK, BTF timeout/recovery и восстановление настроек датчика. Физическое удержание линий, ARLO со вторым мастером, EEPROM с 16-битной адресацией, аналоговые параметры и 24-часовой soak требуют дополнительного оборудования; такие испытания не заявляются.

## Лицензия

Библиотека распространяется под свободной лицензией MIT.

[LICENSE](LICENSE)
