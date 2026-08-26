# Надёжный драйвер шины I2C (I2C1) для CH32V003 (RISC-V) — Версия 7.0.0

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

[🇬🇧 English](README.md)

Высоконадёжный, устойчивый к аппаратным сбоям шины и оптимизированный по размеру памяти драйвер I2C для микроконтроллеров серии **CH32V003**. Разработан для полной замены стандартной библиотеки WCH EVT, которая склонна к глухим зависаниям ядра в бесконечных циклах ожидания флагов (`while(!I2C_CheckEvent(...))`) при возникновении электромагнитных помех, просадках питания ведомых устройств или физическом замыкании линий.

---

## Архитектурные особенности (Версия 7.0.0)

1. **Совместимость с C++:** Заголовочный файл `i2c.h` обёрнут в `extern "C"` блоки для прозрачной сборки в C++ и Arduino проектах.
2. **Мгновенный STOP без задержек (`i2c_stop`):** Устранена принудительная задержка 50 мкс из стандартного вызова STOP. Межкадровая пауза перенесена в `i2c_probe_address()` исключительно для сканера адресов, что позволило получить предельную скорость шины (до 400 кГц) при потоковом выводе данных в ЦАП или на дисплеи.
3. **Динамический масштабируемый таймаут:** Циклы ожидания масштабируются по частоте `SystemCoreClock` через `I2C_TIMEOUT_MS` (по умолчанию 40 мс), предотвращая разброс реального таймаута при разных частотах системного тактирования от 2 до 48 МГц.
4. **Защита в read-only сценариях:** Счётчик критических ошибок `consecutive_errors` сбрасывается при успешном ACK на этапе адресации `ADDR` в `i2c_send_addr()`. Это предотвращает накопление спорадических редких ошибок в ходе длительных сессий чтения.
5. **Raw API и 16-битные адреса регистров:** Добавлены функции `i2c_write_raw()` / `i2c_read_raw()` для чипов без внутренней адресации регистров (ЦАП DAC7571) и `i2c_write_buffer16()` / `i2c_read_buffer16()` для памяти с 16-битным адресом слова (EEPROM 24LC32..24LC1025).
6. **Детализированные коды возврата:** Введены явные коды ошибок (`I2C_ERR_TIMEOUT`, `I2C_ERR_BERR`, `I2C_ERR_ARLO`, `I2C_ERR_CLK`, `I2C_NACK`, `I2C_OK`). Для совместимости со старым кодом доступен флаг `-DI2C_LEGACY_STATUS=1`.
7. **Валидация скорости шины:** Функция `i2c_init()` проверяет скорость (до 400 кГц) и возвращает `I2C_ERR_CLK` при некорректных параметрах.
8. **Изоляция и инкапсуляция:** Функция восстановления шины `i2c_bus_recovery` объявлена `static` и изолирована внутри `i2c.c`.
9. **Мгновенное восстановление:** При застревании шины в `BUSY` или на этапе `START`/`ADDR` драйвер немедленно запускает процедуру восстановления шины.
10. **Безопасное разделение API:** `i2c_send_byte` освобождает буфер `DATAR`, а `i2c_write_byte` атомарно передаёт байт с контролем ACK.
11. **Защита бита ACK при авариях:** Во всех ветках ошибок чтения бит `ACK = 1` принудительно восстанавливается перед выходом.
12. **Оптимизация под RV32EC:** Размеры буферов типизированы как `uint16_t` для снижения накладных расходов вызовов.

---

## Физический уровень и подключение

Драйвер работает с аппаратным блоком **I2C1** на штатных выводах контроллера:

* **PC1 — SDA** (Режим: Alternate Function Open-Drain, 50MHz)
* **PC2 — SCL** (Режим: Alternate Function Open-Drain, 50MHz)

> **ВАЖНО:** Для корректной работы шины на обеих линиях (SDA и SCL) **обязательно** должны быть установлены внешние подтягивающие (pull-up) резисторы номиналом от **2.2 кОм до 4.7 кОм** к линии питания 3.3В. Встроенная подтяжка микроконтроллера не способна обеспечить необходимую крутизну фронтов на частотах выше 10 кГц.

---

## Коды возврата

| Код | Значение | Описание |
|---|:---:|---|
| `I2C_OK` | `0` | Успешно / Подтверждено (ACK) |
| `I2C_NACK` | `1` | Не подтверждено (NACK от ведомого) или общая ошибка |
| `I2C_ERR_TIMEOUT` | `2` | Программный таймаут ожидания шины или аппаратного флага |
| `I2C_ERR_CLK` | `3` | Некорректная частота (PCLK1 вне 2..48 МГц, speed > 400 кГц или speed == 0) |
| `I2C_ERR_BERR` | `4` | Аппаратная ошибка шины (Bus Error — некорректный START/STOP) |
| `I2C_ERR_ARLO` | `5` | Потеря арбитража (Arbitration Lost в мультимастерной сети) |

*Все ненулевые коды корректно отрабатывают при проверке `if (err != I2C_OK)`.* Для возврата к логике v6 (все ошибки сворачиваются в `I2C_NACK`) задайте флаг компиляции `-DI2C_LEGACY_STATUS=1`.

---

## Полное описание функций API

### Низкоуровневые функции управления и инициализации
* `uint8_t i2c_init(uint32_t bound);`
  Выполняет сброс блока I2C1 через `SWRST`, настраивает тактирование GPIO и периферии, вычисляет регистры `CTLR2` и `CKCFGR` для Standard Mode (до 100 кГц) или Fast Mode (до 400 кГц). Автоматически выставляет обязательный 14-й бит в `OADDR1`. Возвращает `I2C_OK` или `I2C_ERR_CLK` при некорректных частотах или скорости `bound > 400000`.
* `void i2c_deinit(void);`
  Отключает периферию I2C1, деактивирует тактирование шины APB1 и переводит пины PC1/PC2 в высокоимпедансное состояние.
* `uint8_t i2c_wait_bus_free(void);`
  Опрашивает флаг `BUSY`. Контролирует ошибки `BERR`/`ARLO` для немедленного восстановления. При превышении `I2C_TIMEOUT_MS` аварийно вызывает `i2c_bus_recovery`.
* `uint8_t i2c_start(void);`
  Генерирует СТАРТ-условие на шине с проверкой доступности шины. Ограничено таймаутом.
* `uint8_t i2c_repeated_start(void);`
  Генерирует Повторный СТАРТ (Repeated START) без проверки флага `BUSY`.
* `uint8_t i2c_stop(void);`
  Выставляет бит `STOP` и ожидает освобождения шины. Возвращает `I2C_OK` при успехе.
* `uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2);`
  Проверка одного 7-битного I2C-адреса. Возвращает `I2C_OK` если устройство ответило ACK, иначе `I2C_NACK`. Включает межкадровую паузу `I2C_INTER_FRAME_DELAY_US`.

### Функции передачи данных
* `uint8_t i2c_send_addr(uint8_t addr, uint8_t direction);`
  Отправляет 7-битный адрес устройства с битом направления (`I2C_DIR_TX` или `I2C_DIR_RX`). Накладывает маску `direction & 1`. Очищает флаг `ADDR` и сбрасывает счётчик `consecutive_errors`.
* `uint8_t i2c_send_byte(uint8_t data);`
  *Низкоуровневая функция.* Записывает байт в `DATAR` и ожидает только освобождения буфера (`TXE`). **Не проверяет физический прием байта слейвом!**
* `uint8_t i2c_wait_ack(void);`
  *Низкоуровневая функция.* Ожидает флаг завершения передачи (`BTF`) или флаг отсутствия подтверждения (`AF`).
* `static inline uint8_t i2c_write_byte(uint8_t data);`
  *Атомарная инлайновая функция.* Объединяет `i2c_send_byte` и `i2c_wait_ack`.

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
  Пакетная запись с 16-битным адресом памяти/регистра `reg_addr` (big-endian, для EEPROM 24LC32..24LC1025).
* `uint8_t i2c_read_buffer16(uint8_t dev_addr, uint16_t reg_addr, uint8_t *p_buf, uint16_t len);`
  Пакетное чтение с 16-битным адресом памяти/регистра `reg_addr` (big-endian).

---

## Полные примеры практического применения

### Пример 1: Сканирование шины I2C (I2C Bus Scanner)
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
                printf("-- "); // Нет ответа (NACK)
            }
        }
        printf("\n");
    }
    printf("--- found: %d ---\n", found);
}
```

### Пример 2: Чтение FIFO буфера жестов датчика APDS-9960

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

### Пример 4: Работа с DAC7571 через Raw API

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

#### Генерация пилообразного сигнала (Sawtooth Wave)

```c
#include "i2c.h"

int main(void) {
    SystemCoreClockUpdate();
    i2c_init(400000); // 400 кГц Fast Mode
    
    uint16_t dac_value = 0;

    while(1) {
        dac7571_set_voltage(dac_value);
        
        dac_value += 4;
        if (dac_value >= 4096) {
            dac_value = 0;
        }
    }
}
```

---

## Алгоритм восстановления шины (Как это устроено внутри)

Если внешнее устройство зависло на полуслове и удерживает линию данных SDA в состоянии LOW:

1. `I2C1->CTLR1 &= ~I2C_CTLR1_PE;` полностью выключает аппаратный блок I2C.
2. Пины PC1 и PC2 переводятся в режим программного выхода с открытым стоком (`GPIO_PC1_PC2_OUT_OD_2M`).
3. Драйвер генерирует до 16 импульсов на линии SCL с контролем отпускания SDA и Clock Stretching (`I2C_STRETCH_TIMEOUT`).
4. Силами GPIO формируется STOP-условие: SCL LOW → SDA LOW → SCL HIGH → SDA HIGH.
5. Блоку I2C1 выдается команда аппаратного сброса SWRST.
6. Пины PC1 и PC2 переключаются обратно в режим альтернативной функции AF_OD.
7. Восстанавливаются конфигурационные регистры и активируется аппаратный ACK.

---

## Установка и интеграция

### Вариант 1: PlatformIO (рекомендуется)

```ini
lib_deps =
    https://github.com/tama18101971/I2C-CH32V003.git
```

### Вариант 2: Ручная интеграция

Скопируйте `i2c.h` и `i2c.c` из папки `src/` в ваш проект.

```c
#include "i2c.h"
```

Инициализируйте шину:

```c
i2c_init(400000); // Fast Mode 400 кГц
```

---

## Lite Mode (экономия Flash-памяти)

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
```

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
| `i2c_bus_recovery` (Clock Recovery) | **−** | + |
| Счётчик `consecutive_errors` | **−** | + |

### Измеренная экономия Flash (v7.0.0)

Результаты `pio run` на `genericCH32V003F4P6` (NoneOS SDK, release build):

| Профиль | Build flags | Flash | RAM | Изменение Flash |
|---|---|---:|---:|---:|
| Full | — | 2268 B | 292 B | — |
| Без recovery | `I2C_DISABLE_BUS_RECOVERY` | 1892 B | 292 B | **−376 B** |
| Без счётчика ошибок | `I2C_DISABLE_ERROR_COUNTER` | 2208 B | 292 B | −60 B |
| Без buffer API | `I2C_DISABLE_BUFFER_API` | 2216 B | 292 B | −52 B |
| Lite | `I2C_LITE=1` | 1844 B | 292 B | **−424 B** |

---

## Лицензия

Библиотека распространяется под свободной лицензией MIT.

[LICENSE](LICENSE)
