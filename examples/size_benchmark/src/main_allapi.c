#include <ch32v00x.h>

#include "i2c.h"

/*
 * Полный размерный бенчмарк: ссылается на КАЖДУЮ публичную функцию драйвера,
 * поэтому --gc-sections не может выбросить ни одну из них. Показывает реальный
 * footprint приложения, использующего весь API (в отличие от size_benchmark,
 * который измеряет только типовое «регистровое» применение).
 *
 * Транзакции не выполняются: гейт `gate` всегда 0, устройство на шине не нужно.
 */
static volatile uint8_t gate;

int main(void) {
    uint8_t value = 0;
#ifndef I2C_DISABLE_BUFFER_API
    uint8_t buf[4] = {0};
#endif

    SystemCoreClockUpdate();

    if (i2c_init(100000) == I2C_OK) {
        if (gate) {
            /* Управление шиной */
            (void)i2c_wait_bus_free();
            (void)i2c_start();
            (void)i2c_repeated_start();
            (void)i2c_stop();

            /* Низкоуровневое API */
            (void)i2c_send_addr(0x50, I2C_DIR_TX);
            (void)i2c_send_byte(0x00);
            (void)i2c_wait_ack();
            (void)i2c_write_byte(0x00);

            /* Регистровое API */
            (void)i2c_write_register(0x50, 0x00, 0x00);
            (void)i2c_read_register(0x50, 0x00, &value);

#ifndef I2C_DISABLE_LAST_ERROR
            (void)i2c_get_last_star1();
#endif

#ifndef I2C_DISABLE_SCANNER
            (void)i2c_probe_address(0x50, NULL, NULL);
#endif

#ifndef I2C_DISABLE_BUFFER_API
            (void)i2c_write_buffer(0x50, 0x00, buf, sizeof(buf));
            (void)i2c_read_buffer(0x50, 0x00, buf, sizeof(buf));
            (void)i2c_write_raw(0x50, buf, sizeof(buf));
            (void)i2c_read_raw(0x50, buf, sizeof(buf));
            (void)i2c_write_buffer16(0x50, 0x0000, buf, sizeof(buf));
            (void)i2c_read_buffer16(0x50, 0x0000, buf, sizeof(buf));
#endif
        }
        i2c_deinit();
    }

    while (1) {
    }
}
