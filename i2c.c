/*
 * i2c.c — Универсальный отказоустойчивый драйвер I2C1 для CH32V003 — Версия 5 (Финал)
 */

#include "i2c.h"
#include "ch32v00x.h"

/* RCC: включение тактирования периферии */
#define RCC_APB2PCENR_IOPCEN  (1 << 4)   /* GPIOC clock enable */
#define RCC_APB1PCENR_I2C1EN  (1 << 21)  /* I2C1 clock enable */

/* GPIO: AF_OD (Alternate Function Open-Drain) + 50MHz */
#define GPIO_CFG_AF_OD_50M  0x0F

/* Конфигурация выходов для режима восстановления (General Purpose Open-Drain, 2MHz) */
#define GPIO_CFG_OUT_OD_2M  0x06

/* Маски для CKCFGR */
#define CKCFGR_FS_Set    ((uint16_t)0x8000)  /* Bit 15: F/S (0=Standard, 1=Fast) */
#define CKCFGR_CCR_Set   ((uint16_t)0x0FFF)  /* Bits 11:0: Clock Control Register */

/* OADDR1: бит 14 должен быть установлен в 1 согласно требованиям Synopsys/WCH */
#define OADDR1_REQUIRED_BIT14   (1 << 14)

/* Константа полупериода такта SCL при аппаратном восстановлении шины (для nop-генератора) */
#define I2C_SCL_PULSE_DELAY    80

/* Лимит последовательных КРИТИЧЕСКИХ ХАРДВЕРНЫХ ошибок перед восстановлением шины */
#define MAX_ERROR_COUNT        5

/* Глобальные статические переменные драйвера */
static uint32_t i2c_speed = 100000;
static uint8_t consecutive_errors = 0;

/* Прототип локальной функции восстановления шины */
static void i2c_bus_recovery(void);

/**
 * @brief Компактная предсказуемая задержка на NOP-инструкциях для GPIO recovery.
 */
static inline void i2c_delay(void) {
    uint16_t i = I2C_SCL_PULSE_DELAY;
    while (i--) {
        __asm volatile("nop");
    }
}

/**
 * @brief Фиксация аппаратных критических ошибок (ARLO, BERR). Исключает программные таймауты.
 */
static inline void handle_critical_error(void) {
    consecutive_errors++;
    if (consecutive_errors >= MAX_ERROR_COUNT) {
        i2c_bus_recovery();
    }
}

/**
 * @brief Вспомогательная функция полной конфигурации регистров тактирования I2C.
 */
static void i2c_configure_registers(void) {
    I2C1->CTLR2 = APB1_FREQ_MHZ;

    uint16_t ccr_val;
    uint16_t fs_bit = 0;

    if (i2c_speed <= 100000) {
        /* Standard Mode: CCR = PCLK1 / (2 × FI2C) */
        ccr_val = PCLK1_HZ / (i2c_speed * 2);
        if (ccr_val < 4) ccr_val = 4;
        I2C1->TRISE = TRISE_STD;
    } else {
        /* Fast Mode (Duty=2): CCR = PCLK1 / (3 × FI2C) */
        ccr_val = PCLK1_HZ / (i2c_speed * 3);
        if (ccr_val == 0) ccr_val = 1;
        fs_bit = CKCFGR_FS_Set;
        I2C1->TRISE = TRISE_FAST;
    }
    
    if (ccr_val > CKCFGR_CCR_Set) {
        ccr_val = CKCFGR_CCR_Set;
    }
    I2C1->CKCFGR = fs_bit | ccr_val;
    I2C1->OADDR1 = OADDR1_REQUIRED_BIT14;
}

/**
 * @brief Локальное аппаратное восстановление шины I2C (Clock Recovery)
 */
static void i2c_bus_recovery(void) {
    /* 1. Отключаем периферию I2C */
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB2PCENR |= RCC_APB2PCENR_IOPCEN;

    /* 2. Переводим SCL (PC2) и SDA (PC1) в режим обычного выхода Open-Drain 2MHz */
    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
    GPIOC->CFGLR |=  (GPIO_CFG_OUT_OD_2M << 4) | (GPIO_CFG_OUT_OD_2M << 8);

    /* 3. Генерируем до 16 тактов SCL с контролем Clock Stretching */
    for (uint8_t i = 0; i < 16; i++) {
        GPIOC->BCR = (1 << 2); /* SCL LOW */
        i2c_delay();

        GPIOC->BSHR = (1 << 2); /* SCL HIGH */
        
        uint32_t stretch = I2C_STRETCH_TIMEOUT;
        while (!(GPIOC->INDR & (1 << 2)) && --stretch);
        
        i2c_delay();
        if (GPIOC->INDR & (1 << 1)) break; /* Если SDA отпущен в HIGH — слейв сдался */
    }

    /* 4. Формирование STOP-условия силами GPIO в режиме Open-Drain */
    GPIOC->BCR = (1 << 2); /* SCL LOW */
    i2c_delay();
    GPIOC->BCR = (1 << 1); /* SDA LOW */
    i2c_delay();
    
    GPIOC->BSHR = (1 << 2); /* SCL HIGH */
    uint32_t stop_stretch = I2C_STRETCH_TIMEOUT;
    while (!(GPIOC->INDR & (1 << 2)) && --stop_stretch);
    i2c_delay();
    
    GPIOC->BSHR = (1 << 1); /* SDA HIGH */
    i2c_delay();

    /* 5. Выполняем Software Reset (SWRST) очищенного блока I2C */
    I2C1->CTLR1 |= (1 << 15);  /* SWRST = 1 */
    I2C1->CTLR1 &= ~(1 << 15); /* SWRST = 0 */

    /* 6. Возвращаем GPIO обратно в режим альтернативной функции Open-Drain (AF_OD) */
    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
    GPIOC->CFGLR |=  (GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8);

    /* 7. Полное восстановление конфигурационных регистров I2C */
    i2c_configure_registers();

    /* 8. Включаем I2C обратно + ACK */
    I2C1->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;

    /* Атомарный безопасный сброс флагов ошибок прямой записью инвертированной маски */
    I2C1->STAR1 = (uint16_t)~(I2C_STAR1_AF | I2C_STAR1_ARLO | I2C_STAR1_BERR);
    
    consecutive_errors = 0;
}

/**
 * @brief Инициализация I2C1
 */
void i2c_init(uint32_t bound) {
    i2c_speed = bound;

    RCC->APB2PCENR |= RCC_APB2PCENR_IOPCEN;
    RCC->APB1PCENR |= RCC_APB1PCENR_I2C1EN;

    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
    GPIOC->CFGLR |=  (GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8);

    I2C1->CTLR1 |= (1 << 15);  /* SWRST = 1 */
    I2C1->CTLR1 &= ~(1 << 15); /* SWRST = 0 */

    i2c_configure_registers();

    I2C1->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;
    consecutive_errors = 0;
}

/**
 * @brief Ожидание освобождения шины I2C (флаг BUSY=0) с мгновенным вызовом recovery при таймауте
 */
uint8_t i2c_wait_bus_free(void) {
    uint32_t timeout = I2C_TIMEOUT;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        if (--timeout == 0) {
            i2c_bus_recovery(); /* Мгновенный сброс, зависание недопустимо */
            return I2C_NACK;
        }
    }
    return I2C_OK;
}

/**
 * @brief Генерация START условия на шине I2C
 */
uint8_t i2c_start(void) {
    if (i2c_wait_bus_free() != I2C_OK) {
        return I2C_NACK;
    }
    
    I2C1->CTLR1 |= I2C_CTLR1_START;
    
    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & I2C_STAR1_SB)) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            I2C1->CTLR1 &= ~I2C_CTLR1_START;
            handle_critical_error();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            I2C1->CTLR1 &= ~I2C_CTLR1_START;
            i2c_bus_recovery(); /* Программный таймаут -> жесткий сброс шины */
            return I2C_NACK;
        }
    }
    return I2C_OK;
}

/**
 * @brief Генерация Повторного СТАРТа (Repeated START)
 */
uint8_t i2c_repeated_start(void) {
    I2C1->CTLR1 |= I2C_CTLR1_START;
    
    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & I2C_STAR1_SB)) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            I2C1->CTLR1 &= ~I2C_CTLR1_START;
            handle_critical_error();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            I2C1->CTLR1 &= ~I2C_CTLR1_START;
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }
    return I2C_OK;
}

/**
 * @brief Генерация STOP условия с собственным таймаутом и мгновенным recovery
 */
void i2c_stop(void) {
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    
    uint32_t timeout = I2C_TIMEOUT;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
        }
        if (--timeout == 0) {
            i2c_bus_recovery();
            return;
        }
    }
}

/**
 * @brief Отправка 7-битного адреса ведомого
 */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction) {
    I2C1->DATAR = (addr << 1) | (direction & 0x01);
    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & I2C_STAR1_ADDR)) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            i2c_stop();
            handle_critical_error();
            return I2C_NACK;
        }
        if (star1 & I2C_STAR1_AF) {
            I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
            i2c_stop();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            i2c_stop();
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }
    (void)I2C1->STAR1;
    (void)I2C1->STAR2;
    return I2C_OK;
}

/**
 * @brief Отправка одного байта данных (Ожидает только TXE!)
 */
uint8_t i2c_send_byte(uint8_t data) {
    I2C1->DATAR = data;
    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & I2C_STAR1_TXE)) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            i2c_stop();
            handle_critical_error();
            return I2C_NACK;
        }
        if (star1 & I2C_STAR1_AF) {
            I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
            i2c_stop();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            i2c_stop();
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }
    return I2C_OK;
}

/**
 * @brief Ожидание подтверждения приёма байта ведомым (ACK)
 */
uint8_t i2c_wait_ack(void) {
    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & (I2C_STAR1_BTF | I2C_STAR1_AF))) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            i2c_stop();
            handle_critical_error();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            i2c_stop();
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
        i2c_stop();
        return I2C_NACK;
    }
    
    consecutive_errors = 0; /* Полный успех сбрасывает счетчик ошибок */
    return I2C_OK;
}

/**
 * @brief Запись в одиночный 8-битный регистр устройства
 */
uint8_t i2c_write_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t value) {
    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    
    if (i2c_write_byte(reg_addr) != I2C_OK) return I2C_NACK;
    if (i2c_write_byte(value) != I2C_OK) return I2C_NACK;
    
    i2c_stop();
    return I2C_OK;
}

/**
 * @brief Чтение одиночного 8-битного регистра устройств с принудительной защитой ACK при таймауте
 */
uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value) {
    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    if (i2c_write_byte(reg_addr) != I2C_OK) return I2C_NACK;
    
    if (i2c_repeated_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_RX) != I2C_OK) return I2C_NACK;
    
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    
    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & I2C_STAR1_RXNE)) {
        if (--timeout == 0) {
            I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Восстанавливаем ACK перед выходом! */
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }
    
    *p_value = (uint8_t)I2C1->DATAR;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    return I2C_OK;
}

/**
 * @brief Пакетная последовательная запись буфера в целевой регистр устройства (Симметрия API)
 */
uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len) {
    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    
    /* Отправляем адрес начального регистра */
    if (i2c_write_byte(reg_addr) != I2C_OK) return I2C_NACK;
    
    /* Потоковая передача массива данных с непрерывным побайтовым контролем ACK */
    for (uint16_t i = 0; i < len; i++) {
        if (i2c_write_byte(p_buf[i]) != I2C_OK) {
            return I2C_NACK; /* Прерывание транзакции при NACK */
        }
    }
    
    i2c_stop();
    return I2C_OK;
}

/**
 * @brief Пакетное последовательное чтение буфера из регистра (Полная защита ACK во всех ветках)
 */
uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len) {
    if (len == 0) return I2C_OK;

    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    if (i2c_write_byte(reg_addr) != I2C_OK) return I2C_NACK;
    
    if (i2c_repeated_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_RX) != I2C_OK) return I2C_NACK;

    I2C1->CTLR1 |= I2C_CTLR1_ACK;

    if (len == 1) {
        I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        
        uint32_t timeout = I2C_TIMEOUT;
        while (!(I2C1->STAR1 & I2C_STAR1_RXNE)) {
            if (--timeout == 0) {
                I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Восстановление */
                i2c_bus_recovery();
                return I2C_NACK;
            }
        }
        p_buf[0] = (uint8_t)I2C1->DATAR;
    } 
    else if (len == 2) {
        uint32_t timeout = I2C_TIMEOUT;
        while (!(I2C1->STAR1 & I2C_STAR1_BTF)) {
            if (--timeout == 0) {
                I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Восстановление */
                i2c_bus_recovery();
                return I2C_NACK;
            }
        }
        I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        
        p_buf[0] = (uint8_t)I2C1->DATAR;
        p_buf[1] = (uint8_t)I2C1->DATAR;
    } 
    else {
        /* Для длинных пакетов N >= 3 (например, чтение FIFO APDS-9960) */
        for (uint16_t i = 0; i < len; i++) {
            
            if (i == len - 3) {
                uint32_t timeout = I2C_TIMEOUT;
                while (!(I2C1->STAR1 & I2C_STAR1_BTF)) {
                    if (--timeout == 0) {
                        I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Восстановление */
                        i2c_bus_recovery();
                        return I2C_NACK;
                    }
                }
                
                I2C1->CTLR1 &= ~I2C_CTLR1_ACK; /* Байт N-2 прочитан, ACK выключаем */
                p_buf[i++] = (uint8_t)I2C1->DATAR;
                
                timeout = I2C_TIMEOUT;
                while (!(I2C1->STAR1 & I2C_STAR1_BTF)) {
                    if (--timeout == 0) {
                        I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Восстановление */
                        i2c_bus_recovery();
                        return I2C_NACK;
                    }
                }
                
                I2C1->CTLR1 |= I2C_CTLR1_STOP; /* Выставляем STOP перед последним байтом */
                p_buf[i++] = (uint8_t)I2C1->DATAR;
                
                timeout = I2C_TIMEOUT;
                while (!(I2C1->STAR1 & I2C_STAR1_RXNE)) {
                    if (--timeout == 0) {
                        I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Восстановление */
                        i2c_bus_recovery();
                        return I2C_NACK;
                    }
                }
                
                p_buf[i] = (uint8_t)I2C1->DATAR;
                break;
            }
            
            uint32_t timeout = I2C_TIMEOUT;
            while (!(I2C1->STAR1 & I2C_STAR1_RXNE)) {
                if (--timeout == 0) {
                    I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Восстановление */
                    i2c_bus_recovery();
                    return I2C_NACK;
                }
            }
            p_buf[i] = (uint8_t)I2C1->DATAR;
        }
    }

    I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Финальное гарантированное включение ACK для следующих сессий */
    return I2C_OK;
}

/**
 * @brief Полное отключение I2C1
 */
void i2c_deinit(void) {
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB1PCENR &= ~RCC_APB1PCENR_I2C1EN;
    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
    consecutive_errors = 0;
}