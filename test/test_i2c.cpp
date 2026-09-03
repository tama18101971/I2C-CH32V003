/*
 * test/test_i2c.cpp — хост-тесты конечного автомата драйвера I2C-CH32V003.
 *
 * Драйвер (src/i2c.c) компилируется без изменений: настоящий ch32v00x.h
 * подменяется на test/mock/ch32v00x.h, а обращения к регистрам обслуживает
 * программная модель периферии и ведомого (test/i2c_mock.cpp).
 *
 * Покрываются: инициализация и валидация тактирования, START/STOP, адресная
 * фаза, запись, чтение len = 1 / 2 / 3 / N (включая каноничность ACK/POS/STOP
 * по RM0008 §26.3.3), NACK, BERR/ARLO, таймауты, восстановление шины,
 * критические секции, снимок последней ошибки и семантика len == 0.
 */

#include "i2c_mock.h"

/* i2c.h сам оборачивает объявления в extern "C" при сборке C++ */
#include "i2c.h"

#include <cstdio>
#include <cstring>

/* === Мини-фреймворк === */

static int g_tests = 0;
static int g_failures = 0;
static const char *g_current = "";

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

#define CHECK_EQ(actual, expected)                                              \
    do {                                                                        \
        long _a = (long)(actual);                                               \
        long _e = (long)(expected);                                             \
        if (_a != _e) {                                                         \
            std::printf("  FAIL %s:%d  %s: got %ld, expected %ld\n",            \
                        __FILE__, __LINE__, #actual, _a, _e);                   \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

#define CHECK_HEX(actual, expected)                                             \
    do {                                                                        \
        unsigned _a = (unsigned)(actual);                                        \
        unsigned _e = (unsigned)(expected);                                      \
        if (_a != _e) {                                                         \
            std::printf("  FAIL %s:%d  %s: got 0x%04X, expected 0x%04X\n",      \
                        __FILE__, __LINE__, #actual, _a, _e);                   \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

static void begin(const char *name) {
    g_current = name;
    g_tests++;
    std::printf("[ RUN ] %s\n", name);
    mock_reset();
}

/*
 * Ожидаемые коды ошибок зависят от режима статусов: при I2C_LEGACY_STATUS=1
 * таймауты и аппаратные ошибки сворачиваются в единый I2C_NACK (см. i2c.h).
 */
#if I2C_LEGACY_STATUS
#define EXPECT_TIMEOUT  I2C_NACK
#define EXPECT_BERR     I2C_NACK
#define EXPECT_ARLO     I2C_NACK
#else
#define EXPECT_TIMEOUT  I2C_ERR_TIMEOUT
#define EXPECT_BERR     I2C_ERR_BERR
#define EXPECT_ARLO     I2C_ERR_ARLO
#endif

/*
 * Ожидаемое число восстановлений шины зависит от собранной конфигурации:
 *   - без I2C_DISABLE_BUS_RECOVERY и со счётчиком: recovery на I2C_MAX_ERROR_COUNT-й
 *     подряд аппаратной ошибке (по умолчанию — на второй);
 *   - без счётчика: recovery на каждой аппаратной ошибке;
 *   - при отключённом восстановлении: никогда.
 * Таймаут вызывает recovery независимо от счётчика.
 */
#if defined(I2C_DISABLE_BUS_RECOVERY)
#define REC_ON_TIMEOUT      0
#define REC_AFTER_1ST_BERR  0
#define REC_AFTER_2ND_BERR  0
#elif defined(I2C_DISABLE_ERROR_COUNTER)
#define REC_ON_TIMEOUT      1
#define REC_AFTER_1ST_BERR  1
#define REC_AFTER_2ND_BERR  2
#else
#define REC_ON_TIMEOUT      1
#define REC_AFTER_1ST_BERR  0
#define REC_AFTER_2ND_BERR  1
#endif


/* Штатная инициализация с ведомым 0x50 на шине. */
static void init_ok(const uint8_t *slave_tx = nullptr, int tx_len = 0) {
    mock_slave_present(0x50, slave_tx, tx_len);
    CHECK_EQ(i2c_init(100000), I2C_OK);
}

/* === Инициализация и тактирование === */

static void test_init_registers(void) {
    begin("init: программирование CTLR2/CKCFGR/OADDR1 и включение тактов");
    init_ok();

    CHECK_EQ(mock_ctlr2() & 0x3F, 48);                 /* FREQ = 48 МГц */
    CHECK_EQ(mock_ckcfgr() & I2C_CKCFGR_FS, 0);        /* стандартный режим */
    CHECK_EQ(mock_ckcfgr() & I2C_CKCFGR_CCR, 240);     /* 48e6 / (100e3*2) */
    CHECK_HEX(mock_oaddr1(), 0x4000);                  /* требуемый бит 14 */
    CHECK(mock_rcc_apb1() & RCC_I2C1EN);
    CHECK(mock_ctlr1() & I2C_CTLR1_PE);
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
    CHECK_EQ(mock_swrst_count(), 1);
    /* PC1/PC2 в режиме AF_OD 50 МГц */
    CHECK_HEX(mock_gpioc_cfglr() & 0xFF0u, 0xFF0u);
}

static void test_init_fast_mode(void) {
    begin("init: fast mode 400 кГц выставляет FS и CCR = PCLK/(3*speed)");
    mock_slave_present(0x50, nullptr, 0);
    CHECK_EQ(i2c_init(400000), I2C_OK);
    CHECK(mock_ckcfgr() & I2C_CKCFGR_FS);
    CHECK_EQ(mock_ckcfgr() & I2C_CKCFGR_CCR, 40);      /* 48e6 / (400e3*3) */
}

static void test_init_rejects_bad_speed(void) {
    begin("init: отвергает speed = 0 и speed > 400 кГц");
    CHECK_EQ(i2c_init(0), I2C_ERR_CLK);
    mock_reset();
    CHECK_EQ(i2c_init(400001), I2C_ERR_CLK);
}

static void test_init_rejects_bad_clock(void) {
    begin("init: отвергает SystemCoreClock вне диапазона 2..48 МГц");
    mock_reset();
    SystemCoreClock = 1000000UL;
    CHECK_EQ(i2c_init(100000), I2C_ERR_CLK);
    CHECK_EQ(mock_ctlr1() & I2C_CTLR1_PE, 0);          /* периферия не включена */

    mock_reset();
    SystemCoreClock = 50000000UL;
    CHECK_EQ(i2c_init(100000), I2C_ERR_CLK);
}

static void test_init_low_clock_min_ccr(void) {
    begin("init: 2 МГц — CCR не опускается ниже 4");
    mock_reset();
    SystemCoreClock = 2000000UL;
    mock_slave_present(0x50, nullptr, 0);
    CHECK_EQ(i2c_init(100000), I2C_OK);
    CHECK_EQ(mock_ckcfgr() & I2C_CKCFGR_CCR, 10);
    CHECK_EQ(mock_ctlr2() & 0x3F, 2);

    mock_reset();
    SystemCoreClock = 2000000UL;
    mock_slave_present(0x50, nullptr, 0);
    CHECK_EQ(i2c_init(400000), I2C_OK);
    CHECK(mock_ckcfgr() & I2C_CKCFGR_FS);
    CHECK_EQ(mock_ckcfgr() & I2C_CKCFGR_CCR, 1);       /* min_ccr для fast mode */
}

static void test_init_unreachable_speed_is_error(void) {
    begin("init: недостижимо низкая скорость возвращает I2C_ERR_CLK (без тихого clamp)");
    mock_reset();
    SystemCoreClock = 48000000UL;
    /* CCR = 48e6/(2*speed) > 0xFFF  =>  speed < ~5860 Гц */
    CHECK_EQ(i2c_init(5000), I2C_ERR_CLK);
    CHECK_EQ(mock_ctlr1() & I2C_CTLR1_PE, 0);
}

/* === Базовые операции шины === */

static void test_start_stop(void) {
    begin("start/stop: START поднимает SB, STOP снимает BUSY");
    init_ok();
    CHECK_EQ(i2c_start(), I2C_OK);
    CHECK_EQ(mock_start_count(), 1);
    CHECK_EQ(i2c_stop(), I2C_OK);
    CHECK_EQ(mock_stop_count(), 1);
    CHECK_EQ(i2c_wait_bus_free(), I2C_OK);
}

static void test_send_addr_masks_address(void) {
    begin("send_addr: адрес маскируется до 7 бит (0xD0 -> 0x50)");
    init_ok();
    CHECK_EQ(i2c_start(), I2C_OK);
    /* 0xD0 & 0x7F == 0x50 — ведомый должен ответить ACK */
    CHECK_EQ(i2c_send_addr(0xD0, I2C_DIR_TX), I2C_OK);
    i2c_stop();
}

static void test_send_addr_direction_bit(void) {
    begin("send_addr: бит направления маскируется (direction=2 -> TX)");
    init_ok();
    CHECK_EQ(i2c_start(), I2C_OK);
    CHECK_EQ(i2c_send_addr(0x50, 2), I2C_OK);
    i2c_stop();
}

/* === Запись === */

static void test_write_register(void) {
    begin("write_register: START + addr + reg + value + STOP");
    init_ok();
    CHECK_EQ(i2c_write_register(0x50, 0x12, 0x34), I2C_OK);
    CHECK_EQ(mock_slave_rx_len(), 2);
    CHECK_HEX(mock_slave_rx()[0], 0x12);
    CHECK_HEX(mock_slave_rx()[1], 0x34);
    CHECK_EQ(mock_start_count(), 1);
    CHECK_EQ(mock_stop_count(), 1);
}

#ifndef I2C_DISABLE_BUFFER_API
static void test_write_buffer(void) {
    begin("write_buffer: регистр + все байты буфера");
    init_ok();
    const uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    CHECK_EQ(i2c_write_buffer(0x50, 0x20, data, 4), I2C_OK);
    CHECK_EQ(mock_slave_rx_len(), 5);
    CHECK_HEX(mock_slave_rx()[0], 0x20);
    CHECK_EQ(std::memcmp(mock_slave_rx() + 1, data, 4), 0);
}

static void test_write_buffer16(void) {
    begin("write_buffer16: 16-битный адрес передаётся big-endian");
    init_ok();
    const uint8_t data[2] = {0x11, 0x22};
    CHECK_EQ(i2c_write_buffer16(0x50, 0xABCD, data, 2), I2C_OK);
    CHECK_EQ(mock_slave_rx_len(), 4);
    CHECK_HEX(mock_slave_rx()[0], 0xAB);
    CHECK_HEX(mock_slave_rx()[1], 0xCD);
    CHECK_HEX(mock_slave_rx()[2], 0x11);
    CHECK_HEX(mock_slave_rx()[3], 0x22);
}

static void test_write_raw(void) {
    begin("write_raw: передача без адреса регистра");
    init_ok();
    const uint8_t data[3] = {0x01, 0x02, 0x03};
    CHECK_EQ(i2c_write_raw(0x50, data, 3), I2C_OK);
    CHECK_EQ(mock_slave_rx_len(), 3);
    CHECK_EQ(std::memcmp(mock_slave_rx(), data, 3), 0);
}

static void test_write_zero_length_touches_bus(void) {
    begin("write_buffer(len=0): выполняет START + addr + reg + STOP (контракт 6)");
    init_ok();
    CHECK_EQ(i2c_write_buffer(0x50, 0x77, nullptr, 0), I2C_OK);
    CHECK_EQ(mock_slave_rx_len(), 1);
    CHECK_HEX(mock_slave_rx()[0], 0x77);
    CHECK_EQ(mock_start_count(), 1);
    CHECK_EQ(mock_stop_count(), 1);
}

#endif /* I2C_DISABLE_BUFFER_API */

/* === Чтение: каноничность последовательностей === */

static void test_read_register_single(void) {
    begin("read_register (N=1): ACK=0 до сброса ADDR, STOP сразу после");
    const uint8_t tx[1] = {0x5A};
    init_ok(tx, 1);

    uint8_t value = 0;
    CHECK_EQ(i2c_read_register(0x50, 0x10, &value), I2C_OK);
    CHECK_HEX(value, 0x5A);

    /* RM §26.3.3, N=1: на момент сброса ADDR ACK обязан быть уже снят */
    CHECK_EQ(mock_ctlr1_at_addr_clear() & I2C_CTLR1_ACK, 0);
    CHECK_EQ(mock_ctlr1_at_addr_clear() & I2C_CTLR1_POS, 0);
    /* STOP программируется до чтения единственного байта */
    CHECK_EQ(mock_stop_before_read_index(), 0);
    /* Единственный принятый байт не подтверждается */
    CHECK_EQ(mock_ack_log_len(), 1);
    CHECK_EQ(mock_ack_log()[0], 0);
    /* Инвариант: ACK восстановлен для последующих транзакций */
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
}

#ifndef I2C_DISABLE_BUFFER_API
static void test_read_two_bytes(void) {
    begin("read len=2: POS=1 и ACK=1 до адресации, ACK=0 сразу после сброса ADDR");
    const uint8_t tx[2] = {0xDE, 0xAD};
    init_ok(tx, 2);

    uint8_t buf[2] = {0, 0};
    CHECK_EQ(i2c_read_buffer(0x50, 0x00, buf, 2), I2C_OK);
    CHECK_HEX(buf[0], 0xDE);
    CHECK_HEX(buf[1], 0xAD);

    /* Канон RM §26.3.3 (N=2): POS=1 и ACK=1 на момент сброса ADDR */
    CHECK(mock_ctlr1_at_addr_clear() & I2C_CTLR1_POS);
    CHECK(mock_ctlr1_at_addr_clear() & I2C_CTLR1_ACK);
    /* Немедленно после сброса ADDR ACK снимается (NACK уйдёт на 2-й байт) */
    CHECK_EQ(mock_ctlr1_after_addr_clear() & I2C_CTLR1_ACK, 0);
    CHECK(mock_ctlr1_after_addr_clear() & I2C_CTLR1_POS);
    /* STOP программируется до чтения обоих байт */
    CHECK_EQ(mock_stop_before_read_index(), 0);
    /* POS сброшен, ACK восстановлен по завершении */
    CHECK_EQ(mock_ctlr1() & I2C_CTLR1_POS, 0);
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
}

static void test_read_three_bytes(void) {
    begin("read len=3: ACK=1 до адресации, ACK=0 перед третьим байтом");
    const uint8_t tx[3] = {0x01, 0x02, 0x03};
    init_ok(tx, 3);

    uint8_t buf[3] = {0, 0, 0};
    CHECK_EQ(i2c_read_buffer(0x50, 0x00, buf, 3), I2C_OK);
    CHECK_HEX(buf[0], 0x01);
    CHECK_HEX(buf[1], 0x02);
    CHECK_HEX(buf[2], 0x03);

    CHECK(mock_ctlr1_at_addr_clear() & I2C_CTLR1_ACK);
    CHECK_EQ(mock_ctlr1_at_addr_clear() & I2C_CTLR1_POS, 0);
    /* Последний байт читается с уже снятым ACK */
    CHECK_EQ(mock_ctlr1_before_last_read() & I2C_CTLR1_ACK, 0);
    CHECK_EQ(mock_rx_read_count(), 3);
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
}

static void test_read_many_bytes(void) {
    begin("read len=8: все байты корректны, последний не подтверждается");
    uint8_t tx[8];
    for (int i = 0; i < 8; i++) tx[i] = (uint8_t)(0x10 + i);
    init_ok(tx, 8);

    uint8_t buf[8] = {0};
    CHECK_EQ(i2c_read_buffer(0x50, 0x00, buf, 8), I2C_OK);
    CHECK_EQ(std::memcmp(buf, tx, 8), 0);
    CHECK_EQ(mock_rx_read_count(), 8);
    CHECK_EQ(mock_ctlr1_before_last_read() & I2C_CTLR1_ACK, 0);
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
}

static void test_read_buffer16(void) {
    begin("read_buffer16: 16-битный адрес + repeated START + данные");
    const uint8_t tx[2] = {0x77, 0x88};
    init_ok(tx, 2);

    uint8_t buf[2] = {0, 0};
    CHECK_EQ(i2c_read_buffer16(0x50, 0x1234, buf, 2), I2C_OK);
    CHECK_HEX(mock_slave_rx()[0], 0x12);
    CHECK_HEX(mock_slave_rx()[1], 0x34);
    CHECK_HEX(buf[0], 0x77);
    CHECK_HEX(buf[1], 0x88);
    CHECK_EQ(mock_start_count(), 2);        /* START + repeated START */
}

static void test_read_raw(void) {
    begin("read_raw: чтение без адреса регистра");
    const uint8_t tx[4] = {0xA1, 0xA2, 0xA3, 0xA4};
    init_ok(tx, 4);

    uint8_t buf[4] = {0};
    CHECK_EQ(i2c_read_raw(0x50, buf, 4), I2C_OK);
    CHECK_EQ(std::memcmp(buf, tx, 4), 0);
    CHECK_EQ(mock_slave_rx_len(), 0);       /* адрес регистра не отправлялся */
}

static void test_read_zero_length_no_bus_activity(void) {
    begin("read(len=0): возвращает I2C_OK, не задействуя шину");
    init_ok();
    uint8_t buf[1] = {0};
    CHECK_EQ(i2c_read_buffer(0x50, 0x00, buf, 0), I2C_OK);
    CHECK_EQ(i2c_read_raw(0x50, buf, 0), I2C_OK);
    CHECK_EQ(i2c_read_buffer16(0x50, 0x0000, buf, 0), I2C_OK);
    CHECK_EQ(mock_start_count(), 0);
    CHECK_EQ(mock_stop_count(), 0);
}
#endif /* I2C_DISABLE_BUFFER_API */

/* === Отказы === */

static void test_nack_on_address(void) {
    begin("NACK на адресе: I2C_NACK, STOP выдан, снимок STAR1 содержит AF");
    mock_reset();
    mock_slave_absent();
    CHECK_EQ(i2c_init(100000), I2C_OK);

    CHECK_EQ(i2c_write_register(0x55, 0x00, 0x00), I2C_NACK);
    CHECK(mock_stop_count() >= 1);
#ifndef I2C_DISABLE_LAST_ERROR
    CHECK(i2c_get_last_star1() & I2C_STAR1_AF);
#endif
    /* Флаг AF в самой периферии уже очищен драйвером */
    CHECK_EQ(mock_recovery_count(), 0);     /* NACK не требует recovery */
}

static void test_nack_on_data(void) {
    begin("NACK на данных: возвращается I2C_NACK");
    init_ok();
    mock_slave_nack_data_at(1);             /* NACK на значении, не на регистре */
    CHECK_EQ(i2c_write_register(0x50, 0x30, 0x99), I2C_NACK);
#ifndef I2C_DISABLE_LAST_ERROR
    CHECK(i2c_get_last_star1() & I2C_STAR1_AF);
#endif
}

#ifndef I2C_DISABLE_SCANNER
static void test_probe_address(void) {
    begin("probe_address: ACK -> I2C_OK, отсутствие устройства -> I2C_NACK + AF в снимке");
    const uint8_t tx[1] = {0};
    init_ok(tx, 1);

    uint16_t s1 = 0, s2 = 0;
    CHECK_EQ(i2c_probe_address(0x50, &s1, &s2), I2C_OK);

    CHECK_EQ(i2c_probe_address(0x51, &s1, &s2), I2C_NACK);
    CHECK(s1 & I2C_STAR1_AF);               /* снимок сохранил причину */
}

static void test_probe_address_null_pointers(void) {
    begin("probe_address: NULL-указатели допустимы");
    init_ok();
    CHECK_EQ(i2c_probe_address(0x50, nullptr, nullptr), I2C_OK);
}
#endif /* I2C_DISABLE_SCANNER */

static void test_berr_triggers_recovery(void) {
    begin("BERR: код ошибки, снимок STAR1 и recovery по счётчику ошибок");
    init_ok();

    mock_inject_addr_fault(I2C_STAR1_BERR, 1);
    CHECK_EQ(i2c_write_register(0x50, 0x00, 0x00), EXPECT_BERR);
#ifndef I2C_DISABLE_LAST_ERROR
    CHECK(i2c_get_last_star1() & I2C_STAR1_BERR);
#endif
    CHECK_EQ(mock_recovery_count(), REC_AFTER_1ST_BERR);

    mock_inject_addr_fault(I2C_STAR1_BERR, 1);
    CHECK_EQ(i2c_write_register(0x50, 0x00, 0x00), EXPECT_BERR);
    CHECK_EQ(mock_recovery_count(), REC_AFTER_2ND_BERR);
}


static void test_arlo_reported(void) {
    begin("ARLO: код I2C_ERR_ARLO и корректный снимок STAR1");
    init_ok();
    mock_inject_addr_fault(I2C_STAR1_ARLO, 1);
    CHECK_EQ(i2c_write_register(0x50, 0x00, 0x00), EXPECT_ARLO);
#ifndef I2C_DISABLE_LAST_ERROR
    CHECK(i2c_get_last_star1() & I2C_STAR1_ARLO);
#endif
}

#if !defined(I2C_DISABLE_ERROR_COUNTER) && !defined(I2C_DISABLE_BUS_RECOVERY)
static void test_error_counter_resets_on_success(void) {
    begin("счётчик ошибок: успешная транзакция сбрасывает накопленные ошибки");
    init_ok();

    mock_inject_addr_fault(I2C_STAR1_BERR, 1);
    CHECK_EQ(i2c_write_register(0x50, 0x00, 0x00), EXPECT_BERR);
    CHECK_EQ(mock_recovery_count(), 0);

    CHECK_EQ(i2c_write_register(0x50, 0x01, 0x02), I2C_OK);   /* сброс счётчика */

    mock_inject_addr_fault(I2C_STAR1_BERR, 1);
    CHECK_EQ(i2c_write_register(0x50, 0x00, 0x00), EXPECT_BERR);
    CHECK_EQ(mock_recovery_count(), 0);      /* снова первая ошибка подряд */
}
#endif

static void test_timeout_on_frozen_bus(void) {
    begin("таймаут: зависшая шина даёт I2C_ERR_TIMEOUT и один recovery");
    init_ok();
    mock_freeze(1);
    CHECK_EQ(i2c_write_register(0x50, 0x00, 0x00), EXPECT_TIMEOUT);
    /* Ровно одно восстановление: порядок «обработать ошибку, затем STOP»
     * исключает двойной recovery (находка B1 аудита). */
    CHECK_EQ(mock_recovery_count(), REC_ON_TIMEOUT);
}

static void test_timeout_on_stuck_busy(void) {
    begin("таймаут BUSY: i2c_start() возвращает I2C_ERR_TIMEOUT");
    init_ok();
    mock_stuck_busy(1);
    CHECK_EQ(i2c_start(), EXPECT_TIMEOUT);
    CHECK_EQ(mock_recovery_count(), REC_ON_TIMEOUT);
}

#ifndef I2C_DISABLE_BUS_RECOVERY
static void test_recovery_generates_clocks(void) {
    begin("recovery: генерируются такты SCL, выполняется SWRST и переконфигурация");
    init_ok();
    int swrst_before = mock_swrst_count();

    mock_lines_stuck(1, 0);                  /* ведомый удерживает SDA */
    mock_freeze(1);
    CHECK_EQ(i2c_write_register(0x50, 0x00, 0x00), EXPECT_TIMEOUT);

    CHECK_EQ(mock_recovery_count(), 1);
    CHECK(mock_scl_pulses() >= 16);          /* до 16 импульсов + STOP */
    CHECK_EQ(mock_swrst_count(), swrst_before + 1);
    /* Периферия переконфигурирована и снова включена */
    CHECK_EQ(mock_ctlr2() & 0x3F, 48);
    CHECK(mock_ctlr1() & I2C_CTLR1_PE);
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
    /* GPIO возвращены в режим альтернативной функции */
    CHECK_HEX(mock_gpioc_cfglr() & 0xFF0u, 0xFF0u);
}
#endif /* I2C_DISABLE_BUS_RECOVERY */

static void test_ack_restored_after_read_failure(void) {
    begin("инвариант: ACK восстановлен после сбоя в цикле чтения");
    const uint8_t tx[4] = {1, 2, 3, 4};
    init_ok(tx, 4);

    mock_inject_addr_fault(I2C_STAR1_BERR, 1);
    uint8_t value = 0;
    CHECK(i2c_read_register(0x50, 0x00, &value) != I2C_OK);
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
    CHECK_EQ(mock_ctlr1() & I2C_CTLR1_POS, 0);
}

#ifndef I2C_DISABLE_BUFFER_API
static void test_pos_cleared_after_two_byte_failure(void) {
    begin("инвариант: POS сброшен и ACK восстановлен после сбоя чтения len=2");
    const uint8_t tx[2] = {1, 2};
    init_ok(tx, 2);

    mock_inject_addr_fault(I2C_STAR1_BERR, 1);
    uint8_t buf[2] = {0, 0};
    CHECK(i2c_read_buffer(0x50, 0x00, buf, 2) != I2C_OK);
    CHECK_EQ(mock_ctlr1() & I2C_CTLR1_POS, 0);
    CHECK(mock_ctlr1() & I2C_CTLR1_ACK);
}
#endif /* I2C_DISABLE_BUFFER_API */


/* === Критические секции === */

static void test_critical_sections(void) {
    begin("критические секции: EV6_3 защищён только при I2C_ATOMIC_CRITICAL=1");
    const uint8_t tx[1] = {0x11};
    init_ok(tx, 1);

    uint8_t value = 0;
    CHECK_EQ(i2c_read_register(0x50, 0x00, &value), I2C_OK);

#if I2C_ATOMIC_CRITICAL
    CHECK_EQ(mock_irq_depth_at_addr_clear(), 1);
    CHECK_EQ(mock_irq_depth_at_post_addr_write(), 1);
#else
    CHECK_EQ(mock_irq_depth_at_addr_clear(), 0);
    CHECK_EQ(mock_irq_depth_at_post_addr_write(), 0);
#endif
    CHECK(mock_irq_balanced());
}

/* === Деинициализация === */

static void test_deinit(void) {
    begin("deinit: снимает PE, отключает такты I2C1 и переводит пины в аналоговый режим");
    init_ok();
    i2c_deinit();
    CHECK_EQ(mock_ctlr1() & I2C_CTLR1_PE, 0);
    CHECK_EQ(mock_rcc_apb1() & RCC_I2C1EN, 0u);
    CHECK_EQ(mock_gpioc_cfglr() & 0xFF0u, 0u);
}

static void test_reinit_after_deinit(void) {
    begin("повторная init после deinit восстанавливает рабочее состояние");
    init_ok();
    i2c_deinit();
    CHECK_EQ(i2c_init(100000), I2C_OK);
    CHECK_EQ(i2c_write_register(0x50, 0x01, 0x02), I2C_OK);
}

/* === Точка входа === */

int main(void) {
    std::printf("=== I2C-CH32V003 host tests ===\n");
    std::printf("config: ATOMIC_CRITICAL=%d LEGACY_STATUS=%d LITE=%d\n\n",
                (int)I2C_ATOMIC_CRITICAL, (int)I2C_LEGACY_STATUS, (int)I2C_LITE);

    test_init_registers();
    test_init_fast_mode();
    test_init_rejects_bad_speed();
    test_init_rejects_bad_clock();
    test_init_low_clock_min_ccr();
    test_init_unreachable_speed_is_error();

    test_start_stop();
    test_send_addr_masks_address();
    test_send_addr_direction_bit();

    test_write_register();
#ifndef I2C_DISABLE_BUFFER_API
    test_write_buffer();
    test_write_buffer16();
    test_write_raw();
    test_write_zero_length_touches_bus();
#endif

    test_read_register_single();
#ifndef I2C_DISABLE_BUFFER_API
    test_read_two_bytes();
    test_read_three_bytes();
    test_read_many_bytes();
    test_read_buffer16();
    test_read_raw();
    test_read_zero_length_no_bus_activity();
#endif

    test_nack_on_address();
    test_nack_on_data();
#ifndef I2C_DISABLE_SCANNER
    test_probe_address();
    test_probe_address_null_pointers();
#endif
    test_berr_triggers_recovery();
    test_arlo_reported();
#if !defined(I2C_DISABLE_ERROR_COUNTER) && !defined(I2C_DISABLE_BUS_RECOVERY)
    test_error_counter_resets_on_success();
#endif
    test_timeout_on_frozen_bus();
    test_timeout_on_stuck_busy();
#ifndef I2C_DISABLE_BUS_RECOVERY
    test_recovery_generates_clocks();
#endif
    test_ack_restored_after_read_failure();
#ifndef I2C_DISABLE_BUFFER_API
    test_pos_cleared_after_two_byte_failure();
#endif

    test_critical_sections();


    test_deinit();
    test_reinit_after_deinit();

    std::printf("\n=== %d tests, %d failures ===\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
