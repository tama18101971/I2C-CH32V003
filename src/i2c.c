/*
 * i2c.c — Универсальный отказоустойчивый драйвер I2C1 для CH32V003 — Версия 5.4.3
 */

#include "i2c.h"
#include "ch32v00x.h"

/* RCC: включение тактирования периферии */
#define RCC_APB2PCENR_IOPCEN  (1 << 4)   /* GPIOC clock enable */
#define RCC_APB1PCENR_I2C1EN  (1 << 21)  /* I2C1 clock enable */

/* GPIO: AF_OD (Alternate Function Open-Drain) + 50MHz */
#define GPIO_CFG_AF_OD_50M   0x0F

/* Конфигурация выходов для режима восстановления (General Purpose Open-Drain, 2MHz) */
#define GPIO_CFG_OUT_OD_2M   0x06

/* Маски для CKCFGR */
#define CKCFGR_FS_Set    ((uint16_t)0x8000)  /* Bit 15: F/S (0=Standard, 1=Fast) */
#define CKCFGR_CCR_Set   ((uint16_t)0x0FFF)  /* Bits 11:0: Clock Control Register */

/* OADDR1: бит 14 должен быть установлен в 1 согласно требованиям Synopsys/WCH */
#define OADDR1_REQUIRED_BIT14   (1 << 14)

/* Константа полупериода такта SCL при аппаратном восстановлении шины (для nop-генератора) */
#define I2C_SCL_PULSE_DELAY    80

/* Примерное количество тактов ядра на одну итерацию пустого цикла (load/branch/nop) */
#define I2C_NOP_LOOP_CYCLES    4

/* Лимит последовательных КРИТИЧЕСКИХ ХАРДВЕРНЫХ ошибок перед восстановлением шины */
#define MAX_ERROR_COUNT        2

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
 * @brief Программная задержка в микросекундах на основе SystemCoreClock.
 * @note Используется внутри драйвера, чтобы не зависеть от внешнего Delay_Init().
 */
static inline void i2c_usleep(uint32_t us) {
    uint32_t ticks = (SystemCoreClock / 1000000UL) * us;
    uint32_t loops = (ticks < I2C_NOP_LOOP_CYCLES) ? 1 : (ticks / I2C_NOP_LOOP_CYCLES);
    for (volatile uint32_t i = 0; i < loops; i++) {
        __asm volatile("nop");
    }
}

/**
 * @brief Фиксация аппаратных критических ошибок (ARLO, BERR).
 */
static inline void handle_critical_error(void) {
    consecutive_errors++;
    if (consecutive_errors >= MAX_ERROR_COUNT) {
        i2c_bus_recovery();
    }
}

/**
 * @brief Вспомогательная функция конфигурации регистров тактирования I2C
 * @note  CH32V003 не имеет делителя APB1 (PPRE1 отсутствует в CFGR0),
 *        поэтому PCLK1 == HCLK == SystemCoreClock. Проверка ниже —
 *        страховка от некорректной инициализации тактирования.
 * @return I2C_OK или I2C_ERR_CLK (некорректный SystemCoreClock или i2c_speed==0)
 */
static uint8_t i2c_configure_registers(void) {
    uint32_t pclk1 = SystemCoreClock;

    /* CH32V003 I2C: SYSCLK must be in [2 MHz, 48 MHz] range.
     * Below 2 MHz the CCR cannot be programmed; above 48 MHz exceeds
     * the peripheral specification.  If you see this, the clock tree
     * was not configured before calling i2c_init(). */
    if (pclk1 < 2000000UL || pclk1 > 48000000UL) {
        return I2C_ERR_CLK;
    }

    /* Защита от деления на ноль: i2c_speed==0 (например, i2c_init(0))
     * привёл бы к делению на ноль ниже. Проверяется здесь, а не только
     * в i2c_init(), чтобы защитить и повторный вызов из i2c_bus_recovery(). */
    if (i2c_speed == 0) {
        return I2C_ERR_CLK;
    }

    uint32_t freq_mhz = pclk1 / 1000000UL;
    I2C1->CTLR2 = freq_mhz;

    uint16_t ccr_val;
    uint16_t fs_bit = 0;

    if (i2c_speed <= 100000) {
        ccr_val = pclk1 / (i2c_speed * 2);
        if (ccr_val < 4) ccr_val = 4;
    } else {
        ccr_val = pclk1 / (i2c_speed * 3);
        if (ccr_val == 0) ccr_val = 1;
        fs_bit = CKCFGR_FS_Set;
    }
    
    if (ccr_val > CKCFGR_CCR_Set) {
        ccr_val = CKCFGR_CCR_Set;
    }
    I2C1->CKCFGR = fs_bit | ccr_val;
    I2C1->OADDR1 = OADDR1_REQUIRED_BIT14;

    return I2C_OK;
}

/**
 * @brief Локальное аппаратное восстановление шины I2C (Clock Recovery)
 */
static void i2c_bus_recovery(void) {
    /* 1. Отключаем периферию I2C */
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB2PCENR |= RCC_APB2PCENR_IOPCEN;

    /* 2. Переводим SCL (PC2) и SDA (PC1) в режим обычного выхода Open-Drain 2MHz */
    GPIOC->CFGLR &= ~((GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8));
    GPIOC->CFGLR |=  (GPIO_CFG_OUT_OD_2M << 4) | (GPIO_CFG_OUT_OD_2M << 8);
    
    /* Инициализируем линии в состояние HIGH (отпущены) сразу после переключения */
    GPIOC->BSHR = (1 << 1) | (1 << 2);
    i2c_delay();

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
    I2C1->CTLR1 |= I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    /* 6. Возвращаем GPIO обратно в режим альтернативной функции Open-Drain (AF_OD) */
    GPIOC->CFGLR &= ~((GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8));
    GPIOC->CFGLR |=  (GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8);

    /* 7. Полное восстановление конфигурационных регистров I2C */
    uint8_t cfg_status = i2c_configure_registers();

    /* 8. Включаем I2C обратно + ACK, сбрасываем POS на всякий случай */
    I2C1->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;
    I2C1->CTLR1 &= ~I2C_CTLR1_POS;
    
    (void)cfg_status; /* keep for future use */

    /* Атомарный безопасный сброс флагов ошибок прямой записью инвертированной маски */
    I2C1->STAR1 = (uint16_t)~(I2C_STAR1_AF | I2C_STAR1_ARLO | I2C_STAR1_BERR);
    
    /* Ожидание очистки аппаратного флага BUSY цифровым фильтром периферии */
    uint32_t busy_timeout = I2C_TIMEOUT;
    while ((I2C1->STAR2 & I2C_STAR2_BUSY) && --busy_timeout);
    
    consecutive_errors = 0;
}

/**
 * @brief Полная и безопасная инициализация I2C1 на CH32V003
 * @return I2C_OK или I2C_ERR_CLK (если SystemCoreClock вне диапазона 2..48 МГц)
 */
uint8_t i2c_init(uint32_t bound) {
    i2c_speed = bound;

    // 1. Включаем тактирование Порта C и самого блока I2C1
    RCC->APB2PCENR |= RCC_APB2PCENR_IOPCEN;
    RCC->APB1PCENR |= RCC_APB1PCENR_I2C1EN;

    // 2. Конфигурируем PC1 (SDA) и PC2 (SCL) как Alternate Function Open-Drain (50MHz)
    GPIOC->CFGLR &= ~((GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8));
    GPIOC->CFGLR |=  ((GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8));

    // 3. Сбрасываем и очищаем автомат I2C через SWRST
    I2C1->CTLR1 |= I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    // 4. Расчет и запись делителей скорости
    uint8_t cfg_status = i2c_configure_registers();
    if (cfg_status != I2C_OK) {
        return cfg_status;
    }

    // 5. Окончательно включаем периферию I2C + авто-ACK
    I2C1->CTLR1 |= (I2C_CTLR1_PE | I2C_CTLR1_ACK);

    // 6. Короткая пауза, пока периферия выйдет в стабильное состояние
    i2c_usleep(I2C_INTER_FRAME_DELAY_US);

    return I2C_OK;
}

/**
 * @brief Ожидание освобождения шины I2C (флаг BUSY=0)
 */
uint8_t i2c_wait_bus_free(void) {
    uint32_t timeout = I2C_TIMEOUT;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            i2c_bus_recovery();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }
    return I2C_OK;
}

/**
 * @brief Общий цикл ожидания флага SB после запроса START (для i2c_start/i2c_repeated_start)
 */
static uint8_t i2c_wait_start_bit(void) {
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
 * @brief Генерация START условия на шине I2C
 */
uint8_t i2c_start(void) {
    if (i2c_wait_bus_free() != I2C_OK) {
        return I2C_NACK;
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
 * @return I2C_OK если шина освободилась, I2C_NACK при таймауте/восстановлении
 */
uint8_t i2c_stop(void) {
    I2C1->CTLR1 |= I2C_CTLR1_STOP;

    uint32_t timeout = I2C_TIMEOUT;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            /* Как и в i2c_wait_bus_free() (структурно идентичный цикл ожидания
             * BUSY), аппаратная ошибка требует немедленного восстановления,
             * а не ожидания полного таймаута. */
            i2c_bus_recovery();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }

    /* Небольшая пауза между транзакциями — важно при сканировании, когда
     * следующий START выдаётся немедленно после STOP. */
    i2c_usleep(I2C_INTER_FRAME_DELAY_US);
    return I2C_OK;
}

/**
 * @brief Отправка адреса с автоматическим контролем ACK/NACK
 * @note При NACK адреса (AF) или таймауте функция ОБЯЗАТЕЛЬНО освобождает шину
 * (STOP/recovery) — иначе шина останется в состоянии BUSY до следующего i2c_start().
 */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction) {
    /* Проверяем и сбрасываем флаг AF перед отправкой адреса */
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
    }
    
    I2C1->DATAR = (addr << 1) | direction;

    uint32_t timeout = I2C_TIMEOUT; /* Заменено фиксированное число на единую константу */
    while (!(I2C1->STAR1 & (I2C_STAR1_ADDR | I2C_STAR1_AF))) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            i2c_stop();
            handle_critical_error();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            /* Аппаратное зависание на этапе адреса — шину нужно восстановить сразу,
             * а не откладывать это до следующего i2c_start(). */
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }

    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF; 
        /* Ведомый не подтвердил адрес — обязательно генерируем STOP,
         * иначе шина останется занятой (BUSY) до следующей транзакции. */
        i2c_stop();
        return I2C_NACK;
    }

    (void)I2C1->STAR1;
    (void)I2C1->STAR2;

    return I2C_OK;
}

/**
 * @brief Проверка адреса с сохранением STAR1/STAR2 для диагностики.
 * @param addr 7-битный адрес
 * @param p_star1 указатель для сохранения STAR1 (можно NULL)
 * @param p_star2 указатель для сохранения STAR2 (можно NULL)
 * @return I2C_OK если устройство ответило ACK, иначе I2C_NACK
 * @note При NACK (AF) шина освобождается одним STOP, без двойного вызова.
 */
uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2) {
    if (i2c_start() != I2C_OK) {
        if (p_star1) *p_star1 = I2C1->STAR1;
        if (p_star2) *p_star2 = I2C1->STAR2;
        return I2C_NACK;
    }

    I2C1->DATAR = (addr << 1) | I2C_DIR_TX;

    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & (I2C_STAR1_ADDR | I2C_STAR1_AF))) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            i2c_stop();
            handle_critical_error();
            if (p_star1) *p_star1 = I2C1->STAR1;
            if (p_star2) *p_star2 = I2C1->STAR2;
            return I2C_NACK;
        }
        if (--timeout == 0) {
            i2c_bus_recovery();
            if (p_star1) *p_star1 = I2C1->STAR1;
            if (p_star2) *p_star2 = I2C1->STAR2;
            return I2C_NACK;
        }
    }

    uint8_t res = (I2C1->STAR1 & I2C_STAR1_ADDR) ? I2C_OK : I2C_NACK;
    if (p_star1) *p_star1 = I2C1->STAR1;
    if (p_star2) *p_star2 = I2C1->STAR2;

    if (res == I2C_OK) {
        (void)I2C1->STAR1;
        (void)I2C1->STAR2;
        i2c_stop();
    } else {
        I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
        i2c_stop();
    }
    return res;
}

/**
 * @brief Низкоуровневая отправка одного байта данных
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
    
    consecutive_errors = 0;
    return I2C_OK;
}

/**
 * @brief Общий хелпер ожидания флага STAR1 в цикле чтения данных.
 * Проверяет BERR/ARLO на каждой итерации (как и все остальные wait-циклы в
 * этом файле), поэтому реагирует на аппаратную ошибку немедленно, а не через
 * полный I2C_TIMEOUT. При таймауте или ошибке гарантированно восстанавливает
 * ACK (для корректного завершения приема) и восстанавливает шину/счетчик
 * ошибок. Используется во всех read-функциях, где ожидается RXNE/BTF после
 * успешной адресации ведомого на прием.
 */
static uint8_t i2c_wait_flag_or_recover(uint16_t flag) {
    uint32_t timeout = I2C_TIMEOUT;
    while (!(I2C1->STAR1 & flag)) {
        uint16_t star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            I2C1->CTLR1 |= I2C_CTLR1_ACK;
            i2c_stop();
            handle_critical_error();
            return I2C_NACK;
        }
        if (--timeout == 0) {
            I2C1->CTLR1 |= I2C_CTLR1_ACK;
            i2c_bus_recovery();
            return I2C_NACK;
        }
    }
    return I2C_OK;
}

/**
 * @brief Запись в одиночный 8-битный регистр устройства
 * @note При любых внутренних ошибках записи, функции i2c_send_byte/i2c_wait_ack 
 * самостоятельно генерируют STOP-условие, предотвращая зависание шины.
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
 * @brief Чтение одиночного 8-битного регистра
 */
uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value) {
    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    if (i2c_write_byte(reg_addr) != I2C_OK) return I2C_NACK;
    
    if (i2c_repeated_start() != I2C_OK) return I2C_NACK;
    
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    
    if (i2c_send_addr(dev_addr, I2C_DIR_RX) != I2C_OK) {
        I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Исправлено: Гарантированный возврат ACK при ошибке */
        return I2C_NACK;
    }
    
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    
    if (i2c_wait_flag_or_recover(I2C_STAR1_RXNE) != I2C_OK) {
        return I2C_NACK;
    }
    
    *p_value = (uint8_t)I2C1->DATAR;
    
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    return I2C_OK;
}

/**
 * @brief Пакетная последовательная запись буфера
 */
uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len) {
    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    
    if (i2c_write_byte(reg_addr) != I2C_OK) return I2C_NACK;
    
    for (uint16_t i = 0; i < len; i++) {
        if (i2c_write_byte(p_buf[i]) != I2C_OK) {
            return I2C_NACK;
        }
    }
    
    i2c_stop();
    return I2C_OK;
}

/**
 * @brief Пакетное последовательное чтение буфера из регистра
 */
uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len) {
    if (len == 0) return I2C_OK;

    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(dev_addr, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    if (i2c_write_byte(reg_addr) != I2C_OK) return I2C_NACK;
    
    if (i2c_repeated_start() != I2C_OK) return I2C_NACK;

    if (len == 1) {
        I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
        if (i2c_send_addr(dev_addr, I2C_DIR_RX) != I2C_OK) {
            I2C1->CTLR1 |= I2C_CTLR1_ACK;
            return I2C_NACK;
        }
        I2C1->CTLR1 |= I2C_CTLR1_STOP;

        if (i2c_wait_flag_or_recover(I2C_STAR1_RXNE) != I2C_OK) {
            return I2C_NACK;
        }
        p_buf[0] = (uint8_t)I2C1->DATAR;
    } 
    else {
        /* is_pair фиксирует условие len==2 один раз: оно проверяется по обе
         * стороны вызова i2c_send_addr(), потому что POS обязан быть
         * установлен ДО снятия флага ADDR (внутри i2c_send_addr), а ACK
         * снят СРАЗУ ПОСЛЕ снятия ADDR — то есть сама природа тайминга
         * требует двух точек применения одного и того же условия. */
        const uint8_t is_pair = (len == 2);

        I2C1->CTLR1 |= I2C_CTLR1_ACK;

        /* При POS=1 бит ACK управляет СЛЕДУЮЩИМ принимаемым байтом, а не
         * текущим — это единственный документированный (и гонко-безопасный)
         * способ гарантированно NACK-нуть именно 2-й (последний) байт при
         * приёме ровно двух байт. */
        if (is_pair) {
            I2C1->CTLR1 |= I2C_CTLR1_POS;
        }

        if (i2c_send_addr(dev_addr, I2C_DIR_RX) != I2C_OK) {
            I2C1->CTLR1 &= ~I2C_CTLR1_POS;
            return I2C_NACK;
        }

        if (is_pair) {
            /* ACK снимается СРАЗУ после снятия ADDR (уже произошло внутри
             * i2c_send_addr) — при POS=1 это NACK-нёт именно 2-й байт. */
            I2C1->CTLR1 &= ~I2C_CTLR1_ACK;

            if (i2c_wait_flag_or_recover(I2C_STAR1_BTF) != I2C_OK) {
                I2C1->CTLR1 &= ~I2C_CTLR1_POS;
                return I2C_NACK;
            }
            I2C1->CTLR1 |= I2C_CTLR1_STOP;

            p_buf[0] = (uint8_t)I2C1->DATAR;
            p_buf[1] = (uint8_t)I2C1->DATAR;

            I2C1->CTLR1 &= ~I2C_CTLR1_POS; /* Восстановить состояние по умолчанию */
        } 
        else {
            for (uint16_t i = 0; i < len; i++) {
                if (i == len - 3) {
                    if (i2c_wait_flag_or_recover(I2C_STAR1_BTF) != I2C_OK) {
                        return I2C_NACK;
                    }
                    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
                    p_buf[i++] = (uint8_t)I2C1->DATAR;

                    if (i2c_wait_flag_or_recover(I2C_STAR1_BTF) != I2C_OK) {
                        return I2C_NACK;
                    }
                    I2C1->CTLR1 |= I2C_CTLR1_STOP;
                    p_buf[i++] = (uint8_t)I2C1->DATAR;

                    if (i2c_wait_flag_or_recover(I2C_STAR1_RXNE) != I2C_OK) {
                        return I2C_NACK;
                    }
                    p_buf[i] = (uint8_t)I2C1->DATAR;
                    break;
                }

                if (i2c_wait_flag_or_recover(I2C_STAR1_RXNE) != I2C_OK) {
                    return I2C_NACK;
                }
                p_buf[i] = (uint8_t)I2C1->DATAR;
            }
        }
    }

    I2C1->CTLR1 |= I2C_CTLR1_ACK; /* Гарантированный подъем ACK для штатного завершения */
    return I2C_OK;
}

/**
 * @brief Полное отключение I2C1
 */
void i2c_deinit(void) {
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB1PCENR &= ~RCC_APB1PCENR_I2C1EN;
    GPIOC->CFGLR &= ~((GPIO_CFG_AF_OD_50M << 4) | (GPIO_CFG_AF_OD_50M << 8));
    consecutive_errors = 0;
}