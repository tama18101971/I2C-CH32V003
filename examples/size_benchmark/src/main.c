#include <ch32v00x.h>

#include "i2c.h"

static volatile uint8_t measure_register_api;

/*
 * Keep the register API reachable so linker garbage collection measures the
 * practical footprint of a typical sensor application, not just i2c_init().
 * No transaction is issued at runtime because no I2C device is required to
 * build or flash this benchmark.
 */
int main(void) {
    uint8_t value = 0;

    SystemCoreClockUpdate();
    if (i2c_init(100000) == I2C_OK) {
        if (measure_register_api) {
            (void)i2c_write_register(0x50, 0x00, 0x00);
            (void)i2c_read_register(0x50, 0x00, &value);
        }
        i2c_deinit();
    }

    while (1) {
    }
}