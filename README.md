# Reliable I2C (I2C1) Bus Driver for CH32V003 (RISC-V) — Version 7.0.0

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

[🇷🇺 Русский](README.ru.md)

A high-reliability, fault-tolerant, memory-optimized I2C driver for **CH32V003** series microcontrollers. Designed as a complete replacement for the standard WCH EVT library, which is prone to hard lockups in infinite polling loops (`while(!I2C_CheckEvent(...))`) caused by electromagnetic interference, slave power dips, or bus line shorts.

---

## Architectural Highlights (Version 7.0.0)

1. **C++ Compatibility:** Header `i2c.h` is enclosed in `extern "C"` blocks for direct usage in C++ and Arduino projects without linking issues.
2. **Zero-Delay Transactions (`i2c_stop`):** Removed fixed 50 µs delay from standard STOP generation. Inter-frame delay is applied strictly within `i2c_probe_address()` during bus scanning, allowing maximum bus throughput (up to 400 kHz) for streaming devices like DACs and displays.
3. **Dynamic Clock-Scaled Timeout:** Bounded wait loops scale dynamically against `SystemCoreClock` via `I2C_TIMEOUT_MS` (default 40 ms), preventing wildly varying timeout periods between 2 MHz and 48 MHz system clocks.
4. **Read-Only Workload Protection:** `consecutive_errors` counter resets upon successful `ADDR` phase ACK in `i2c_send_addr()`, preventing sporadic transient hardware errors from accumulating into false bus recovery triggers during long read sessions.
5. **Raw & 16-Bit Register APIs:** Native `i2c_write_raw()` / `i2c_read_raw()` for register-less devices (DAC7571) and `i2c_write_buffer16()` / `i2c_read_buffer16()` for memories with 16-bit word addresses (24LC32..24LC1025).
6. **Granular Return Codes:** Distinct status codes (`I2C_ERR_TIMEOUT`, `I2C_ERR_BERR`, `I2C_ERR_ARLO`, `I2C_ERR_CLK`, `I2C_NACK`, `I2C_OK`) with optional legacy fallback `-DI2C_LEGACY_STATUS=1`.
7. **Bus Speed Validation:** `i2c_init()` validates speed up to 400 kHz and returns `I2C_ERR_CLK` for out-of-spec configurations.
8. **Symbol encapsulation:** The `i2c_bus_recovery` function is declared `static`, isolated inside `i2c.c` and not exported externally.
9. **Instant timeout recovery:** Stuck bus loops immediately trigger hardware peripheral recovery without waiting for multiple attempts.
10. **Safe low-level API (`i2c_send_byte`):** Byte transmission is separated from acknowledgment waiting. For atomic operations, `i2c_write_byte()` is provided.
11. **Absolute ACK bit protection on faults:** In all failure paths of read functions (`i2c_read_register` and `i2c_read_buffer`), `ACK = 1` is forcibly restored before exit.
12. **RISC-V (RV32EC) optimization:** Buffer lengths use `uint16_t` to minimize register usage and call overhead on RV32EC.

---

## Physical Layer and Wiring

The driver operates with the **I2C1** hardware block on the controller's dedicated pins:

* **PC1 — SDA** (Mode: Alternate Function Open-Drain, 50MHz)
* **PC2 — SCL** (Mode: Alternate Function Open-Drain, 50MHz)

> **IMPORTANT:** For proper bus operation, both lines (SDA and SCL) **must** have external pull-up resistors in the range of **2.2 kΩ to 4.7 kΩ** connected to the 3.3V supply. The MCU's internal pull-up cannot provide sufficient edge slope at frequencies above 10 kHz.

---

## Status Codes

| Code | Value | Description |
|---|:---:|---|
| `I2C_OK` | `0` | Success / Acknowledged (ACK) |
| `I2C_NACK` | `1` | Not acknowledged (NACK from slave) or generic error |
| `I2C_ERR_TIMEOUT` | `2` | Software timeout waiting for bus release or peripheral flag |
| `I2C_ERR_CLK` | `3` | Invalid clock configuration (PCLK1 outside 2..48 MHz, speed > 400 kHz, or speed == 0) |
| `I2C_ERR_BERR` | `4` | Bus Error (misplaced START or STOP condition) |
| `I2C_ERR_ARLO` | `5` | Arbitration Lost (multi-master collision) |

*All non-zero codes evaluate to true on `if (err != I2C_OK)` checks.* To map `TIMEOUT`, `BERR`, and `ARLO` into `I2C_NACK` (v6 behavior), define `-DI2C_LEGACY_STATUS=1`.

---

## API Reference

### Low-Level Bus Management and Initialization Functions
* `uint8_t i2c_init(uint32_t bound);`
  Performs an I2C1 block reset via `SWRST`, configures GPIO and peripheral clocking, and calculates `CTLR2` and `CKCFGR` register values for Standard Mode (up to 100 kHz) or Fast Mode (up to 400 kHz) based on the current `SystemCoreClock`. Automatically sets the mandatory bit 14 in `OADDR1`. Returns `I2C_OK` or `I2C_ERR_CLK` if `SystemCoreClock` is outside the valid 2..48 MHz range or `bound > 400000`.
* `void i2c_deinit(void);`
  Disables the I2C1 peripheral, deactivates the APB1 bus clock, and puts pins PC1/PC2 into a high-impedance state.
* `uint8_t i2c_wait_bus_free(void);`
  Polls the `BUSY` flag. Also detects `BERR`/`ARLO` hardware errors inside the loop for immediate recovery. If the flag is not cleared within `I2C_TIMEOUT_MS`, the function emergency-calls `i2c_bus_recovery`.
* `uint8_t i2c_start(void);`
  Generates a START condition on the bus with a preliminary bus availability check. Timeout-limited.
* `uint8_t i2c_repeated_start(void);`
  Generates a Repeated START without checking the `BUSY` flag. Used when switching from register address write to data read.
* `uint8_t i2c_stop(void);`
  Sets the `STOP` bit and waits for the bus to clear. Returns `I2C_OK` on success. On failure, triggers bus recovery internally.
* `uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2);`
  Probes a single 7-bit I2C address. Returns `I2C_OK` if the device ACKed, `I2C_NACK` otherwise. Optionally saves `STAR1`/`STAR2` register values for diagnostics (pass `NULL` to skip). Includes inter-frame spacing `I2C_INTER_FRAME_DELAY_US`.

### Data Transfer Functions
* `uint8_t i2c_send_addr(uint8_t addr, uint8_t direction);`
  Sends a 7-bit device address shifted left, combined with the direction bit (`I2C_DIR_TX` or `I2C_DIR_RX`). Enforces `direction & 1` masking. Clears the `ADDR` flag by reading `STAR1` and `STAR2` registers and resets `consecutive_errors`.
* `uint8_t i2c_send_byte(uint8_t data);`
  *Low-level function.* Writes a byte to `DATAR` and waits only for transmit buffer release (`TXE`). **Does not check physical reception by the slave!**
* `uint8_t i2c_wait_ack(void);`
  *Low-level function.* Waits for byte transfer complete (`BTF`) or acknowledgment failure (`AF`) flag. Resets the error counter on success.
* `static inline uint8_t i2c_write_byte(uint8_t data);`
  *Atomic inline function.* Combines `i2c_send_byte` and `i2c_wait_ack`. Recommended for custom low-level sequences.

### High-Level Application API
* `uint8_t i2c_write_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);`
  Writes a single byte `value` to 8-bit register `reg_addr` of device `dev_addr`.
* `uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value);`
  Reads a single byte from 8-bit register `reg_addr`.
* `uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len);`
  Sequential write of data array `p_buf` of length `len` starting from 8-bit register `reg_addr`.
* `uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len);`
  Multi-byte streaming read starting from 8-bit register `reg_addr`.
* `uint8_t i2c_write_raw(uint8_t dev_addr, const uint8_t *p_buf, uint16_t len);`
  Direct multi-byte write to `dev_addr` without sending a register address. Ideal for DACs, streaming I/O, and register-less devices.
* `uint8_t i2c_read_raw(uint8_t dev_addr, uint8_t *p_buf, uint16_t len);`
  Direct multi-byte read from `dev_addr` without sending a register address preamble.
* `uint8_t i2c_write_buffer16(uint8_t dev_addr, uint16_t reg_addr, const uint8_t *p_buf, uint16_t len);`
  Sequential write with a 16-bit big-endian register/word address `reg_addr` (for EEPROMs 24LC32..24LC1025).
* `uint8_t i2c_read_buffer16(uint8_t dev_addr, uint16_t reg_addr, uint8_t *p_buf, uint16_t len);`
  Multi-byte streaming read with a 16-bit big-endian register/word address `reg_addr`.

---

## Practical Usage Examples

### Example 1: I2C Bus Scanner
The high-level `i2c_probe_address()` helper encapsulates START, address transmission, and STOP into a single safe call — no manual low-level sequencing needed. Calling an address with no responding device returns a regular `I2C_NACK` without causing a bus reset or driver hang.

> **Note:** This example uses `printf()` for console output. On CH32V003, you must redirect stdout to UART yourself (implement `_write`/`putchar` with USART byte transmission). Without this retarget, `printf` calls will produce no visible output.

```c
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
            
            // Skip reserved addresses
            if (addr < 0x08 || addr > 0x77) {
                printf("   ");
                continue;
            }
            
            // i2c_probe_address() handles START, address, and STOP internally
            if (i2c_probe_address(addr, NULL, NULL) == I2C_OK) {
                printf("%02X ", addr); // Device ACKed!
                found++;
            } else {
                printf("-- "); // No response (NACK)
            }
        }
        printf("\n");
    }
    printf("--- found: %d ---\n", found);
}
```

### Example 2: Reading the APDS-9960 Gesture Sensor FIFO Buffer

In sensors with an internal cyclic FIFO buffer, it is critical to read exactly the number of bytes the sensor reports. An extra, unplanned SCL pulse will cause the sensor to irreversibly discard the next byte from memory. Our driver reads N ≥ 3 strictly per specification.

```c
#include "i2c.h"

#define APDS9960_I2C_ADDR    0x39
#define APDS9960_REG_GFLVL   0xAE   // FIFO fill level register
#define APDS9960_REG_GFIFO_R 0xFC   // FIFO read start register

static uint8_t raw_gesture_data[128];

void handle_gesture_sensor(void) {
    uint8_t datasets_count = 0;
    
    // Read the number of available datasets (1 set = 4 bytes: [U, D, L, R])
    if (i2c_read_register(APDS9960_I2C_ADDR, APDS9960_REG_GFLVL, &datasets_count) != I2C_OK) {
        return; // Transaction error — driver is protected against hangs
    }
    
    if (datasets_count == 0) return; // No data yet
    if (datasets_count > 32) datasets_count = 32; // Limit to local buffer size
    
    uint16_t total_bytes = datasets_count * 4;
    
    // Bulk read of FIFO data buffer
    if (i2c_read_buffer(APDS9960_I2C_ADDR, APDS9960_REG_GFIFO_R, raw_gesture_data, total_bytes) == I2C_OK) {
        // Data processing
        for (uint16_t i = 0; i < datasets_count; i++) {
            uint16_t base = i * 4;
            uint8_t up    = raw_gesture_data[base + 0];
            uint8_t down  = raw_gesture_data[base + 1];
            uint8_t left  = raw_gesture_data[base + 2];
            uint8_t right = raw_gesture_data[base + 3];
            
            // Your gesture recognition math here...
        }
    }
}
```

### Example 3: Writing a Configuration Block to EEPROM (24LC64 / 24LCxx)

Demonstrates using `i2c_write_buffer16` to write a block to an EEPROM with a 16-bit word address.

```c
#include "i2c.h"

#define EEPROM_I2C_ADDR    0x50     // 24LC64 base address
#define PAGE_START_ADDR    0x0020   // 16-bit memory cell address inside EEPROM

static const uint8_t calibration_table[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};

uint8_t save_calibration(void) {
    // Sends: START -> 0x50(TX) -> Addr_High -> Addr_Low -> Data[0..7] -> STOP
    return i2c_write_buffer16(EEPROM_I2C_ADDR, PAGE_START_ADDR, calibration_table, 8);
}

uint8_t load_calibration(uint8_t *p_buf) {
    // Sends: START -> 0x50(TX) -> Addr_High -> Addr_Low -> Repeated START -> 0x50(RX) -> Read[0..7] -> STOP
    return i2c_read_buffer16(EEPROM_I2C_ADDR, PAGE_START_ADDR, p_buf, 8);
}
```

### Example 4: Working with the DAC7571 (Raw API)

The DAC7571 (Texas Instruments) is a 12-bit single-channel DAC without internal register addresses. It expects the master to transmit 2 data bytes immediately after the I2C address.

With version 7.0.0, we use `i2c_write_raw()` directly:

```c
#include "i2c.h"

#define DAC7571_I2C_ADDR    0x4C

/**
 * @brief Set the output voltage on the DAC7571
 * @param data_12bit: value from 0 to 4095 (12 bits)
 * @return I2C_OK or error code
 */
uint8_t dac7571_set_voltage(uint16_t data_12bit) {
    if (data_12bit > 4095) data_12bit = 4095;

    uint8_t buf[2];
    buf[0] = (uint8_t)((data_12bit >> 8) & 0x0F); // Normal Mode + 4 MSBs
    buf[1] = (uint8_t)(data_12bit & 0xFF);        // 8 LSBs

    return i2c_write_raw(DAC7571_I2C_ADDR, buf, 2);
}
```

#### Practical Example: Sawtooth Wave Generation

Because version 7.0.0 removes unnecessary delays from `i2c_stop()`, the main loop can drive the DAC at maximum bus throughput:

```c
#include "i2c.h"

int main(void) {
    SystemCoreClockUpdate();
    i2c_init(400000); // 400 kHz Fast Mode
    
    uint16_t dac_value = 0;

    while(1) {
        dac7571_set_voltage(dac_value);
        
        dac_value += 4;
        if (dac_value >= 4096) {
            dac_value = 0;
        }
    }
}
```

---

## Bus Recovery Algorithm (Inside)

If an external device hangs mid-word and holds the SDA line LOW, the master hardware cannot generate a START or STOP condition. The driver solves this as follows:

1. `I2C1->CTLR1 &= ~I2C_CTLR1_PE;` fully disables the I2C hardware block.
2. PC1 and PC2 are switched to general-purpose open-drain output mode (`GPIO_PC1_PC2_OUT_OD_2M`).
3. The driver manually generates up to 16 clock pulses on SCL. After each pulse, it checks SDA. As soon as the slave releases SDA to HIGH, the loop terminates early. If Clock Stretching is active on SCL (slave holds SCL low), the master safely waits within `I2C_STRETCH_TIMEOUT`.
4. A valid STOP sequence is generated via GPIO: SCL LOW → SDA LOW → SCL HIGH → SDA HIGH.
5. A hard SWRST reset is issued to the I2C1 block.
6. PC1 and PC2 are reconfigured back to AF_OD alternate function mode.
7. The register restore function is called: clock control parameters are reloaded, and hardware ACK control is re-enabled.

---

## Installation and Integration

### Option 1: PlatformIO (Recommended)

Add the library to your `platformio.ini`:

```ini
lib_deps =
    https://github.com/tama18101971/I2C-CH32V003.git
```

### Option 2: Manual Integration

Copy `i2c.h` and `i2c.c` from the `src/` folder into your project.

Include the header:

```c
#include "i2c.h"
```

Initialize the bus in `main()`:

```c
i2c_init(400000); // Fast Mode 400 kHz (or 100000 for Standard Mode)
```

---

## Lite Mode (Flash Savings)

For applications with strict Flash budget limits, **Aggressive Lite** mode conditionally compiles out portions of the driver:

```ini
build_flags = -DI2C_LITE=1
```

Or selectively disable features:
```ini
build_flags =
    -DI2C_DISABLE_BUS_RECOVERY    ; removes i2c_bus_recovery()
    -DI2C_DISABLE_SCANNER         ; removes i2c_probe_address()
    -DI2C_DISABLE_BUFFER_API      ; removes i2c_write_buffer(), raw API, buffer16 API
    -DI2C_DISABLE_ERROR_COUNTER   ; removes consecutive_errors + handle_critical_error()
```

### What Lite keeps

| API | Lite | Full |
|---|:-:|:-:|
| `i2c_init`, `i2c_deinit` | + | + |
| `i2c_start`, `i2c_repeated_start`, `i2c_stop` | + | + |
| `i2c_send_addr`, `i2c_send_byte`, `i2c_wait_ack`, `i2c_write_byte` | + | + |
| `i2c_wait_bus_free` | + | + |
| `i2c_write_register`, `i2c_read_register` | + | + |
| `i2c_probe_address` (scanner) | **−** | + |
| `i2c_write_buffer`, `i2c_read_buffer` | **−** | + |
| `i2c_write_raw`, `i2c_read_raw` | **−** | + |
| `i2c_write_buffer16`, `i2c_read_buffer16` | **−** | + |
| `i2c_bus_recovery` (Clock Recovery) | **−** | + |
| `consecutive_errors` counter | **−** | + |

### Measured Flash Footprint (v7.0.0)

Measured with `pio run` on `genericCH32V003F4P6` with NoneOS SDK in release mode:

| Profile | Build flags | Flash | RAM | Flash change |
|---|---|---:|---:|---:|
| Full | — | 2268 B | 292 B | — |
| No recovery | `I2C_DISABLE_BUS_RECOVERY` | 1892 B | 292 B | **−376 B** |
| No error counter | `I2C_DISABLE_ERROR_COUNTER` | 2208 B | 292 B | −60 B |
| No buffer API | `I2C_DISABLE_BUFFER_API` | 2216 B | 292 B | −52 B |
| Lite | `I2C_LITE=1` | 1844 B | 292 B | **−424 B** |

---

## License

This library is released under the permissive MIT License. Use, modification, and redistribution are permitted in both open-source non-commercial and closed-source commercial industrial products with no royalties or restrictions.

[LICENSE](LICENSE)
