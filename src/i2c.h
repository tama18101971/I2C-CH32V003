#ifndef I2C_H
#define I2C_H

/*
 * i2c.h — Блокирующий драйвер I2C1 для CH32V003 — Версия 7.2.1
 *
 * Применение: APDS-9960, DAC7571, EEPROM (24LCxx), OLED (SSD1306) и др.
 * Пин-конфигурация: PC1 — SDA, PC2 — SCL (AF_OD, Open-Drain).
 *
 * === Контракты использования (обязательно к прочтению) ===
 *
 * 1. МОДЕЛЬ КОНКУРЕНТНОСТИ: один контекст исполнения.
 *    Драйвер хранит состояние в статических переменных и монопольно владеет
 *    блоком I2C1. Функции НЕ реентерабельны и НЕ предназначены для вызова из
 *    обработчиков прерываний. Вызов любой функции из ISR одновременно с
 *    main-контекстом разрушает текущую транзакцию. Если I2C нужен из нескольких
 *    контекстов, синхронизация — задача вызывающего кода.
 *
 * 2. ПРЕРЫВАНИЯ ВО ВРЕМЯ ЧТЕНИЯ: аппаратура требует, чтобы служебные
 *    последовательности приёма 1/2 байтов завершились до конца
 *    передачи текущего байта на шине. По умолчанию драйвер их не защищает,
 *    что безопасно для чистого polling-режима. Если в системе есть прерывания
 *    длительностью больше одной байтовой посылки, соберите драйвер с
 *    -DI2C_ATOMIC_CRITICAL=1 (см. ниже).
 *
 * 3. ПИНЫ: предполагается конфигурация I2C1 по умолчанию (AFIO_PCFR1.I2C1_RM[22,1] = 00,
 *    SDA = PC1, SCL = PC2). Драйвер не читает и не изменяет регистры ремапа AFIO.
 *    При включённом ремапе (PD1/PD0 или PC5/PC6) драйвер настроит не те пины.
 *
 * 4. УКАЗАТЕЛИ не проверяются на NULL (кроме явно опциональных параметров
 *    i2c_probe_address). Передача невалидного буфера при len > 0 —
 *    неопределённое поведение.
 *
 * 5. ПОРЯДОК ВЫЗОВОВ: i2c_init() должен быть вызван до любой другой функции
 *    (внутренние таймауты и задержки вычисляются в нём).
 *
 * 6. СЕМАНТИКА len == 0:
 *      - чтение (i2c_read_buffer / i2c_read_raw / i2c_read_buffer16) возвращает
 *        I2C_OK, не задействуя шину;
 *      - запись (i2c_write_buffer / i2c_write_raw / i2c_write_buffer16) выполняет
 *        полную транзакцию START + адрес устройства [+ адрес регистра] + STOP.
 *        Это штатный способ спозиционировать указатель регистра или проверить
 *        наличие устройства.
 *
 * 7. SINGLE MASTER: GPIO recovery допустим только на шине с одним мастером.
 *    ARLO возвращается без STOP/recovery; multi-master recovery не поддержан.
 *    После I2C_ERR_RECOVERY PE выключен, OD-линии отпущены; устраните физическую
 *    неисправность и вызовите i2c_init() повторно. Успех RX включает завершение STOP.
 * 8. CLOCK: поддерживается PCLK1/HCLK 8..48 МГц (консервативно по CTLR2.FREQ).
 *    После смены clock повторите успешный init. TIMEOUT_MS — приблизительный
 *    бюджет каждого polling wait, не единый wall-clock deadline транзакции.
 */

#include "ch32v00x.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Конфигурация Lite-режима (экономия Flash) ===
 * I2C_LITE=1 — Aggressive Lite: убирает bus recovery, сканер, buffer/raw API,
 * счётчик критических ошибок и снимок последней ошибки.
 * Таймаут во всех операциях сохранён, но при аппаратной ошибке (BERR/ARLO)
 * или зависании шины функция просто возвращает код ошибки без попытки
 * аппаратного восстановления шины.
 * По умолчанию (I2C_LITE=0 или не задан) — полная отказоустойчивая версия.
 */
#ifndef I2C_LITE
#define I2C_LITE 0
#endif

#if I2C_LITE
#define I2C_DISABLE_BUS_RECOVERY
#define I2C_DISABLE_SCANNER
#define I2C_DISABLE_BUFFER_API
#define I2C_DISABLE_ERROR_COUNTER
#define I2C_DISABLE_LAST_ERROR
#endif

/* Таймауты операций */
#ifndef I2C_TIMEOUT_MS
#define I2C_TIMEOUT_MS          40          /* Приблизительный бюджет одного polling wait; не общий deadline */
#endif

/* Приблизительный бюджет ожидания SCL в recovery, в мкс.
 * Число итераций масштабируется по PCLK1; длительность зависит от сборки/IRQ.
 */
#ifndef I2C_STRETCH_TIMEOUT_US
#define I2C_STRETCH_TIMEOUT_US  1000
#endif

/* Лимит BERR между успешными фазами обмена перед
 * принудительным восстановлением шины. Действует только при включённом
 * счётчике ошибок (см. I2C_DISABLE_ERROR_COUNTER).
 */
#ifndef I2C_MAX_ERROR_COUNT
#define I2C_MAX_ERROR_COUNT     2
#endif

#if I2C_TIMEOUT_MS < 1 || I2C_TIMEOUT_MS > 89478
#error "I2C_TIMEOUT_MS must be in 1..89478 (32-bit polling budget)"
#endif
#if I2C_STRETCH_TIMEOUT_US < 1 || I2C_STRETCH_TIMEOUT_US > 89478485
#error "I2C_STRETCH_TIMEOUT_US must be in 1..89478485"
#endif
#if I2C_MAX_ERROR_COUNT < 1 || I2C_MAX_ERROR_COUNT > 255
#error "I2C_MAX_ERROR_COUNT must be in 1..255"
#endif
#ifdef I2C_FIXED_PCLK_HZ
#if I2C_FIXED_PCLK_HZ < 8000000UL || I2C_FIXED_PCLK_HZ > 48000000UL
#error "I2C_FIXED_PCLK_HZ must be in 8..48 MHz"
#endif
#endif

/* Защита служебных последовательностей приёма критическими секциями.
 * 0 (по умолчанию) — секции отключены, минимальный размер кода;
 * 1 — сброс ADDR/снятие ACK/программирование STOP выполняются с запрещёнными
 *     прерываниями с сохранением/восстановлением исходной маски mstatus,
 *     если прерывание длится дольше одной байтовой посылки.
 */
#ifndef I2C_ATOMIC_CRITICAL
#define I2C_ATOMIC_CRITICAL 0
#endif

/* Компиляция под фиксированную частоту PCLK1 (в герцах).
 * Если задано, i2c_init() не читает SystemCoreClock. Частота проверяется при
 * компиляции; расчёты могут сворачиваться оптимизатором. Speed проверяется всегда.
 */
/* #define I2C_FIXED_PCLK_HZ 48000000UL */

/* DEPRECATED: не используется драйвером с версии 7.0.0, оставлен только для
 * совместимости со старым пользовательским кодом. Будет удалён в 8.0.0.
 */
#define I2C_TIMEOUT             100000      /* deprecated */

/* DEPRECATED: заменён на I2C_STRETCH_TIMEOUT_US (масштабируется по PCLK1).
 * Оставлен для совместимости, драйвером не используется. Будет удалён в 8.0.0.
 */
#define I2C_STRETCH_TIMEOUT     1000        /* deprecated */

/* === Детализированные коды ошибок (v7.0.0) ===
 * По умолчанию таймаут и аппаратные ошибки возвращают
 * I2C_ERR_TIMEOUT / I2C_ERR_BERR / I2C_ERR_ARLO для точной диагностики.
 * Определите I2C_LEGACY_STATUS=1, чтобы вернуть поведение v6.0.0:
 * все ошибки сворачиваются в единый I2C_NACK — для старого кода,
 * сравнивающего результат только с I2C_NACK.
 */
#ifndef I2C_LEGACY_STATUS
#define I2C_LEGACY_STATUS 0
#endif

/* Пауза между STOP и следующим START при сканировании, мкс */
#ifndef I2C_INTER_FRAME_DELAY_US
#define I2C_INTER_FRAME_DELAY_US  50
#endif

/* Направление передачи */
#define I2C_DIR_TX  0   /* Master Transmitter */
#define I2C_DIR_RX  1   /* Master Receiver */

/* Коды возврата */
#define I2C_OK          0   /* Успешно / Подтверждено (ACK) */
#define I2C_NACK        1   /* Не подтверждено (NACK от ведомого / ошибка адреса) */
#define I2C_ERR_TIMEOUT 2   /* Таймаут ожидания шины или флага */
#define I2C_ERR_CLK     3   /* PCLK1 вне 8..48 МГц, speed=0/>400к или CCR недостижим */
#define I2C_ERR_BERR    4   /* Аппаратная ошибка шины (Bus Error — некорректный START/STOP) */
#define I2C_ERR_ARLO    5   /* Потеря арбитража (Arbitration Lost) */
#define I2C_ERR_RECOVERY 6  /* Восстановление не удалось / PE выключен: нужен успешный init */

/* Основное API управления шиной */
uint8_t i2c_init(uint32_t bound);                          /* OK, ERR_CLK или ошибка готовности/recovery шины */
void i2c_deinit(void);                                    /* Отключение I2C1 (вызывать на свободной шине) */
uint8_t i2c_wait_bus_free(void);                          /* Ожидание освобождения шины с авто-recovery */
uint8_t i2c_start(void);                                  /* Генерация START с проверкой BUSY */
uint8_t i2c_repeated_start(void);                         /* Генерация Повторного СТАРТа (Repeated START) */
uint8_t i2c_stop(void);                                   /* Генерация STOP с контролем таймаута */

/**
 * @brief Снимок регистра STAR1, сделанный в момент обнаружения последней ошибки.
 * @note Значение фиксируется ДО очистки флагов AF/BERR/ARLO, поэтому содержит
 *       реальную причину сбоя (в отличие от прямого чтения I2C1->STAR1 после
 *       возврата из API, где флаги уже сняты). Обнуляется в i2c_init().
 *       Clock/lifecycle ошибки без опроса STAR1 снимок не обновляют.
 *       Компилируется вне Lite-режима (см. I2C_DISABLE_LAST_ERROR).
 */
#ifndef I2C_DISABLE_LAST_ERROR
uint16_t i2c_get_last_star1(void);
#endif

/**
 * @brief Проверка наличия устройства по адресу (для сканера шины).
 * @param addr    7-битный адрес устройства
 * @param p_star1 куда сохранить снимок STAR1 на момент ошибки (можно NULL)
 * @param p_star2 куда сохранить STAR2 (можно NULL)
 * @return I2C_OK если устройство ответило ACK, иначе I2C_NACK / код ошибки
 */
#ifndef I2C_DISABLE_SCANNER
uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2);
#endif

/* Низкоуровневое API передачи */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction);   /* Отправка адреса + направление */
uint8_t i2c_wait_ack(void);                               /* Ожидание подтверждения (ACK) от ведомого */

/**
 * @brief Отправка одного байта данных.
 * @warning ВНИМАНИЕ! Функция НЕ завершает трансляцию байта на физический уровень,
 * а лишь ждет освобождения регистра DATAR (флаг TXE). 
 * После нее ОБЯЗАТЕЛЬНО нужно вызвать i2c_wait_ack()!
 */
uint8_t i2c_send_byte(uint8_t data);

/**
 * @brief Отправка байта с ожиданием подтверждения (встроенная атомарная функция)
 */
static inline uint8_t i2c_write_byte(uint8_t data) {
    uint8_t res = i2c_send_byte(data);
    if (res != I2C_OK) {
        return res;
    }
    return i2c_wait_ack();
}

/* Высокоуровневое симметричное API для работы с регистрами устройств (APDS-9960 и др.) */
uint8_t i2c_write_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);
uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value);

/* Пакетное API.
 * len == 0: чтение возвращает I2C_OK без обращения к шине; запись выполняет
 * транзакцию START + адрес [+ адрес регистра] + STOP (см. контракт 6 выше).
 * Указатели буферов не проверяются на NULL.
 */
#ifndef I2C_DISABLE_BUFFER_API
/* Пакетная работа с 8-битными адресами регистров */
uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len);
uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len);

/* Raw-передача данных без регистрового адреса (DAC7571, потоковый I/O) */
uint8_t i2c_write_raw(uint8_t dev_addr, const uint8_t *p_buf, uint16_t len);
uint8_t i2c_read_raw(uint8_t dev_addr, uint8_t *p_buf, uint16_t len);

/* Пакетная работа с 16-битными адресами памяти/регистров (EEPROM 24LC32..24LC1025, датчики) */
uint8_t i2c_write_buffer16(uint8_t dev_addr, uint16_t reg_addr, const uint8_t *p_buf, uint16_t len);
uint8_t i2c_read_buffer16(uint8_t dev_addr, uint16_t reg_addr, uint8_t *p_buf, uint16_t len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* I2C_H */
