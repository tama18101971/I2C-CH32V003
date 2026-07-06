#include <ch32v00x.h>
#include <debug.h>

#include "i2c.h"
#include <stdio.h>

void i2c_scan_bus(void) {
    printf("--- I2C1 scanner ---\n");
    printf("     0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");

    uint8_t found = 0;

    for (uint8_t i = 0; i < 128; i += 16) {
        printf("%02X: ", i);
        for (uint8_t j = 0; j < 16; j++) {
            uint8_t addr = i + j;
            if (addr < 0x08 || addr > 0x77) {
                printf("   ");
                continue;
            }
            if (i2c_probe_address(addr, NULL, NULL) == I2C_OK) {
                printf("%02X ", addr);
                found++;
            } else {
                printf("-- ");
            }
        }
        printf("\n");
    }
    printf("--- found: %d ---\n", found);
}

int main(void) {
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);

    if (i2c_init(100000) != I2C_OK) {
        printf("I2C init failed\n");
        while (1);
    }

    i2c_scan_bus();

    while (1) {
    }
    return 0;
}
