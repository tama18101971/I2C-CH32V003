/* Hardware regression firmware: CH32V003 PC1/PC2 + APDS-9960 (0x39).
 * Only ENABLE and ALS thresholds are written; all saved values are restored.
 * UART1 TX=PD5, 115200. No physical short-circuit fault injection is claimed.
 */
#include <ch32v00x.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "i2c.h"

#define APDS 0x39
#define R_ENABLE 0x80
#define R_THRESHOLD 0x84
#define R_ID 0x92

volatile struct {
    uint32_t magic, done, checks, failures, id, irq_count, max_ticks, stage;
} hil_result;

static uint8_t saved_enable, saved_threshold[4];
static uint8_t have_saved;

static uint32_t irq_mask(void) {
    uint32_t m;
    __asm volatile("csrr %0, mstatus" : "=r"(m));
    return m & 0x88u;
}

void TIM2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM2_IRQHandler(void) {
    TIM2->INTFR = (uint16_t)~1u;
    hil_result.irq_count++;
    for (uint32_t n = 2000; n; --n) __asm volatile("nop");
}

static void irq_load(int enabled) {
    if (!enabled) {
        TIM2->DMAINTENR = 0;
        TIM2->CTLR1 = 0;
        NVIC_DisableIRQ(TIM2_IRQn);
        return;
    }
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
    TIM2->CTLR1 = 0;
    TIM2->PSC = SystemCoreClock / 1000000UL - 1;
    TIM2->ATRLR = 999;
    TIM2->SWEVGR = 1;
    TIM2->INTFR = 0;
    TIM2->DMAINTENR = 1;
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CTLR1 = 1;
}

static void check(int condition, const char *what) {
    hil_result.checks++;
    if (!condition) {
        hil_result.failures++;
        if (hil_result.failures < 12)
            printf("FAIL stage=%lu %s CTLR1=%04x STAR1=%04x STAR2=%04x\n",
                   (unsigned long)hil_result.stage, what, I2C1->CTLR1,
                   i2c_get_last_star1(), I2C1->STAR2);
    }
}

static void timer_init(void) {
    SysTick->CTLR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = 0xFFFFFFFFUL;
    SysTick->SR = 0;
    SysTick->CTLR = 1; /* HCLK/8, free-running; no SysTick IRQ. */
}

static int read_checked(uint8_t reg, uint8_t *data, uint16_t len) {
    uint32_t begin = SysTick->CNT;
    uint8_t rc = i2c_read_buffer(APDS, reg, data, len);
    uint32_t ticks = SysTick->CNT - begin;
    if (ticks > hil_result.max_ticks) hil_result.max_ticks = ticks;
    check(rc == I2C_OK, "read status");
    check(!(I2C1->STAR2 & (I2C_STAR2_BUSY | I2C_STAR2_MSL)), "RX idle on return");
    check(!(I2C1->CTLR1 & (I2C_CTLR1_STOP | I2C_CTLR1_POS)), "RX STOP/POS clear");
    check((I2C1->CTLR1 & I2C_CTLR1_ACK) != 0, "RX ACK restored");
    return rc == I2C_OK;
}

static void exercise(unsigned rounds) {
    uint8_t pattern[4], frame[18], raw[5];
    static const uint8_t lengths[] = {1, 2, 3, 4, 5, 8, 16};
    for (unsigned round = 0; round < rounds; ++round) {
        for (unsigned k = 0; k < 4; ++k) pattern[k] = (uint8_t)(round * 37u + k * 61u);
        check(i2c_write_buffer(APDS, R_THRESHOLD, pattern, 4) == I2C_OK, "write4");
        for (unsigned j = 0; j < sizeof(lengths); ++j) {
            uint8_t n = lengths[j];
            memset(frame, 0xA5, sizeof(frame));
            uint8_t reg = n <= 4 ? R_THRESHOLD : R_ENABLE;
            if (read_checked(reg, frame + 1, n)) {
                unsigned count = n <= 4 ? n : n - 4;
                if (count > 4) count = 4;
                check(memcmp(frame + 1 + (n <= 4 ? 0 : 4), pattern, count) == 0,
                      "threshold payload");
            }
            check(frame[0] == 0xA5 && frame[n + 1] == 0xA5, "buffer guards");
        }
        raw[0] = R_THRESHOLD;
        memcpy(raw + 1, pattern, 4);
        check(i2c_write_raw(APDS, raw, 5) == I2C_OK, "raw write5");
        uint8_t reg = R_THRESHOLD;
        check(i2c_write_raw(APDS, &reg, 1) == I2C_OK, "raw pointer");
        check(i2c_read_raw(APDS, frame, 4) == I2C_OK, "raw read4");
        check(memcmp(frame, pattern, 4) == 0, "raw payload");
        for (unsigned n = 1; n <= 3; ++n) {
            check(i2c_write_buffer(APDS, R_THRESHOLD, pattern, (uint16_t)n) == I2C_OK,
                  "short write");
        }
        uint8_t id = 0;
        check(i2c_read_register(APDS, R_ID, &id) == I2C_OK && id == hil_result.id,
              "ID after transfers");
        if (hil_result.failures) return;
    }
}

int main(void) {
    SystemCoreClockUpdate();
    USART_Printf_Init(115200);
    timer_init();
    hil_result.magic = 0x49324348UL;
    printf("HIL START I2C 7.2.0 APDS9960 atomic=%d CPU=%lu\n",
           I2C_ATOMIC_CRITICAL, (unsigned long)SystemCoreClock);
    hil_result.stage = 1;
    uint8_t rc = i2c_init(100000);
    check(rc == I2C_OK, "init100k");
    printf("INIT rc=%u lines=%lx\n", rc, (unsigned long)(GPIOC->INDR & 6));
    if (rc != I2C_OK) goto done;
    check(i2c_probe_address(APDS, NULL, NULL) == I2C_OK, "probe39");
    uint8_t id;
    rc = i2c_read_register(APDS, R_ID, &id);
    hil_result.id = id;
    printf("APDS ID rc=%u value=%02x\n", rc, id);
    for (unsigned n = 1; n <= 4; ++n) {
        uint8_t block[4] = {0};
        uint8_t block_rc = i2c_read_buffer(APDS, 0x90, block, (uint16_t)n);
        printf("IDENTITY reg90 len=%u rc=%u %02x %02x %02x %02x\n", n, block_rc,
               block[0], block[1], block[2], block[3]);
    }
    for (unsigned n = 0; n < 4; ++n) {
        uint8_t repeated = 0;
        uint8_t id_rc = i2c_read_register(APDS, R_ID, &repeated);
        printf("IDENTITY single rc=%u id=%02x\n", id_rc, repeated);
    }
    check(rc == I2C_OK, "ID read status");
    if (id != 0xAB)
        printf("HIL WARN DATASHEET_ID_MISMATCH expected=ab actual=%02x; testing transport and restorable register map only\n", id);
    if (hil_result.failures) goto done;
    check(i2c_read_register(APDS, R_ENABLE, &saved_enable) == I2C_OK, "save ENABLE");
    check(i2c_read_buffer(APDS, R_THRESHOLD, saved_threshold, 4) == I2C_OK, "save thresholds");
    if (hil_result.failures) goto done;
    have_saved = 1;
    check(i2c_write_register(APDS, R_ENABLE, 0) == I2C_OK, "disable sensor engines");

    static const uint32_t rates[] = {10000, 50000, 100000, 200000, 400000};
    static const uint32_t divs[] = {RCC_SYSCLK_Div6, RCC_SYSCLK_Div2, RCC_SYSCLK_Div1};
    for (unsigned clock = 0; clock < 3 && !hil_result.failures; ++clock) {
        i2c_deinit();
        while (!(USART1->STATR & USART_STATR_TC)) { }
        RCC_HCLKConfig(divs[clock]);
        SystemCoreClockUpdate();
        USART_Printf_Init(115200);
        timer_init();
        for (unsigned r = 0; r < 5 && !hil_result.failures; ++r) {
            hil_result.stage = 10 + clock * 5 + r;
            check(i2c_init(rates[r]) == I2C_OK, "init rate");
            printf("RATE CPU=%lu request=%lu CCR=%04x\n", (unsigned long)SystemCoreClock,
                   (unsigned long)rates[r], I2C1->CKCFGR);
            exercise(32);
            check(i2c_probe_address(0x38, NULL, NULL) == I2C_NACK, "absent38 NACK");
            uint8_t next;
            check(i2c_read_register(APDS, R_ID, &next) == I2C_OK && next == id,
                  "next transaction after NACK");
            check(i2c_read_buffer(APDS, R_ID, NULL, 0) == I2C_OK, "zero read");
            printf("RATE DONE checks=%lu failures=%lu\n", (unsigned long)hil_result.checks,
                   (unsigned long)hil_result.failures);
        }
    }
#if I2C_ATOMIC_CRITICAL
    if (!hil_result.failures) {
        hil_result.stage = 30;
        check(i2c_init(400000) == I2C_OK, "init IRQ stress");
        uint32_t saved = irq_mask();
        __disable_irq();
        uint8_t data[2];
        check(i2c_read_buffer(APDS, R_THRESHOLD, data, 1) == I2C_OK, "masked read1");
        check(irq_mask() == 0, "incoming disabled IRQ preserved1");
        check(i2c_read_buffer(APDS, R_THRESHOLD, data, 2) == I2C_OK, "masked read2");
        check(irq_mask() == 0, "incoming disabled IRQ preserved2");
        __asm volatile("csrs mstatus, %0" :: "r"(saved) : "memory");
        irq_load(1);
        exercise(1024);
        irq_load(0);
        check(hil_result.irq_count > 0, "timer ISR executed");
        printf("IRQ DONE count=%lu failures=%lu\n", (unsigned long)hil_result.irq_count,
               (unsigned long)hil_result.failures);
    }
#endif
    if (!hil_result.failures) {
        hil_result.stage = 40;
        check(i2c_start() == I2C_OK, "fault START");
        I2C1->CTLR1 &= ~I2C_CTLR1_PE; /* Реальный PE-off fault, не physical short. */
        rc = i2c_send_addr(APDS, I2C_DIR_TX);
        check(rc == I2C_ERR_RECOVERY, "PE-off rejected");
        check(i2c_init(400000) == I2C_OK, "PE-off reinit");
        check(i2c_read_register(APDS, R_ID, &id) == I2C_OK && id == hil_result.id,
              "next transaction after PE-off/reinit");
        hil_result.stage = 41;
        check(i2c_start() == I2C_OK, "timeout START");
        check(i2c_send_addr(APDS, I2C_DIR_TX) == I2C_OK, "timeout address");
        /* Не записываем DATAR: BTF не появится. Проверяем настоящий polling
         * timeout/recovery на MCU, а не выдуманный response slave/short. */
        rc = i2c_wait_ack();
        check(rc == I2C_ERR_TIMEOUT, "empty-TX BTF timeout");
        check(i2c_read_register(APDS, R_ID, &id) == I2C_OK && id == hil_result.id,
              "next transaction after BTF timeout/recovery");
        printf("RECOVERY BTF_rc=%u failures=%lu\n", rc, (unsigned long)hil_result.failures);
    }

done:
    irq_load(0);
    if (have_saved) {
        if (!(I2C1->CTLR1 & I2C_CTLR1_PE)) (void)i2c_init(100000);
        check(i2c_write_buffer(APDS, R_THRESHOLD, saved_threshold, 4) == I2C_OK, "restore thresholds");
        check(i2c_write_register(APDS, R_ENABLE, saved_enable) == I2C_OK, "restore ENABLE");
        uint8_t values[4], enable;
        check(i2c_read_buffer(APDS, R_THRESHOLD, values, 4) == I2C_OK &&
              memcmp(values, saved_threshold, 4) == 0, "verify restored thresholds");
        check(i2c_read_register(APDS, R_ENABLE, &enable) == I2C_OK && enable == saved_enable,
              "verify restored ENABLE");
    }
    hil_result.done = 1;
    printf("HIL DONE checks=%lu failures=%lu id=%02lx irq=%lu max_ticks=%lu\n",
           (unsigned long)hil_result.checks, (unsigned long)hil_result.failures,
           (unsigned long)hil_result.id, (unsigned long)hil_result.irq_count,
           (unsigned long)hil_result.max_ticks);
    while (1) __asm volatile("nop");
}
