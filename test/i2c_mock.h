/*
 * test/i2c_mock.h — API программной модели периферии I2C1 и ведомого устройства.
 *
 * Модель реагирует на каждое обращение драйвера к регистрам (перехват описан
 * в test/mock/ch32v00x.h), поэтому src/i2c.c тестируется без изменений и без
 * железа.
 */

#ifndef I2C_MOCK_H
#define I2C_MOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Полный сброс модели: регистры, шина, ведомый, счётчики, инъекции отказов. */
void mock_reset(void);

/* --- Конфигурация ведомого --- */

/* Ведомый по адресу addr подтверждает адресную фазу и отдаёт tx_len байт. */
void mock_slave_present(uint8_t addr, const uint8_t *tx, int tx_len);

/* Ведомого нет: адресная фаза получает NACK (флаг AF). */
void mock_slave_absent(void);

/* Ведомый отвечает NACK на data-байт с индексом idx (0 = первый после адреса). */
void mock_slave_nack_data_at(int idx);

/* --- Инъекция отказов --- */

/* Адресная фаза следующих `times` транзакций поднимает flag (BERR/ARLO). */
void mock_inject_addr_fault(uint16_t star1_flag, int times);

/* Шина «замерла»: флаги не появляются, драйвер обязан выйти по таймауту. */
void mock_freeze(int on);

/* Флаг BUSY не снимается: проверка таймаута ожидания свободной шины. */
void mock_stuck_busy(int on);

/* Ведомый удерживает линии в низком уровне (проверка GPIO-recovery). */
void mock_lines_stuck(int sda_low, int scl_low);

/* --- Наблюдаемое состояние --- */

int mock_recovery_count(void);   /* запусков GPIO-восстановления шины */
int mock_swrst_count(void);      /* выполненных SWRST */
int mock_start_count(void);      /* выданных START-условий */
int mock_stop_count(void);       /* выданных STOP-условий */
int mock_scl_pulses(void);       /* тактов SCL, сгенерированных силами GPIO */

/* Данные, принятые ведомым (мастер → ведомый). */
int mock_slave_rx_len(void);
const uint8_t *mock_slave_rx(void);

/* Подтверждения, выданные мастером на принятые байты: 1 = ACK, 0 = NACK.
 * По RM последний байт чтения обязан быть NACK. */
int mock_ack_log_len(void);
const uint8_t *mock_ack_log(void);

/* --- Снимки состояния в ключевых точках последовательности приёма --- */

/* Значение CTLR1 в момент сброса флага ADDR (проверка ACK/POS по RM). */
uint16_t mock_ctlr1_at_addr_clear(void);

/* Значение, записанное в CTLR1 первой записью сразу после сброса ADDR. */
uint16_t mock_ctlr1_after_addr_clear(void);

/* Индекс байта (0-based), на котором был запрограммирован STOP; -1 — не было. */
int mock_stop_before_read_index(void);

/* Значение CTLR1 перед чтением последнего байта (ожидается ACK=0). */
uint16_t mock_ctlr1_before_last_read(void);

/* Сколько байт прочитано из DATAR в режиме приёма. */
int mock_rx_read_count(void);

/* --- Критические секции --- */

/* Глубина запрета прерываний в момент сброса ADDR и в момент первой записи
 * в CTLR1 после него. -1 = событие не наблюдалось. */
int mock_irq_depth_at_addr_clear(void);
int mock_irq_depth_at_post_addr_write(void);
int mock_irq_balanced(void);      /* 1, если запреты и разрешения сбалансированы */

/* --- Текущие регистры глазами теста --- */

uint16_t mock_ctlr1(void);
uint16_t mock_ctlr2(void);
uint16_t mock_ckcfgr(void);
uint16_t mock_oaddr1(void);
uint32_t mock_gpioc_cfglr(void);
uint32_t mock_rcc_apb1(void);

#ifdef __cplusplus
}
#endif

#endif /* I2C_MOCK_H */
