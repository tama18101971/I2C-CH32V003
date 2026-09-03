/*
 * i2c.c — Универсальный отказоустойчивый драйвер I2C1 для CH32V003 — Версия 7.1.0
 */

#include "i2c.h"
#include "ch32v00x.h"

/* Маски конфигурации пинов PC1 и PC2 */
#define GPIO_PC1_PC2_MASK       0x00000FF0UL
#define GPIO_PC1_PC2_AF_OD_50M  GPIO_PC1_PC2_MASK
#define GPIO_PC1_PC2_OUT_OD_2M  0x00000660UL

/* Номера битов линий в порту C */
#define I2C_SDA_PIN             1   /* PC1 */
#define I2C_SCL_PIN             2   /* PC2 */

/* OADDR1: бит 14 должен быть установлен в 1 согласно требованиям Synopsys/WCH */
#define OADDR1_REQUIRED_BIT14   (1 << 14)

/* Целевой полупериод такта SCL при аппаратном восстановлении шины, мкс
 * (~50 кГц; масштабируется по частоте ядра в i2c_init).
 */
#define I2C_SCL_PULSE_US        10

/* Примерное количество тактов ядра на одну итерацию пустого цикла (load/branch/nop) */
#define I2C_NOP_LOOP_CYCLES    4

/* Критические секции для служебных последовательностей приёма (EV6_3 / EV7_2 / EV7_3).
 * Управляются макросом I2C_ATOMIC_CRITICAL из i2c.h.
 */
#if I2C_ATOMIC_CRITICAL
#define I2C_ENTER_CRITICAL()   __disable_irq()
#define I2C_EXIT_CRITICAL()    __enable_irq()
#else
#define I2C_ENTER_CRITICAL()   ((void)0)
#define I2C_EXIT_CRITICAL()    ((void)0)
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

/* Глобальные статические переменные драйвера.
 * Без инициализаторов: значения программируются в i2c_init() до первого
 * использования, а .bss (в отличие от .data) не занимает Flash.
 */
static uint32_t i2c_speed;
static uint32_t i2c_timeout_loops;
#if !defined(I2C_DISABLE_BUS_RECOVERY)
static uint32_t i2c_stretch_loops;
#endif
#if !defined(I2C_DISABLE_LAST_ERROR)
static uint16_t i2c_last_star1;
#define I2C_NOTE_ERROR(star1)   (i2c_last_star1 = (star1))
#define I2C_LAST_ERROR()        (i2c_last_star1)
#else
#define I2C_NOTE_ERROR(star1)   ((void)0)
#define I2C_LAST_ERROR()        ((uint16_t)0)
#endif
#if !defined(I2C_DISABLE_ERROR_COUNTER)
static uint8_t consecutive_errors;
#endif

/* Прототип локальной функции восстановления шины */
#if !defined(I2C_DISABLE_BUS_RECOVERY)
static void i2c_bus_recovery(void);
#endif

/**
 * @brief Снимок STAR1 на момент обнаружения последней ошибки (до очистки флагов).
 */
#if !defined(I2C_DISABLE_LAST_ERROR)
uint16_t i2c_get_last_star1(void) {
    return i2c_last_star1;
}
#endif

/**
 * @brief Установка режима работы пинов PC1 (SDA) и PC2 (SCL)
 */
static void i2c_set_gpio_mode(uint32_t mode) {
    GPIOC->CFGLR = (GPIOC->CFGLR & ~GPIO_PC1_PC2_MASK) | mode;
}

/**
 * @brief Пустой цикл на заданное число итераций (общая точка для всех задержек).
 */
static void i2c_spin(uint32_t loops) {
    while (loops--) {
        __asm volatile("nop");
    }
}

/**
 * @brief Программная задержка в микросекундах на основе частоты ядра.
 * @note Реализована макросом, чтобы аргумент всегда был константой этапа
 *       компиляции: умножение сворачивается в сдвиги-сложения и не тянет
 *       за собой __mulsi3 из libgcc (в rv32ec нет аппаратного умножения).
 *       Частота берётся из уже запрограммированного I2C1->CTLR2 (поле FREQ,
 *       равное PCLK1 в МГц), поэтому вызывать только после i2c_init.
 */
#define I2C_USLEEP(us) \
    i2c_spin(((uint32_t)(I2C1->CTLR2 & 0x3F) * (uint32_t)(us)) / I2C_NOP_LOOP_CYCLES)

/**
 * @brief Задержка на полупериод такта SCL для GPIO recovery.
 */
#if !defined(I2C_DISABLE_BUS_RECOVERY)
static void i2c_delay(void) {
    I2C_USLEEP(I2C_SCL_PULSE_US);
}
#endif

/**
 * @brief Фиксация аппаратных критических ошибок (ARLO, BERR).
 */
#if !defined(I2C_DISABLE_ERROR_COUNTER) && !defined(I2C_DISABLE_BUS_RECOVERY)
static inline void handle_critical_error(void) {
    consecutive_errors++;
    if (consecutive_errors >= I2C_MAX_ERROR_COUNT) {
        i2c_bus_recovery();
    }
}
#endif

/**
 * @brief Обработчик аппаратных ошибок BERR/ARLO с вызовом recovery.
 * @note Снимок STAR1 фиксируется здесь — до очистки флагов, единой точкой
 *       для всех циклов ожидания.
 */
static uint8_t i2c_handle_error(uint16_t star1) {
    I2C_NOTE_ERROR(star1);
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
 * @note Снимок STAR1 фиксируется вызывающим (I2C_NOTE_ERROR) до очистки флагов.
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
#ifdef I2C_FIXED_PCLK_HZ
    const uint32_t pclk1 = (I2C_FIXED_PCLK_HZ);
    if (i2c_speed == 0 || i2c_speed > 400000UL) {
        return I2C_ERR_CLK;
    }
#else
    uint32_t pclk1 = SystemCoreClock;

    /* CH32V003 I2C: SYSCLK must be in [2 MHz, 48 MHz] range, speed in [1, 400000] Hz. */
    if (pclk1 < 2000000UL || pclk1 > 48000000UL || i2c_speed == 0 || i2c_speed > 400000UL) {
        return I2C_ERR_CLK;
    }
#endif

    /* Вычисление масштабированного числа итераций таймаута (~16 тактов на итерацию цикла) */
    uint32_t freq_mhz = pclk1 / 1000000UL;
    i2c_timeout_loops = ((pclk1 / 1000UL) * (uint32_t)I2C_TIMEOUT_MS) / 16UL;
    if (i2c_timeout_loops < 100UL) {
        i2c_timeout_loops = 100UL;
    }

    /* Окно ожидания отпускания SCL ведомым: ~8 тактов на итерацию цикла опроса
     * (деление на 8 вместо точного 6.5 — сдвиг вместо деления, погрешность
     * ~20% для таймаута восстановления несущественна).
     */
#if !defined(I2C_DISABLE_BUS_RECOVERY)
    i2c_stretch_loops = (freq_mhz * (uint32_t)I2C_STRETCH_TIMEOUT_US) >> 3;
    if (i2c_stretch_loops < 100UL) {
        i2c_stretch_loops = 100UL;
    }
#endif

    I2C1->CTLR2 = (uint16_t)freq_mhz;

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
        /* Запрошенная скорость недостижима: делитель не помещается в CCR[11:0] */
        return I2C_ERR_CLK;
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
    uint32_t stretch = i2c_stretch_loops;
    while (!(GPIOC->INDR & (1 << I2C_SCL_PIN)) && --stretch);
    i2c_delay();
}

static void i2c_bus_recovery(void) {
    /* 1. Отключаем периферию I2C */
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB2PCENR |= RCC_IOPCEN;

    /* 2. Переводим SCL (PC2) и SDA (PC1) в режим Open-Drain 2MHz */
    i2c_set_gpio_mode(GPIO_PC1_PC2_OUT_OD_2M);

    /* Инициализируем линии в состояние HIGH (отпущены) сразу после переключения */
    GPIOC->BSHR = (1 << I2C_SDA_PIN) | (1 << I2C_SCL_PIN);
    i2c_delay();

    /* 3. Генерируем до 16 тактов SCL с контролем Clock Stretching */
    for (uint8_t i = 0; i < 16; i++) {
        GPIOC->BSHR = (1 << (I2C_SCL_PIN + 16)); /* SCL LOW */
        i2c_delay();

        GPIOC->BSHR = (1 << I2C_SCL_PIN);        /* SCL HIGH */
        i2c_wait_scl_high();

        if (GPIOC->INDR & (1 << I2C_SDA_PIN)) break; /* Если SDA отпущен в HIGH — слейв сдался */
    }

    /* 4. Формирование STOP-условия силами GPIO в режиме Open-Drain */
    GPIOC->BSHR = (1 << (I2C_SCL_PIN + 16)); /* SCL LOW */
    i2c_delay();
    GPIOC->BSHR = (1 << (I2C_SDA_PIN + 16)); /* SDA LOW */
    i2c_delay();

    GPIOC->BSHR = (1 << I2C_SCL_PIN);        /* SCL HIGH */
    i2c_wait_scl_high();

    GPIOC->BSHR = (1 << I2C_SDA_PIN);        /* SDA HIGH */
    i2c_delay();

    /* 5. Выполняем Software Reset (SWRST) очищенного блока I2C */
    I2C1->CTLR1 |= I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    /* 6. Возвращаем GPIO обратно в режим альтернативной функции Open-Drain (AF_OD) */
    i2c_set_gpio_mode(GPIO_PC1_PC2_AF_OD_50M);

    /* 7. Полное восстановление конфигурационных регистров I2C.
     * При ошибке (изменилась частота ядра / недостижимая скорость) периферия
     * остаётся выключенной: это безопаснее, чем включить PE с невалидными
     * CTLR2/CKCFGR и получить неопределённое поведение блока.
     */
    if (i2c_configure_registers() != I2C_OK) {
        return;
    }

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
#if !defined(I2C_DISABLE_LAST_ERROR)
    i2c_last_star1 = 0;
#endif
#if !defined(I2C_DISABLE_ERROR_COUNTER)
    consecutive_errors = 0;
#endif

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

    I2C_USLEEP(I2C_INTER_FRAME_DELAY_US);

    return I2C_OK;
}

/**
 * @brief Общий цикл ожидания снятия флага BUSY с обработкой ошибок и recovery.
 */
static uint8_t i2c_wait_busy_clear(void) {
    uint32_t timeout = i2c_timeout_loops;
    uint16_t star1;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        star1 = I2C1->STAR1;
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            return i2c_handle_error(star1);
        }
        if (--timeout == 0) {
            I2C_NOTE_ERROR(star1);
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
 * @note Порядок «обработать ошибку, затем STOP» важен: recovery уже принудительно
 *       завершает транзакцию, поэтому последующий i2c_stop() выполняется на чистой
 *       шине мгновенно и не вызывает второй recovery с повторным инкрементом
 *       счётчика ошибок (иначе латентность отказа удваивалась до 2*I2C_TIMEOUT_MS).
 */
static uint8_t i2c_wait_star1_flag(uint16_t flag) {
    uint32_t timeout = i2c_timeout_loops;
    uint16_t star1;

    for (;;) {
        star1 = I2C1->STAR1;
        if (star1 & flag) {
            return I2C_OK;
        }
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO | I2C_STAR1_AF)) {
            break;
        }
        if (--timeout == 0) {
            break;
        }
    }

    if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
        uint8_t status = i2c_handle_error(star1);
        i2c_stop();
        return status;
    }
    if (star1 & I2C_STAR1_AF) {
        I2C_NOTE_ERROR(star1);
        I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
        i2c_stop();
        return I2C_NACK;
    }

    /* Таймаут: STOP без повторного ожидания BUSY — его выполнит recovery */
    I2C_NOTE_ERROR(star1);
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return i2c_handle_timeout();
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
 * @brief Сброс аппаратного флага ADDR каноническим чтением STAR1, затем STAR2.
 * @note После сброса ADDR периферия отпускает SCL, и ведомый немедленно начинает
 *       передавать первый байт — см. комментарии к критическим секциям ниже.
 *       Результаты чтений намеренно не используются; регистры объявлены как
 *       volatile, поэтому оба обращения к периферии сохраняются компилятором.
 */
static inline void i2c_clear_addr(void) {
    uint16_t star1 = I2C1->STAR1;
    uint16_t star2 = I2C1->STAR2;
    (void)star1;
    (void)star2;
}

/**
 * @brief Сброс счётчика последовательных ошибок после успешной фазы обмена.
 */
static inline void i2c_note_success(void) {
#if !defined(I2C_DISABLE_ERROR_COUNTER)
    consecutive_errors = 0;
#endif
}

/**
 * @brief Отправка адреса и ожидание ADDR БЕЗ сброса флага.
 * @note Сброс ADDR оставлен вызывающему: приёмные пути обязаны выполнить его
 *       в одной последовательности с программированием STOP / снятием ACK.
 */
static uint8_t i2c_send_addr_raw(uint8_t addr, uint8_t direction) {
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
    }

    I2C1->DATAR = (uint16_t)(((addr & 0x7F) << 1) | (direction & 1));

    return i2c_wait_star1_flag(I2C_STAR1_ADDR);
}

/**
 * @brief Отправка адреса с автоматическим контролем ACK/NACK и сбросом ADDR
 */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction) {
    uint8_t res = i2c_send_addr_raw(addr, direction);
    if (res == I2C_OK) {
        i2c_clear_addr();
        i2c_note_success();
    }
    return res;
}

/**
 * @brief Проверка адреса для сканера
 * @param addr 7-битный адрес
 * @param p_star1 указатель для снимка STAR1 на момент ошибки (можно NULL)
 * @param p_star2 указатель для сохранения STAR2 (можно NULL)
 * @return I2C_OK если устройство ответило ACK, иначе код ошибки / I2C_NACK
 */
#ifndef I2C_DISABLE_SCANNER
uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2) {
    uint8_t res = I2C_NACK;
    if (i2c_start() == I2C_OK) {
        res = i2c_send_addr(addr, I2C_DIR_TX);
        if (res == I2C_OK) {
            i2c_stop();
        }
    }
    /* Отдаётся снимок, сделанный в момент ошибки: прямое чтение STAR1 здесь
     * бесполезно, так как AF/BERR/ARLO уже сняты обработчиком ошибок. */
    if (p_star1) *p_star1 = I2C_LAST_ERROR();
    if (p_star2) *p_star2 = I2C1->STAR2;
    I2C_USLEEP(I2C_INTER_FRAME_DELAY_US);
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
 * @note Последовательность EV6_3 (RM §26.3.3, N=1): ACK=0 ДО сброса ADDR,
 *       затем «сброс ADDR → STOP» единой последовательностью. Она должна
 *       завершиться до конца передачи первого байта, поэтому опционально
 *       выполняется с запрещёнными прерываниями (I2C_ATOMIC_CRITICAL).
 */
static uint8_t i2c_read_1byte(uint8_t dev_addr, uint8_t *p_buf) {
    uint8_t res;
    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    if ((res = i2c_send_addr_raw(dev_addr, I2C_DIR_RX)) != I2C_OK) {
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        return res;
    }

    I2C_ENTER_CRITICAL();
    i2c_clear_addr();
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    I2C_EXIT_CRITICAL();
    i2c_note_success();

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
        /* Канон RM §26.3.3 (N=2): POS=1 и ACK=1 выставляются ДО адресации,
         * затем «сброс ADDR → ACK=0» единой последовательностью до конца
         * приёма первого байта (опционально под критической секцией).
         */
        I2C1->CTLR1 |= (I2C_CTLR1_ACK | I2C_CTLR1_POS);

        if ((res = i2c_send_addr_raw(dev_addr, I2C_DIR_RX)) != I2C_OK) {
            I2C1->CTLR1 &= ~I2C_CTLR1_POS;
            return res;
        }

        I2C_ENTER_CRITICAL();
        i2c_clear_addr();
        I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
        I2C_EXIT_CRITICAL();
        i2c_note_success();

        /* Далее шина остановлена флагом BTF (SCL удерживается низким),
         * поэтому STOP и оба чтения DATAR не имеют временных ограничений.
         */
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

    /* len >= 3: ACK=1 до адресации, сброс ADDR штатным путём (первый байт
     * подтверждается, временного окна нет).
     */
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

    /* Хвост EV7_2: на каждом BTF шина остановлена (SCL низкий), поэтому
     * порядок «ACK=0 до чтения» и «STOP до чтения» достаточен сам по себе
     * и не требует запрета прерываний.
     */
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
 * @brief Начало транзакции с 16-битным адресом памяти/регистра (START + dev_addr TX + addr hi/lo)
 */
static uint8_t i2c_start_reg16_write(uint8_t dev_addr, uint16_t reg_addr) {
    uint8_t res;
    if ((res = i2c_start()) != I2C_OK ||
        (res = i2c_send_addr(dev_addr, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte((uint8_t)(reg_addr >> 8))) != I2C_OK ||
        (res = i2c_write_byte((uint8_t)(reg_addr & 0xFF))) != I2C_OK) {
        return res;
    }
    return I2C_OK;
}

/**
 * @brief Пакетная запись буфера с 16-битным адресом памяти/регистра (EEPROM 24LC32..24LC1025)
 */
uint8_t i2c_write_buffer16(uint8_t dev_addr, uint16_t reg_addr, const uint8_t *p_buf, uint16_t len) {
    uint8_t res;
    if ((res = i2c_start_reg16_write(dev_addr, reg_addr)) != I2C_OK ||
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
    if ((res = i2c_start_reg16_write(dev_addr, reg_addr)) != I2C_OK ||
        (res = i2c_repeated_start()) != I2C_OK) {
        return res;
    }
    return i2c_read_bytes_rx(dev_addr, p_buf, len);
}
#endif /* I2C_DISABLE_BUFFER_API */

/**
 * @brief Полное отключение I2C1
 * @note Дожидается освобождения шины, чтобы не оставить ведомого в подвешенном
 *       состоянии при снятии PE посреди транзакции.
 */
void i2c_deinit(void) {
    (void)i2c_wait_bus_free();
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB1PCENR &= ~RCC_I2C1EN;
    i2c_set_gpio_mode(0);
#if !defined(I2C_DISABLE_ERROR_COUNTER)
    consecutive_errors = 0;
#endif
}
