/*
 * i2c.c — Универсальный отказоустойчивый драйвер I2C1 для CH32V003 — Версия 7.0.1
 */

#include "i2c.h"
#include "ch32v00x.h"

/* Маски конфигурации пинов PC1 и PC2 */
#define GPIO_PC1_PC2_MASK       0x00000FF0UL
#define GPIO_PC1_PC2_AF_OD_50M  0x00000FF0UL
#define GPIO_PC1_PC2_OUT_OD_2M  0x00000660UL

/* OADDR1: бит 14 должен быть установлен в 1 согласно требованиям Synopsys/WCH */
#define OADDR1_REQUIRED_BIT14   (1 << 14)

/* Константа полупериода такта SCL при аппаратном восстановлении шины (для nop-генератора) */
#define I2C_SCL_PULSE_DELAY    80

/* Примерное количество тактов ядра на одну итерацию пустого цикла (load/branch/nop) */
#define I2C_NOP_LOOP_CYCLES    4

/* Лимит последовательных КРИТИЧЕСКИХ ХАРДВЕРНЫХ ошибок перед восстановлением шины */
#if !defined(I2C_DISABLE_ERROR_COUNTER)
#define MAX_ERROR_COUNT        2
#endif

/* Отображение статус-кодов (режим legacy сворачивает таймауты и BERR/ARLO в I2C_NACK) */
#if I2C_LEGACY_STATUS
#define I2C_STATUS_TIMEOUT  I2C_NACK
#define I2C_STATUS_BERR     I2C_NACK
#define I2C_STATUS_ARLO     I2C_NACK
#else
#define I2C_STATUS_TIMEOUT  I2C_ERR_TIMEOUT
#define I2C_STATUS_BERR     I2C_ERR_BERR
#define I2C_STATUS_ARLO     I2C_ERR_ARLO
#endif

/* Глобальные статические переменные драйвера */
static uint32_t i2c_speed = 100000;
static uint32_t i2c_timeout_loops = 100000;
#if !defined(I2C_DISABLE_ERROR_COUNTER)
static uint8_t consecutive_errors = 0;
#endif

/* Прототип локальной функции восстановления шины */
#if !defined(I2C_DISABLE_BUS_RECOVERY)
static void i2c_bus_recovery(void);
#endif

/**
 * @brief Установка режима работы пинов PC1 (SDA) и PC2 (SCL)
 */
static void i2c_set_gpio_mode(uint32_t mode) {
    GPIOC->CFGLR = (GPIOC->CFGLR & ~GPIO_PC1_PC2_MASK) | mode;
}

/**
 * @brief Компактная предсказуемая задержка на NOP-инструкциях для GPIO recovery.
 */
static inline void i2c_delay(void) {
    uint32_t i = I2C_SCL_PULSE_DELAY;
    while (i--) {
        __asm volatile("nop");
    }
}

/**
 * @brief Программная задержка в микросекундах на основе частоты шины/ядра.
 * @note Используется внутри драйвера, чтобы не зависеть от внешнего Delay_Init().
 */
static void i2c_usleep(uint32_t us) {
    uint32_t loops = ((I2C1->CTLR2 & 0x3F) * us) / I2C_NOP_LOOP_CYCLES;
    while (loops--) {
        __asm volatile("nop");
    }
}

/**
 * @brief Фиксация аппаратных критических ошибок (ARLO, BERR).
 */
#if !defined(I2C_DISABLE_ERROR_COUNTER) && !defined(I2C_DISABLE_BUS_RECOVERY)
static inline void handle_critical_error(void) {
    consecutive_errors++;
    if (consecutive_errors >= MAX_ERROR_COUNT) {
        i2c_bus_recovery();
    }
}
#endif

/**
 * @brief Обработчик аппаратных ошибок BERR/ARLO с вызовом recovery.
 */
static uint8_t i2c_handle_error(uint16_t star1) {
    uint8_t status = (star1 & I2C_STAR1_ARLO) ? I2C_STATUS_ARLO : I2C_STATUS_BERR;
    I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
#if !defined(I2C_DISABLE_ERROR_COUNTER) && !defined(I2C_DISABLE_BUS_RECOVERY)
    handle_critical_error();
#elif !defined(I2C_DISABLE_BUS_RECOVERY)
    i2c_bus_recovery();
#endif
    return status;
}

/**
 * @brief Обработчик программных таймаутов с вызовом recovery.
 */
static uint8_t i2c_handle_timeout(void) {
#if !defined(I2C_DISABLE_BUS_RECOVERY)
    i2c_bus_recovery();
#endif
    return I2C_STATUS_TIMEOUT;
}

/**
 * @brief Вспомогательная функция конфигурации регистров тактирования I2C
 * @return I2C_OK или I2C_ERR_CLK (некорректный SystemCoreClock, speed > 400kHz или i2c_speed==0)
 */
static uint8_t i2c_configure_registers(void) {
    uint32_t pclk1 = SystemCoreClock;

    /* CH32V003 I2C: SYSCLK must be in [2 MHz, 48 MHz] range, speed in [1, 400000] Hz. */
    if (pclk1 < 2000000UL || pclk1 > 48000000UL || i2c_speed == 0 || i2c_speed > 400000UL) {
        return I2C_ERR_CLK;
    }

    /* Вычисление масштабированного числа итераций таймаута (~16 тактов на итерацию цикла) */
    i2c_timeout_loops = ((pclk1 / 1000UL) * (uint32_t)I2C_TIMEOUT_MS) / 16UL;
    if (i2c_timeout_loops < 100UL) {
        i2c_timeout_loops = 100UL;
    }

    I2C1->CTLR2 = (uint16_t)(pclk1 / 1000000UL);

    uint16_t fs_bit = 0;
    uint32_t divisor = i2c_speed * 2;
    if (i2c_speed > 100000) {
        divisor = i2c_speed * 3;
        fs_bit = I2C_CKCFGR_FS;
    }

    uint32_t ccr_val = pclk1 / divisor;
    uint32_t min_ccr = (fs_bit == 0) ? 4 : 1;
    if (ccr_val < min_ccr) {
        ccr_val = min_ccr;
    } else if (ccr_val > I2C_CKCFGR_CCR) {
        ccr_val = I2C_CKCFGR_CCR;
    }

    I2C1->CKCFGR = fs_bit | (uint16_t)ccr_val;
    I2C1->OADDR1 = OADDR1_REQUIRED_BIT14;

    return I2C_OK;
}

/**
 * @brief Локальное аппаратное восстановление шины I2C (Clock Recovery)
 */
#if !defined(I2C_DISABLE_BUS_RECOVERY)
static void i2c_wait_scl_high(void) {
    uint32_t stretch = I2C_STRETCH_TIMEOUT;
    while (!(GPIOC->INDR & (1 << 2)) && --stretch);
    i2c_delay();
}

static void i2c_bus_recovery(void) {
    /* 1. Отключаем периферию I2C */
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB2PCENR |= RCC_IOPCEN;

    /* 2. Переводим SCL (PC2) и SDA (PC1) в режим Open-Drain 2MHz */
    i2c_set_gpio_mode(GPIO_PC1_PC2_OUT_OD_2M);
    
    /* Инициализируем линии в состояние HIGH (отпущены) сразу после переключения */
    GPIOC->BSHR = (1 << 1) | (1 << 2);
    i2c_delay();

    /* 3. Генерируем до 16 тактов SCL с контролем Clock Stretching */
    for (uint8_t i = 0; i < 16; i++) {
        GPIOC->BSHR = (1 << (2 + 16)); /* SCL LOW */
        i2c_delay();

        GPIOC->BSHR = (1 << 2);        /* SCL HIGH */
        i2c_wait_scl_high();
        
        if (GPIOC->INDR & (1 << 1)) break; /* Если SDA отпущен в HIGH — слейв сдался */
    }

    /* 4. Формирование STOP-условия силами GPIO в режиме Open-Drain */
    GPIOC->BSHR = (1 << (2 + 16)); /* SCL LOW */
    i2c_delay();
    GPIOC->BSHR = (1 << (1 + 16)); /* SDA LOW */
    i2c_delay();
    
    GPIOC->BSHR = (1 << 2);        /* SCL HIGH */
    i2c_wait_scl_high();
    
    GPIOC->BSHR = (1 << 1);        /* SDA HIGH */
    i2c_delay();

    /* 5. Выполняем Software Reset (SWRST) очищенного блока I2C */
    I2C1->CTLR1 |= I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    /* 6. Возвращаем GPIO обратно в режим альтернативной функции Open-Drain (AF_OD) */
    i2c_set_gpio_mode(GPIO_PC1_PC2_AF_OD_50M);

    /* 7. Полное восстановление конфигурационных регистров I2C */
    (void)i2c_configure_registers();

    /* 8. Включаем I2C обратно + ACK, сбрасываем POS */
    I2C1->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;

    /* Атомарный безопасный сброс флагов ошибок прямой записью инвертированной маски */
    I2C1->STAR1 = (uint16_t)~(I2C_STAR1_AF | I2C_STAR1_ARLO | I2C_STAR1_BERR);
    
    /* Ожидание очистки аппаратного флага BUSY цифровым фильтром периферии */
    uint32_t busy_timeout = i2c_timeout_loops;
    while ((I2C1->STAR2 & I2C_STAR2_BUSY) && --busy_timeout);
    
#if !defined(I2C_DISABLE_ERROR_COUNTER)
    consecutive_errors = 0;
#endif
}
#endif /* !I2C_DISABLE_BUS_RECOVERY */

/**
 * @brief Полная и безопасная инициализация I2C1 на CH32V003
 * @return I2C_OK или I2C_ERR_CLK (если SystemCoreClock вне диапазона 2..48 МГц)
 */
uint8_t i2c_init(uint32_t bound) {
    i2c_speed = bound;

    RCC->APB2PCENR |= RCC_IOPCEN;
    RCC->APB1PCENR |= RCC_I2C1EN;

    i2c_set_gpio_mode(GPIO_PC1_PC2_AF_OD_50M);

    I2C1->CTLR1 |= I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    uint8_t cfg_status = i2c_configure_registers();
    if (cfg_status != I2C_OK) {
        return cfg_status;
    }

    I2C1->CTLR1 |= (I2C_CTLR1_PE | I2C_CTLR1_ACK);

    i2c_usleep(I2C_INTER_FRAME_DELAY_US);

    return I2C_OK;
}

/**
 * @brief Общий цикл ожидания снятия флага BUSY с обработкой ошибок и recovery.
 */
static uint8_t i2c_wait_busy_clear(void) {
    uint32_t timeout = i2c_timeout_loops;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            return i2c_handle_error(star1);
        }
        if (--timeout == 0) {
            return i2c_handle_timeout();
        }
    }
    return I2C_OK;
}

/**
 * @brief Ожидание освобождения шины I2C (флаг BUSY=0)
 */
uint8_t i2c_wait_bus_free(void) {
    return i2c_wait_busy_clear();
}

/**
 * @brief Унифицированное ожидание бита флага в STAR1 с контролем ошибок и таймаута
 */
static uint8_t i2c_wait_star1_flag(uint16_t flag) {
    uint32_t timeout = i2c_timeout_loops;
    while (!(I2C1->STAR1 & flag)) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            i2c_stop();
            return i2c_handle_error(star1);
        }
        if (star1 & I2C_STAR1_AF) {
            I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
            i2c_stop();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            i2c_stop();
            return i2c_handle_timeout();
        }
    }
    return I2C_OK;
}

/**
 * @brief Общий цикл ожидания флага SB после запроса START (для i2c_start/i2c_repeated_start)
 */
static uint8_t i2c_wait_start_bit(void) {
    I2C1->CTLR1 |= I2C_CTLR1_START;
    return i2c_wait_star1_flag(I2C_STAR1_SB);
}

/**
 * @brief Генерация START условия на шине I2C
 */
uint8_t i2c_start(void) {
    uint8_t res = i2c_wait_bus_free();
    if (res != I2C_OK) {
        return res;
    }

    return i2c_wait_start_bit();
}

/**
 * @brief Генерация Повторного СТАРТа (Repeated START)
 */
uint8_t i2c_repeated_start(void) {
    return i2c_wait_start_bit();
}

/**
 * @brief Генерация STOP условия с собственным таймаутом
 * @return I2C_OK если шина освободилась, иначе код ошибки
 */
uint8_t i2c_stop(void) {
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return i2c_wait_busy_clear();
}

/**
 * @brief Отправка адреса с автоматическим контролем ACK/NACK
 */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction) {
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
    }
    
    I2C1->DATAR = (uint16_t)((addr << 1) | (direction & 1));

    uint8_t res = i2c_wait_star1_flag(I2C_STAR1_ADDR);
    if (res != I2C_OK) {
        return res;
    }

    (void)I2C1->STAR1;
    (void)I2C1->STAR2;

#if !defined(I2C_DISABLE_ERROR_COUNTER)
    consecutive_errors = 0;
#endif

    return I2C_OK;
}

/**
 * @brief Проверка адреса для сканера
 * @param addr 7-битный адрес
 * @param p_star1 указатель для сохранения STAR1 (можно NULL)
 * @param p_star2 указатель для сохранения STAR2 (можно NULL)
 * @return I2C_OK если устройство ответило ACK, иначе код ошибки / I2C_NACK
 */
#ifndef I2C_DISABLE_SCANNER
uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2) {
    uint8_t res = I2C_NACK;
    if (i2c_start() == I2C_OK) {
        res = i2c_send_addr(addr, I2C_DIR_TX);
        if (p_star1) *p_star1 = I2C1->STAR1;
        if (p_star2) *p_star2 = I2C1->STAR2;
        if (res == I2C_OK) {
            i2c_stop();
        }
    } else {
        if (p_star1) *p_star1 = I2C1->STAR1;
        if (p_star2) *p_star2 = I2C1->STAR2;
    }
    i2c_usleep(I2C_INTER_FRAME_DELAY_US);
    return res;
}
#endif /* I2C_DISABLE_SCANNER */

/**
 * @brief Низкоуровневая отправка одного байта данных
 */
uint8_t i2c_send_byte(uint8_t data) {
    I2C1->DATAR = data;
    return i2c_wait_star1_flag(I2C_STAR1_TXE);
}

/**
 * @brief Ожидание подтверждения приёма байта ведомым (ACK)
 */
uint8_t i2c_wait_ack(void) {
    uint8_t res = i2c_wait_star1_flag(I2C_STAR1_BTF);
#if !defined(I2C_DISABLE_ERROR_COUNTER)
    if (res == I2C_OK) {
        consecutive_errors = 0;
    }
#endif
    return res;
}

/**
 * @brief Хелпер ожидания флага STAR1 в цикле чтения данных с защитой ACK.
 */
static uint8_t i2c_wait_flag_or_recover(uint16_t flag) {
    uint8_t res = i2c_wait_star1_flag(flag);
    if (res != I2C_OK) {
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
    }
    return res;
}

/**
 * @brief Начало транзакции записи в регистр устройства (START + dev_addr TX + reg_addr)
 */
static uint8_t i2c_start_reg_write(uint8_t dev_addr, uint8_t reg_addr) {
    uint8_t res;
    if ((res = i2c_start()) != I2C_OK ||
        (res = i2c_send_addr(dev_addr, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte(reg_addr)) != I2C_OK) {
        return res;
    }
    return I2C_OK;
}

/**
 * @brief Начало транзакции чтения из регистра устройства (START + dev_addr TX + reg_addr + repeated START)
 */
static uint8_t i2c_start_reg_read(uint8_t dev_addr, uint8_t reg_addr) {
    uint8_t res;
    if ((res = i2c_start_reg_write(dev_addr, reg_addr)) != I2C_OK ||
        (res = i2c_repeated_start()) != I2C_OK) {
        return res;
    }
    return I2C_OK;
}

/**
 * @brief Чтение одного байта после выставления START / Repeated START
 */
static uint8_t i2c_read_1byte(uint8_t dev_addr, uint8_t *p_buf) {
    uint8_t res;
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    if ((res = i2c_send_addr(dev_addr, I2C_DIR_RX)) != I2C_OK) {
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        return res;
    }
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    
    if ((res = i2c_wait_flag_or_recover(I2C_STAR1_RXNE)) != I2C_OK) {
        return res;
    }
    *p_buf = (uint8_t)I2C1->DATAR;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    return I2C_OK;
}

/**
 * @brief Запись в одиночный 8-битный регистр устройства
 */
uint8_t i2c_write_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t value) {
    uint8_t res;
    if ((res = i2c_start_reg_write(dev_addr, reg_addr)) != I2C_OK ||
        (res = i2c_write_byte(value)) != I2C_OK) {
        return res;
    }
    return i2c_stop();
}

/**
 * @brief Чтение одиночного 8-битного регистра
 */
uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value) {
    uint8_t res;
    if ((res = i2c_start_reg_read(dev_addr, reg_addr)) != I2C_OK) {
        return res;
    }
    return i2c_read_1byte(dev_addr, p_value);
}

#ifndef I2C_DISABLE_BUFFER_API
/**
 * @brief Внутренний цикл последовательной передачи буфера байтов
 */
static uint8_t i2c_write_bytes(const uint8_t *p_buf, uint16_t len) {
    while (len--) {
        uint8_t res = i2c_write_byte(*p_buf++);
        if (res != I2C_OK) {
            return res;
        }
    }
    return I2C_OK;
}

/**
 * @brief Универсальный приемный движок I2C (len 1, 2, >=3) после выставления START/Repeated START
 * @note Предусловие: len >= 1 (гарантируется публичным API)
 */
static uint8_t i2c_read_bytes_rx(uint8_t dev_addr, uint8_t *p_buf, uint16_t len) {
    if (len == 1) {
        return i2c_read_1byte(dev_addr, p_buf);
    }

    uint8_t res;

    if (len == 2) {
        I2C1->CTLR1 |= (I2C_CTLR1_ACK | I2C_CTLR1_POS);

        if ((res = i2c_send_addr(dev_addr, I2C_DIR_RX)) != I2C_OK) {
            I2C1->CTLR1 &= ~I2C_CTLR1_POS;
            return res;
        }

        I2C1->CTLR1 &= ~I2C_CTLR1_ACK;

        if ((res = i2c_wait_flag_or_recover(I2C_STAR1_BTF)) != I2C_OK) {
            I2C1->CTLR1 &= ~I2C_CTLR1_POS;
            return res;
        }
        I2C1->CTLR1 |= I2C_CTLR1_STOP;

        p_buf[0] = (uint8_t)I2C1->DATAR;
        p_buf[1] = (uint8_t)I2C1->DATAR;

        I2C1->CTLR1 &= ~I2C_CTLR1_POS;
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        return I2C_OK;
    }

    /* len >= 3 */
    I2C1->CTLR1 |= I2C_CTLR1_ACK;

    if ((res = i2c_send_addr(dev_addr, I2C_DIR_RX)) != I2C_OK) {
        return res;
    }

    while (len > 3) {
        if ((res = i2c_wait_flag_or_recover(I2C_STAR1_RXNE)) != I2C_OK) {
            return res;
        }
        *p_buf++ = (uint8_t)I2C1->DATAR;
        len--;
    }

    if ((res = i2c_wait_flag_or_recover(I2C_STAR1_BTF)) != I2C_OK) {
        return res;
    }
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    *p_buf++ = (uint8_t)I2C1->DATAR;

    if ((res = i2c_wait_flag_or_recover(I2C_STAR1_BTF)) != I2C_OK) {
        return res;
    }
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    *p_buf++ = (uint8_t)I2C1->DATAR;

    if ((res = i2c_wait_flag_or_recover(I2C_STAR1_RXNE)) != I2C_OK) {
        return res;
    }
    *p_buf = (uint8_t)I2C1->DATAR;

    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    return I2C_OK;
}

/**
 * @brief Пакетная последовательная запись буфера
 */
uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len) {
    uint8_t res;
    if ((res = i2c_start_reg_write(dev_addr, reg_addr)) != I2C_OK ||
        (res = i2c_write_bytes(p_buf, len)) != I2C_OK) {
        return res;
    }
    return i2c_stop();
}

/**
 * @brief Пакетное последовательное чтение буфера из регистра
 */
uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len) {
    if (len == 0) return I2C_OK; /* Нулевая длина: шина не задействуется */

    uint8_t res;
    if ((res = i2c_start_reg_read(dev_addr, reg_addr)) != I2C_OK) {
        return res;
    }
    return i2c_read_bytes_rx(dev_addr, p_buf, len);
}

/**
 * @brief Raw-запись буфера без регистрового адреса (DAC7571, потоковый вывод)
 */
uint8_t i2c_write_raw(uint8_t dev_addr, const uint8_t *p_buf, uint16_t len) {
    uint8_t res;
    if ((res = i2c_start()) != I2C_OK ||
        (res = i2c_send_addr(dev_addr, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_bytes(p_buf, len)) != I2C_OK) {
        return res;
    }
    return i2c_stop();
}

/**
 * @brief Raw-чтение буфера без предварительной записи регистра
 */
uint8_t i2c_read_raw(uint8_t dev_addr, uint8_t *p_buf, uint16_t len) {
    if (len == 0) return I2C_OK; /* Нулевая длина: шина не задействуется */

    uint8_t res;
    if ((res = i2c_start()) != I2C_OK) {
        return res;
    }
    return i2c_read_bytes_rx(dev_addr, p_buf, len);
}

/**
 * @brief Пакетная запись буфера с 16-битным адресом памяти/регистра (EEPROM 24LC32..24LC1025)
 */
uint8_t i2c_write_buffer16(uint8_t dev_addr, uint16_t reg_addr, const uint8_t *p_buf, uint16_t len) {
    uint8_t res;
    if ((res = i2c_start()) != I2C_OK ||
        (res = i2c_send_addr(dev_addr, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte((uint8_t)(reg_addr >> 8))) != I2C_OK ||
        (res = i2c_write_byte((uint8_t)(reg_addr & 0xFF))) != I2C_OK ||
        (res = i2c_write_bytes(p_buf, len)) != I2C_OK) {
        return res;
    }
    return i2c_stop();
}

/**
 * @brief Пакетное чтение буфера с 16-битным адресом памяти/регистра (EEPROM 24LC32..24LC1025)
 */
uint8_t i2c_read_buffer16(uint8_t dev_addr, uint16_t reg_addr, uint8_t *p_buf, uint16_t len) {
    if (len == 0) return I2C_OK; /* Нулевая длина: шина не задействуется */

    uint8_t res;
    if ((res = i2c_start()) != I2C_OK ||
        (res = i2c_send_addr(dev_addr, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte((uint8_t)(reg_addr >> 8))) != I2C_OK ||
        (res = i2c_write_byte((uint8_t)(reg_addr & 0xFF))) != I2C_OK ||
        (res = i2c_repeated_start()) != I2C_OK) {
        return res;
    }
    return i2c_read_bytes_rx(dev_addr, p_buf, len);
}
#endif /* I2C_DISABLE_BUFFER_API */

/**
 * @brief Полное отключение I2C1
 */
void i2c_deinit(void) {
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB1PCENR &= ~RCC_I2C1EN;
    i2c_set_gpio_mode(0);
#if !defined(I2C_DISABLE_ERROR_COUNTER)
    consecutive_errors = 0;
#endif
}
