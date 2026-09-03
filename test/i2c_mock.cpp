/*
 * test/i2c_mock.cpp — программная модель периферии I2C1 CH32V003 и ведомого.
 *
 * Модель шаговая и реактивная: каждое обращение драйвера к регистру проходит
 * через mock_reg_read()/mock_reg_write() (перехват в test/mock/ch32v00x.h), где
 * и реализована семантика аппаратуры:
 *
 *   START (CTLR1.START=1)      -> BUSY=1, MSL=1, SB=1
 *   запись DATAR в фазе адреса -> ADDR=1 (ACK ведомого) либо AF=1 (NACK)
 *   чтение STAR1, затем STAR2  -> сброс ADDR, начало фазы данных
 *   запись DATAR в фазе данных -> сброс TXE/BTF, затем TXE=1 и BTF=1
 *                                 (или AF=1, если ведомый не подтвердил байт)
 *   приём                      -> RXNE=1; второй байт в сдвиговом регистре -> BTF=1
 *   чтение DATAR               -> сброс RXNE/BTF и продвижение приёма
 *   STOP (CTLR1.STOP=1)        -> завершение транзакции; в фазе приёма STOP
 *                                 отложен до вычитывания уже принятых байт,
 *                                 как и в аппаратуре
 *   SWRST                      -> сброс регистров периферии
 *
 * Приём продвигается двумя событиями: опросом STAR1 (аналог «байт дошёл по
 * шине») и чтением DATAR. Подтверждение (ACK/NACK) для каждого принимаемого
 * байта берётся из бита ACK в CTLR1 на момент приёма этого байта — так же, как
 * в аппаратуре. Именно это позволяет проверить каноничность последовательностей
 * приёма для len = 1, 2 и N (RM0008 §26.3.3).
 */

#include "mock/ch32v00x.h"
#include "i2c_mock.h"

#include <string.h>

/* === Экземпляры, видимые драйверу === */

I2C_TypeDef  mock_i2c1;
GPIO_TypeDef mock_gpioc;
RCC_TypeDef  mock_rcc;

/* SystemCoreClock объявлен в mock/ch32v00x.h внутри extern "C" */
uint32_t SystemCoreClock = 48000000UL;

/* === Внутреннее состояние === */

#define MOCK_MAX_LOG 64

enum Phase {
    PH_IDLE = 0,   /* шина свободна */
    PH_START,      /* выдан START, ожидается запись адреса */
    PH_ADDR,       /* адрес отправлен, выставлен ADDR или AF */
    PH_TX,         /* мастер-передатчик */
    PH_RX          /* мастер-приёмник */
};

namespace {

struct Model {
    uint32_t reg[MOCK_REG_COUNT];

    int phase;
    int addr_seen_star1;

    /* Ведомый */
    int slave_present;
    uint8_t slave_addr;
    uint8_t slave_tx[MOCK_MAX_LOG];
    int slave_tx_len;
    int slave_tx_pos;
    int nack_data_at;
    int data_index;

    /* Приёмник: DATAR + теневой (сдвиговый) регистр */
    int rx_shadow_valid;
    uint8_t rx_shadow;
    int rx_done;              /* мастер уже выдал NACK — байт был последним */
    int stop_pending;         /* STOP запрограммирован, ждём вычитывания байт */

    /* Инъекции отказов */
    uint16_t addr_fault_flag;
    int addr_fault_times;
    int frozen;
    int stuck_busy;
    int sda_low;
    int scl_low;

    /* Наблюдение */
    int recovery_count;
    int swrst_count;
    int start_count;
    int stop_count;
    int scl_pulses;
    int pe_low_seen;
    int prev_scl_out;

    uint8_t slave_rx[MOCK_MAX_LOG];
    int slave_rx_len;

    uint8_t ack_log[MOCK_MAX_LOG];
    int ack_log_len;

    int irq_depth;
    int irq_min_depth;
    int irq_depth_at_addr_clear;
    int irq_depth_at_post_addr_write;
    int expect_post_addr_write;

    /* Снимки последовательности приёма */
    uint16_t ctlr1_at_addr_clear;
    uint16_t ctlr1_after_addr_clear;
    int stop_before_read_index;
    uint16_t ctlr1_before_last_read;
    int rx_read_count;
};

Model m;

uint16_t star1()           { return (uint16_t)m.reg[MOCK_I2C_STAR1]; }
void star1_set(uint16_t v) { m.reg[MOCK_I2C_STAR1] |= v; }
void star1_clr(uint16_t v) { m.reg[MOCK_I2C_STAR1] &= (uint32_t)(uint16_t)~v; }
uint16_t star2()           { return (uint16_t)m.reg[MOCK_I2C_STAR2]; }
void star2_set(uint16_t v) { m.reg[MOCK_I2C_STAR2] |= v; }
void star2_clr(uint16_t v) { m.reg[MOCK_I2C_STAR2] &= (uint32_t)(uint16_t)~v; }
uint16_t ctlr1()           { return (uint16_t)m.reg[MOCK_I2C_CTLR1]; }

void log_ack(bool acked) {
    if (m.ack_log_len < MOCK_MAX_LOG) {
        m.ack_log[m.ack_log_len++] = (uint8_t)(acked ? 1 : 0);
    }
}

/* Окончательное завершение транзакции: шина освобождается. */
void bus_release() {
    m.phase = PH_IDLE;
    m.addr_seen_star1 = 0;
    m.rx_shadow_valid = 0;
    m.rx_done = 0;
    m.stop_pending = 0;
    m.data_index = 0;
    m.slave_tx_pos = 0;
    star1_clr((uint16_t)(I2C_STAR1_SB | I2C_STAR1_ADDR | I2C_STAR1_TXE |
                         I2C_STAR1_BTF | I2C_STAR1_RXNE));
    if (!m.stuck_busy) {
        star2_clr((uint16_t)(I2C_STAR2_BUSY | I2C_STAR2_MSL | I2C_STAR2_TRA));
    }
}

/*
 * Приём одного байта из ведомого. Байт попадает в DATAR, а если тот занят —
 * в теневой сдвиговый регистр (совпадение обоих поднимает BTF, как в железе).
 * Подтверждение берётся из текущего значения бита ACK; NACK означает, что этот
 * байт для мастера последний.
 */
void rx_receive_byte() {
    if (m.frozen || m.rx_done || m.rx_shadow_valid) {
        return;
    }
    if (m.slave_tx_pos >= m.slave_tx_len) {
        return;                     /* ведомому больше нечего отдавать */
    }

    uint8_t byte = m.slave_tx[m.slave_tx_pos++];
    bool acked = (ctlr1() & I2C_CTLR1_ACK) != 0;
    log_ack(acked);
    if (!acked) {
        m.rx_done = 1;              /* дальше мастер приём не продолжит */
    }

    if (!(star1() & I2C_STAR1_RXNE)) {
        m.reg[MOCK_I2C_DATAR] = byte;
        star1_set(I2C_STAR1_RXNE);
    } else {
        m.rx_shadow = byte;
        m.rx_shadow_valid = 1;
        star1_set(I2C_STAR1_BTF);
    }
}

void on_write_ctlr1(uint16_t v) {
    /* Первая запись в CTLR1 сразу после сброса ADDR — ключевая точка
     * последовательностей EV6_3 (STOP для N=1, ACK=0 для N=2). Фиксируется до
     * разбора конкретного бита, иначе ветка STOP/START «съест» событие. */
    if (m.expect_post_addr_write) {
        m.irq_depth_at_post_addr_write = m.irq_depth;
        m.ctlr1_after_addr_clear = v;
        m.expect_post_addr_write = 0;
    }

    if (v & I2C_CTLR1_SWRST) {
        m.swrst_count++;
        m.reg[MOCK_I2C_CTLR1] = I2C_CTLR1_SWRST;
        m.reg[MOCK_I2C_CTLR2] = 0;
        m.reg[MOCK_I2C_CKCFGR] = 0;
        m.reg[MOCK_I2C_OADDR1] = 0;
        m.reg[MOCK_I2C_STAR1] = 0;
        m.reg[MOCK_I2C_STAR2] = 0;
        m.phase = PH_IDLE;
        m.addr_seen_star1 = 0;
        m.rx_shadow_valid = 0;
        m.rx_done = 0;
        m.stop_pending = 0;
        m.stuck_busy = 0;
        if (m.pe_low_seen) {
            m.recovery_count++;     /* PE=0 -> такты SCL -> SWRST */
            m.pe_low_seen = 0;
        }
        return;
    }

    if (!(v & I2C_CTLR1_PE)) {
        m.pe_low_seen = 1;
    }

    if (v & I2C_CTLR1_STOP) {
        m.reg[MOCK_I2C_CTLR1] = (uint16_t)(v & (uint16_t)~I2C_CTLR1_STOP);
        m.stop_count++;
        if (m.phase == PH_RX) {
            /* В аппаратуре STOP вступает в силу после текущего байта: уже
             * принятые байты остаются доступными для чтения. */
            if (m.stop_before_read_index < 0) {
                m.stop_before_read_index = m.rx_read_count;
            }
            m.rx_done = 1;
            if ((star1() & I2C_STAR1_RXNE) || m.rx_shadow_valid) {
                m.stop_pending = 1;
                return;
            }
        }
        bus_release();
        return;
    }

    if (v & I2C_CTLR1_START) {
        m.reg[MOCK_I2C_CTLR1] = (uint16_t)(v & (uint16_t)~I2C_CTLR1_START);
        if (m.frozen) {
            return;
        }
        m.start_count++;
        m.phase = PH_START;
        m.addr_seen_star1 = 0;
        m.data_index = 0;
        m.slave_tx_pos = 0;
        m.rx_shadow_valid = 0;
        m.rx_done = 0;
        m.stop_pending = 0;
        star1_clr((uint16_t)(I2C_STAR1_ADDR | I2C_STAR1_TXE |
                             I2C_STAR1_BTF | I2C_STAR1_RXNE));
        star1_set(I2C_STAR1_SB);
        star2_set((uint16_t)(I2C_STAR2_BUSY | I2C_STAR2_MSL));
        return;
    }
}



void on_write_datar(uint16_t v) {
    if (m.frozen) {
        return;
    }

    /* Запись в DATAR очищает TXE (и BTF) — как в аппаратуре. */
    star1_clr((uint16_t)(I2C_STAR1_TXE | I2C_STAR1_BTF));

    if (m.phase == PH_START) {
        /* Адресная фаза */
        star1_clr(I2C_STAR1_SB);
        m.phase = PH_ADDR;

        if (m.addr_fault_times > 0) {
            m.addr_fault_times--;
            star1_set(m.addr_fault_flag);
            return;
        }

        uint8_t addr = (uint8_t)(v >> 1);
        bool reading = (v & 1) != 0;

        if (m.slave_present && addr == m.slave_addr) {
            star1_set(I2C_STAR1_ADDR);
            if (reading) {
                star2_clr(I2C_STAR2_TRA);
            } else {
                star2_set(I2C_STAR2_TRA);
            }
        } else {
            star1_set(I2C_STAR1_AF);
        }
        return;
    }

    if (m.phase == PH_TX) {
        /* Фаза данных, мастер-передатчик */
        if (m.nack_data_at >= 0 && m.data_index == m.nack_data_at) {
            m.data_index++;
            star1_set(I2C_STAR1_AF);
            return;
        }
        if (m.slave_rx_len < MOCK_MAX_LOG) {
            m.slave_rx[m.slave_rx_len++] = (uint8_t)v;
        }
        m.data_index++;
        star1_set((uint16_t)(I2C_STAR1_TXE | I2C_STAR1_BTF));
    }
}

void on_write_star1(uint16_t v) {
    /* Драйвер очищает флаги записью инвертированной маски: нулевой бит = очистить.
     * Аппаратура позволяет сбрасывать только AF/BERR/ARLO/OVR. */
    const uint16_t clearable = (uint16_t)(I2C_STAR1_AF | I2C_STAR1_BERR |
                                          I2C_STAR1_ARLO | I2C_STAR1_OVR);
    star1_clr((uint16_t)(clearable & (uint16_t)~v));
}

void refresh_indr() {
    uint32_t indr = m.reg[MOCK_GPIOC_OUTDR];
    if (m.sda_low) indr &= ~(1u << 1);
    if (m.scl_low) indr &= ~(1u << 2);
    m.reg[MOCK_GPIOC_INDR] = indr;
}

void on_write_bshr(uint32_t v) {
    /* Set/reset-регистр: младшие 16 бит устанавливают, старшие сбрасывают. */
    uint32_t out = m.reg[MOCK_GPIOC_OUTDR];
    out |= (v & 0xFFFFu);
    out &= ~((v >> 16) & 0xFFFFu);
    m.reg[MOCK_GPIOC_OUTDR] = out;

    int scl_now = (out & (1u << 2)) ? 1 : 0;
    if (scl_now && !m.prev_scl_out) {
        m.scl_pulses++;
    }
    m.prev_scl_out = scl_now;

    refresh_indr();
}

} /* namespace */

/* === Точки входа перехвата === */

extern "C" uint32_t mock_reg_read(int id) {
    switch (id) {
    case MOCK_I2C_STAR1:
        if (m.phase == PH_ADDR && (star1() & I2C_STAR1_ADDR)) {
            m.addr_seen_star1 = 1;
        }
        /* Опрос STAR1 в фазе приёма продвигает шину: приходит следующий байт. */
        if (m.phase == PH_RX) {
            rx_receive_byte();
        }
        return m.reg[MOCK_I2C_STAR1];

    case MOCK_I2C_STAR2:
        if (m.addr_seen_star1 && (star1() & I2C_STAR1_ADDR)) {
            /* Канонический сброс ADDR: чтение STAR1, затем STAR2 */
            m.irq_depth_at_addr_clear = m.irq_depth;
            m.ctlr1_at_addr_clear = ctlr1();
            m.expect_post_addr_write = 1;
            m.addr_seen_star1 = 0;
            star1_clr(I2C_STAR1_ADDR);
            if (star2() & I2C_STAR2_TRA) {
                m.phase = PH_TX;
                star1_set(I2C_STAR1_TXE);
            } else {
                m.phase = PH_RX;
                rx_receive_byte();      /* первый байт приходит сразу */
            }
        }
        return m.reg[MOCK_I2C_STAR2];

    case MOCK_I2C_DATAR:
        if (m.phase == PH_RX) {
            uint32_t value = m.reg[MOCK_I2C_DATAR];
            if (m.rx_done && !m.rx_shadow_valid) {
                m.ctlr1_before_last_read = ctlr1();
            }
            m.rx_read_count++;
            star1_clr((uint16_t)(I2C_STAR1_RXNE | I2C_STAR1_BTF));

            if (m.rx_shadow_valid) {
                m.reg[MOCK_I2C_DATAR] = m.rx_shadow;
                m.rx_shadow_valid = 0;
                star1_set(I2C_STAR1_RXNE);
            } else {
                rx_receive_byte();      /* следующий байт по шине */
            }

            if (m.stop_pending && !(star1() & I2C_STAR1_RXNE) && !m.rx_shadow_valid) {
                bus_release();          /* все принятые байты вычитаны */
            }
            return value;
        }
        return m.reg[MOCK_I2C_DATAR];

    default:
        return m.reg[id];
    }
}

extern "C" void mock_reg_write(int id, uint32_t value) {
    switch (id) {
    case MOCK_I2C_CTLR1:
        on_write_ctlr1((uint16_t)value);
        if (!((uint16_t)value & (I2C_CTLR1_SWRST | I2C_CTLR1_START | I2C_CTLR1_STOP))) {
            m.reg[MOCK_I2C_CTLR1] = value;
        }
        return;

    case MOCK_I2C_DATAR:
        m.reg[MOCK_I2C_DATAR] = value;
        on_write_datar((uint16_t)value);
        return;

    case MOCK_I2C_STAR1:
        on_write_star1((uint16_t)value);
        return;

    case MOCK_GPIOC_BSHR:
        m.reg[MOCK_GPIOC_BSHR] = value;
        on_write_bshr(value);
        return;

    default:
        m.reg[id] = value;
        return;
    }
}

extern "C" void mock_disable_irq(void) {
    m.irq_depth++;
}

extern "C" void mock_enable_irq(void) {
    m.irq_depth--;
    if (m.irq_depth < m.irq_min_depth) {
        m.irq_min_depth = m.irq_depth;
    }
}

/* === Публичный API модели === */

extern "C" void mock_reset(void) {
    memset(&m, 0, sizeof(m));
    m.nack_data_at = -1;
    m.irq_depth_at_addr_clear = -1;
    m.irq_depth_at_post_addr_write = -1;
    m.stop_before_read_index = -1;
    m.prev_scl_out = 1;
    m.reg[MOCK_GPIOC_OUTDR] = (1u << 1) | (1u << 2);   /* линии отпущены */
    m.reg[MOCK_GPIOC_INDR]  = (1u << 1) | (1u << 2);
    SystemCoreClock = 48000000UL;
}

extern "C" void mock_slave_present(uint8_t addr, const uint8_t *tx, int tx_len) {
    m.slave_present = 1;
    m.slave_addr = addr;
    m.slave_tx_len = 0;
    if (tx && tx_len > 0) {
        if (tx_len > MOCK_MAX_LOG) tx_len = MOCK_MAX_LOG;
        memcpy(m.slave_tx, tx, (size_t)tx_len);
        m.slave_tx_len = tx_len;
    }
    m.slave_tx_pos = 0;
}

extern "C" void mock_slave_absent(void)          { m.slave_present = 0; }
extern "C" void mock_slave_nack_data_at(int idx) { m.nack_data_at = idx; }

extern "C" void mock_inject_addr_fault(uint16_t star1_flag, int times) {
    m.addr_fault_flag = star1_flag;
    m.addr_fault_times = times;
}

extern "C" void mock_freeze(int on) { m.frozen = on; }

extern "C" void mock_stuck_busy(int on) {
    m.stuck_busy = on;
    if (on) {
        star2_set(I2C_STAR2_BUSY);
    }
}

extern "C" void mock_lines_stuck(int sda_low, int scl_low) {
    m.sda_low = sda_low;
    m.scl_low = scl_low;
    refresh_indr();
}

extern "C" int mock_recovery_count(void) { return m.recovery_count; }
extern "C" int mock_swrst_count(void)    { return m.swrst_count; }
extern "C" int mock_start_count(void)    { return m.start_count; }
extern "C" int mock_stop_count(void)     { return m.stop_count; }
extern "C" int mock_scl_pulses(void)     { return m.scl_pulses; }

extern "C" int mock_slave_rx_len(void)        { return m.slave_rx_len; }
extern "C" const uint8_t *mock_slave_rx(void) { return m.slave_rx; }
extern "C" int mock_ack_log_len(void)         { return m.ack_log_len; }
extern "C" const uint8_t *mock_ack_log(void)  { return m.ack_log; }

extern "C" uint16_t mock_ctlr1_at_addr_clear(void)    { return m.ctlr1_at_addr_clear; }
extern "C" uint16_t mock_ctlr1_after_addr_clear(void) { return m.ctlr1_after_addr_clear; }
extern "C" int mock_stop_before_read_index(void)      { return m.stop_before_read_index; }
extern "C" uint16_t mock_ctlr1_before_last_read(void) { return m.ctlr1_before_last_read; }
extern "C" int mock_rx_read_count(void)               { return m.rx_read_count; }

extern "C" int mock_irq_depth_at_addr_clear(void)      { return m.irq_depth_at_addr_clear; }
extern "C" int mock_irq_depth_at_post_addr_write(void) { return m.irq_depth_at_post_addr_write; }
extern "C" int mock_irq_balanced(void) { return (m.irq_depth == 0 && m.irq_min_depth >= 0) ? 1 : 0; }

extern "C" uint16_t mock_ctlr1(void)       { return (uint16_t)m.reg[MOCK_I2C_CTLR1]; }
extern "C" uint16_t mock_ctlr2(void)       { return (uint16_t)m.reg[MOCK_I2C_CTLR2]; }
extern "C" uint16_t mock_ckcfgr(void)      { return (uint16_t)m.reg[MOCK_I2C_CKCFGR]; }
extern "C" uint16_t mock_oaddr1(void)      { return (uint16_t)m.reg[MOCK_I2C_OADDR1]; }
extern "C" uint32_t mock_gpioc_cfglr(void) { return m.reg[MOCK_GPIOC_CFGLR]; }
extern "C" uint32_t mock_rcc_apb1(void)    { return m.reg[MOCK_RCC_APB1PCENR]; }
