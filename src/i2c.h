#ifndef I2C_H
#define I2C_H

/*
 * i2c.h — Универсальный отказоустойчивый драйвер I2C1 для CH32V003 — Версия 5.4.3
 *
 * Применение: APDS-9960, DAC7571, EEPROM (24LCxx), OLED (SSD1306) и др.
 * Пин-конфигурация: PC1 — SDA, PC2 — SCL (AF_OD, Open-Drain).
 */

#include "ch32v00x.h"
#include <stdint.h>

/* Единый таймаут для всех операций */
#define I2C_TIMEOUT             100000
#define I2C_STRETCH_TIMEOUT     1000        /* Максимальное время ожидания отпускания SCL ведомым */

/* Пауза между STOP и следующим START при сканировании, мкс */
#define I2C_INTER_FRAME_DELAY_US  50

/* Направление передачи */
#define I2C_DIR_TX  0   /* Master Transmitter */
#define I2C_DIR_RX  1   /* Master Receiver */

/* Коды возврата */
#define I2C_OK          0   /* Успешно / Подтверждено (ACK) */
#define I2C_NACK        1   /* Не подтверждено (NACK) или ошибка/таймаут */
#define I2C_ERR_CLK     3   /* Некорректная частота тактирования (PCLK1 вне 2..48 МГц) или i2c_init(0) */

/* Основное API управления шиной */
uint8_t i2c_init(uint32_t bound);                          /* Инициализация I2C1. Возвращает I2C_OK или I2C_ERR_CLK */
void i2c_deinit(void);                                    /* Отключение I2C1 */
uint8_t i2c_wait_bus_free(void);                          /* Ожидание освобождения шины с авто-recovery */
uint8_t i2c_start(void);                                  /* Генерация START с проверкой BUSY */
uint8_t i2c_repeated_start(void);                         /* Генерация Повторного СТАРТа (Repeated START) */
uint8_t i2c_stop(void);                                      /* Генерация STOP с контролем таймаута */

/* Проверка адреса для сканера */
uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2);

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
    if (i2c_send_byte(data) != I2C_OK) {
        return I2C_NACK;
    }
    return i2c_wait_ack();
}

/* Высокоуровневое симметричное API для работы с регистрами устройств (APDS-9960 и др.) */
uint8_t i2c_write_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);
uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value);
uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len);
uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len);

#endif /* I2C_H */