# Reliable I2C (I2C1) Bus Driver for CH32V003 (RISC-V) — Version 6.0.0 (I2C Audit)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

[🇷🇺 Русский](README.ru.md)

A high-reliability, fault-tolerant, memory-optimized I2C driver for **CH32V003** series microcontrollers. Designed as a complete replacement for the standard WCH EVT library, which is prone to hard lockups in infinite polling loops (`while(!I2C_CheckEvent(...))`) caused by electromagnetic interference, slave power dips, or bus line shorts.

---

## Architectural Improvements

Version 6.0.0 delivers a **~25% flash-footprint reduction** (see [CHANGELOG](CHANGELOG.md))
on top of a driver already hardened against bus faults and hard lockups:

1. **Symbol encapsulation:** The `i2c_bus_recovery` function is declared `static`, isolated inside `i2c.c` and not exported externally, minimizing the global symbol graph and maximizing GCC inlining opportunities.
2. **Instant timeout recovery:** Fault handling is split into two independent loops:
   * **Software timeouts (stuck loop):** If the bus is blocked (`BUSY` flag stuck high) or the state machine hangs at `START`/`ADDR`, the driver **immediately** initiates a hardware reset without waiting for repeated errors.
   * **Hardware faults (`BERR`, `ARLO`):** For sporadic hardware errors (bus error, arbitration loss), an accumulator counter `consecutive_errors` with limit `MAX_ERROR_COUNT = 2` is retained.
3. **Safe low-level API (`i2c_send_byte`):** The byte send function is cleanly separated from acknowledgment waiting. The documentation strictly warns: `i2c_send_byte` only controls `DATAR` buffer release (`TXE`) but does not complete the physical bus transfer. For atomic sends, the inline function `i2c_write_byte()` is provided.
4. **Absolute ACK bit protection on faults:** In all software timeout branches of read functions (`i2c_read_register` and `i2c_read_buffer` for all packet lengths: `len==1`, `len==2`, `len>=3`), the `ACK = 1` bit is forcibly restored before exit. This guarantees a transaction that fails mid-read will not break subsequent bus calls.
5. **RISC-V (RV32EC) data type optimization:** All buffer length parameters are typed as `uint16_t` instead of `uint32_t`. For a microcontroller with 16 KB Flash and a limited register set, this reduces function call overhead and binary code size.
6. **Critical path inlining:** The `handle_critical_error` function is declared `static inline`, eliminating extra code jumps (`jal`/`jr`) in error handlers.
7. **Magic number elimination:** The clock stretching hold time is defined as a named constant `#define I2C_STRETCH_TIMEOUT 1000`.
8. **Compact, predictable micro-delays:** Heavy, unpredictable `for` loops with `volatile uint32_t` are replaced by a compact delay generator based on `__asm volatile("nop")` instructions, guaranteeing stable GPIO pulse timing regardless of compiler optimization level (`-O0`, `-Os`, `-O3`).
9. **Full high-level API symmetry:** A complete sequential buffer write function `i2c_write_buffer()` is added. Both register write and buffer write now use a unified, byte-by-byte control mechanism underneath.
10. **Unified STAR1 wait automaton (v6.0.0):** A single internal helper `i2c_wait_star1_flag()`
    centralizes flag polling, `BERR`/`ARLO` detection, `AF` handling and timeout recovery across
    all wait loops, eliminating ~250–350 B of duplicated machine code.
11. **Optimized delays & clock setup (v6.0.0):** `i2c_usleep()`/`i2c_delay()` use register-backed
    `nop` loops without stack-backed counters or runtime 32-bit division (frequency is read from the
    programmed `I2C1->CTLR2`); `i2c_configure_registers()` computes the CCR divisor in a single path.
12. **Compact GPIO management & unified build (v6.0.0):** A single `i2c_set_gpio_mode()` helper
    removes repeated `GPIOC->CFGLR` read-modify-write sequences, and a root `platformio.ini`
    builds every example/benchmark with one command.

---

## Physical Layer and Wiring

The driver operates with the **I2C1** hardware block on the controller's dedicated pins:

* **PC1 — SDA** (Mode: Alternate Function Open-Drain, 50MHz)
* **PC2 — SCL** (Mode: Alternate Function Open-Drain, 50MHz)

> **IMPORTANT:** For proper bus operation, both lines (SDA and SCL) **must** have external pull-up resistors in the range of **2.2 kΩ to 4.7 kΩ** connected to the 3.3V supply. The MCU's internal pull-up cannot provide sufficient edge slope at frequencies above 10 kHz.

---

## API Reference

### Low-Level Bus Management and Initialization Functions
* `uint8_t i2c_init(uint32_t bound);`
  Performs an I2C1 block reset via `SWRST`, configures GPIO and peripheral clocking, and calculates `CTLR2` and `CKCFGR` register values for Standard Mode (up to 100 kHz) or Fast Mode (up to 400 kHz) based on the current `SystemCoreClock`. Automatically sets the mandatory bit 14 in `OADDR1`. Returns `I2C_OK` or `I2C_ERR_CLK` if `SystemCoreClock` is outside the valid 2..48 MHz range.
* `void i2c_deinit(void);`
  Disables the I2C1 peripheral, deactivates the APB1 bus clock, and puts pins PC1/PC2 into a high-impedance state.
* `uint8_t i2c_wait_bus_free(void);`
  Polls the `BUSY` flag. Also detects `BERR`/`ARLO` hardware errors inside the loop for immediate recovery. If the flag is not cleared within `I2C_TIMEOUT`, the function emergency-calls `i2c_bus_recovery`. Returns `I2C_OK` or `I2C_NACK`.
* `uint8_t i2c_start(void);`
  Generates a START condition on the bus with a preliminary bus availability check. Timeout-limited.
* `uint8_t i2c_repeated_start(void);`
  Generates a Repeated START without checking the `BUSY` flag. Used when switching from register address write to data read.
* `uint8_t i2c_stop(void);`
  Sets the `STOP` bit. Best-effort: returns `I2C_OK` or `I2C_NACK` if bus release fails. On failure, triggers bus recovery internally.
* `uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2);`
  Probes a single 7-bit I2C address. Returns `I2C_OK` if the device ACKed, `I2C_NACK` otherwise. Optionally saves `STAR1`/`STAR2` register values for diagnostics (pass `NULL` to skip). Issues exactly one STOP on both success and failure.

### Data Transfer Functions
* `uint8_t i2c_send_addr(uint8_t addr, uint8_t direction);`
  Sends a 7-bit device address shifted left, combined with the direction bit (`I2C_DIR_TX` or `I2C_DIR_RX`). Clears the `ADDR` flag by reading `STAR1` and `STAR2` registers. Treats `AF` (address NACK) as non-critical.
* `uint8_t i2c_send_byte(uint8_t data);`
  *Low-level function.* Writes a byte to `DATAR` and waits only for transmit buffer release (`TXE`). **Does not check physical reception by the slave!**
* `uint8_t i2c_wait_ack(void);`
  *Low-level function.* Waits for byte transfer complete (`BTF`) or acknowledgment failure (`AF`) flag. Resets the error counter on success.
* `static inline uint8_t i2c_write_byte(uint8_t data);`
  *Atomic inline function.* Combines `i2c_send_byte` and `i2c_wait_ack`. Recommended for custom low-level sequences.

### High-Level Application API
* `uint8_t i2c_write_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);`
  Writes a single byte `value` to register `reg_addr` of device `dev_addr`.
* `uint8_t i2c_read_register(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_value);`
  Reads a single byte from a device register. Implements a safe sequence with `ACK` disabled and `STOP` set immediately after sending the read address.
* `uint8_t i2c_write_buffer(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *p_buf, uint16_t len);`
  Sequential write of data array `p_buf` of length `len` starting from register `reg_addr`. Useful for sending configuration tables or display data.
* `uint8_t i2c_read_buffer(uint8_t dev_addr, uint8_t reg_addr, uint8_t *p_buf, uint16_t len);`
  Multi-byte streaming read. Implements three distinct hardware algorithms (`len==1`, `len==2`, and `len>=3`) strictly per the I2C IP block vendor (Synopsys/WCH) specifications.

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

### Example 3: Writing a Configuration Block to EEPROM (24LCxx)

Demonstrates use of the symmetric `i2c_write_buffer` function to send a page of data to non-volatile memory.

```c
#include "i2c.h"

#define EEPROM_I2C_ADDR    0x50 // 24LC64 base address
#define PAGE_START_ADDR    0x00 // Memory cell address inside EEPROM

static const uint8_t calibration_table[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};

uint8_t save_calibration(void) {
    // The function automatically generates START, transmits the memory address,
    // sends the internal register/cell address PAGE_START_ADDR,
    // sequentially clocks out all 8 array bytes with per-byte ACK checking, and ends via STOP.
    return i2c_write_buffer(EEPROM_I2C_ADDR, PAGE_START_ADDR, calibration_table, 8);
}
```

### Example 4: Working with the DAC7571

The DAC7571 (Texas Instruments) has one important quirk you need to account for when using our driver.

Unlike most sensors (such as APDS-9960), the DAC7571 has no internal register addresses. This chip is a 12-bit single-channel DAC that expects the master to transmit exactly 2 data bytes immediately after its I2C address, containing the operating mode configuration and the 12-bit voltage value.

Therefore, the standard `i2c_write_register` function won't work (it sends 3 bytes: address → register → data). However, because we keep the low-level API open in `i2c.h`, working with the DAC7571 becomes straightforward and elegant.

#### DAC7571 Write Protocol

After sending the device address (0x4C or 0x4D), the DAC expects two bytes:

- **Byte 1 (MSB):** `[ PD1 | PD0 | 0 | 0 | D11 | D10 | D9 | D8 ]` — power control bits + 4 MSBs of data.
- **Byte 2 (LSB):** `[ D7  | D6  | D5| D4| D3  | D2  | D1  | D0 ]` — 8 LSBs of data.

For normal operation, Power-Down bits (PD1, PD0) must be 00.

#### Preferred Approach: Using the Low-Level API

This is the most transparent and architecturally correct way to work with register-less devices.

```c
#include "i2c.h"

// DAC7571 address depends on the A0 pin:
// If A0 is tied to GND, 7-bit address = 0x4C (binary: 1001100)
// If A0 is tied to VDD, 7-bit address = 0x4D (binary: 1001101)
#define DAC7571_I2C_ADDR    0x4C

/**
 * @brief Set the output voltage on the DAC7571
 * @param data_12bit: value from 0 to 4095 (12 bits)
 * @return I2C_OK or I2C_NACK
 */
uint8_t dac7571_set_voltage(uint16_t data_12bit) {
    // 1. Clamp to 12-bit resolution
    if (data_12bit > 4095) {
        data_12bit = 4095;
    }

    // 2. Form two bytes per TI specification
    // MSB: PD1=0, PD0=0 (Normal Mode), then 4 MSBs of data
    uint8_t byte_msb = (uint8_t)((data_12bit >> 8) & 0x0F);
    // LSB: remaining 8 data bits
    uint8_t byte_lsb = (uint8_t)(data_12bit & 0xFF);

    // 3. Low-level transaction with step-by-step error control
    if (i2c_start() != I2C_OK) return I2C_NACK;
    
    if (i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX) != I2C_OK) {
        // If the device is disconnected, i2c_send_addr generates STOP itself
        return I2C_NACK; 
    }
    
    // Send MSB and wait for ACK
    if (i2c_write_byte(byte_msb) != I2C_OK) return I2C_NACK;
    
    // Send LSB and wait for ACK
    if (i2c_write_byte(byte_lsb) != I2C_OK) return I2C_NACK;
    
    // Successful transaction complete
    i2c_stop();
    return I2C_OK;
}

/**
 * @brief Put the DAC into power-down mode to save energy
 * @param mode: 1 - 1 kΩ pull-down to GND, 2 - 100 kΩ pull-down to GND, 3 - High-Z
 */
uint8_t dac7571_power_down(uint8_t mode) {
    if (mode < 1 || mode > 3) return I2C_NACK;
    
    // Shift mode into PD1:PD0 bits (bits 6-7 of the first byte)
    uint8_t byte_msb = (mode << 6); 
    
    if (i2c_start() != I2C_OK) return I2C_NACK;
    if (i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX) != I2C_OK) return I2C_NACK;
    if (i2c_write_byte(byte_msb) != I2C_OK) return I2C_NACK;
    if (i2c_write_byte(0x00) != I2C_OK) return I2C_NACK; // Second byte is unused but required
    i2c_stop();
    
    return I2C_OK;
}
```

#### Alternative: "Trick" via i2c_write_buffer

If you want to use only high-level functions, you can outsmart the protocol. `i2c_write_buffer(dev_addr, reg_addr, p_buf, len)` sends `reg_addr` as the first data byte, then sends the array.

We can pass Byte 1 (MSB) as the `reg_addr` argument and Byte 2 (LSB) in a 1-byte buffer:

```c
uint8_t dac7571_set_voltage_via_buffer(uint16_t data_12bit) {
    if (data_12bit > 4095) data_12bit = 4095;

    uint8_t byte_msb = (uint8_t)((data_12bit >> 8) & 0x0F); // Goes in place of register address
    uint8_t byte_lsb = (uint8_t)(data_12bit & 0xFF);        // Goes in the buffer

    // Bus sequence will be perfect: START -> ADDR -> MSB -> LSB -> STOP
    return i2c_write_buffer(DAC7571_I2C_ADDR, byte_msb, &byte_lsb, 1);
}
```

This saves additional Flash bytes on CH32V003 by reusing the already-written `i2c_write_buffer` code.

#### Practical Example: Sawtooth Wave Generation

Since our driver is optimized for speed with no extra delays, you can cyclically update the DAC in the main loop, generating a high-frequency analog signal.

```c
#include "i2c.h"

int main(void) {
    // MCU initialization at 48 MHz (SystemInit)
    
    // Enable I2C at maximum speed Fast Mode (400 kHz)
    // Higher I2C speed = higher signal sampling rate!
    i2c_init(400000); 
    
    uint16_t dac_value = 0;

    while(1) {
        // Send current value to the DAC
        dac7571_set_voltage(dac_value);
        
        // Increment voltage value
        dac_value += 4; // Sawtooth step (tuned experimentally)
        
        // Reset to 0 at 12-bit max (4095)
        if (dac_value >= 4096) {
            dac_value = 0;
        }
        
        // No delays needed! The driver is already limited by the 400kHz bus speed.
        // On an oscilloscope you'll see a clean, smooth analog "sawtooth"
        // without a single bus hang.
    }
}
```

#### Driver Advantage for DACs (fault tolerance):

When generating a streaming analog signal (as above), the I2C bus is 100% loaded. If a power motor or relay activates nearby, the standard WCH EVT driver will hard-lock.

Our driver in that scenario:

1. Detects the timeout or BERR/ARLO error.
2. Within microseconds, resets the peripheral and clocks SCL via `__asm volatile("nop")`.
3. Immediately resumes wave generation. On an oscilloscope, it appears as a barely noticeable micro-glitch, but the system continues stably and never hangs.

## Bus Recovery Algorithm (Inside)

If an external device hangs mid-word and holds the SDA line LOW, the master hardware cannot generate a START or STOP condition. The driver solves this as follows:

1. `I2C1->CTLR1 &= ~I2C_CTLR1_PE;` fully disables the I2C hardware block.
2. PC1 and PC2 are switched to general-purpose open-drain output mode (GPIO_CFG_OUT_OD_2M).
3. The driver manually generates up to 16 clock pulses on SCL. After each pulse, it checks SDA. As soon as the slave releases SDA to HIGH, the loop terminates early. If Clock Stretching is active on SCL (slave holds SCL low), the master safely waits within `I2C_STRETCH_TIMEOUT`.
4. A valid STOP sequence is generated via GPIO: SCL LOW → SDA LOW → SCL HIGH → SDA HIGH.
5. A hard SWRST reset is issued to the I2C1 block.
6. PC1 and PC2 are reconfigured back to AF_OD alternate function mode.
7. The register restore function is called: clock control parameters are reloaded, and the hardware ACK control is re-enabled.

## Installation and Integration

### Option 1: PlatformIO (Recommended)

Add the library to your `platformio.ini`:

```ini
lib_deps =
    https://github.com/tama18101971/I2C-CH32V003.git
```

### Option 2: Manual Integration

Copy `i2c.h` and `i2c.c` from the `src/` folder into your project.

The clock frequency is automatically determined from `SystemCoreClock` — no manual configuration is needed.

Include the header:

```c
#include "i2c.h"
```

Initialize the bus in `main()`:

```c
i2c_init(400000); // Fast Mode 400 kHz (or 100000 for Standard Mode)
```

### Complete Examples

A full working I2C bus scanner example with a ready-made `platformio.ini` is located in
[`examples/i2c_scanner`](examples/i2c_scanner). A minimal flash-size benchmark lives in
[`examples/size_benchmark`](examples/size_benchmark).

The repository also ships a **unified root `platformio.ini`** — build any example or benchmark
directly from the root with a single command:

```sh
pio run -e scanner        # I2C bus scanner
pio run -e benchmark_full # flash-size benchmark (Full build)
pio run -e benchmark_lite # flash-size benchmark (Lite build)
```

---

## Lite Mode (Flash Savings)

For applications with strict Flash budget limits (CH32V003 has only 16 KB), an **Aggressive Lite** mode is provided. It conditionally compiles out portions of the driver — all timeouts remain in place, but on a hardware fault (`BERR`/`ARLO`) or bus stall, functions simply return `I2C_NACK` without attempting hardware bus recovery.

### Enabling

Add to your `platformio.ini`:
```ini
build_flags = -DI2C_LITE=1
```

Or selectively disable only what you don't need:
```ini
build_flags =
    -DI2C_DISABLE_BUS_RECOVERY    ; removes i2c_bus_recovery()
    -DI2C_DISABLE_SCANNER          ; removes i2c_probe_address()
    -DI2C_DISABLE_BUFFER_API      ; removes i2c_write_buffer()/i2c_read_buffer()
    -DI2C_DISABLE_ERROR_COUNTER   ; removes consecutive_errors + handle_critical_error()
```

`I2C_LITE=1` is equivalent to enabling all four `I2C_DISABLE_*` macros at once. By default (no macro defined or `I2C_LITE=0`), the full fault-tolerant version is built.

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
| `i2c_bus_recovery` (Clock Recovery) | **−** | + |
| `consecutive_errors` counter + `handle_critical_error` | **−** | + |

### Lite error handling behavior

All wait loops remain bounded by `I2C_TIMEOUT` — the driver **never hard-locks** (unlike the WCH EVT library). On `BERR`/`ARLO` or timeout, the function:
1. Clears the error flag.
2. Attempts `i2c_stop()` to release the bus (where appropriate).
3. Returns `I2C_NACK`.

If an external device holds SDA LOW after a fault, the bus stays `BUSY` until the next peripheral reset (`i2c_deinit()`/`i2c_init()` or power cycle). Hardware recovery (16 SCL pulses via GPIO) is only available in the full version.

### Measured Flash savings

The results below come from `pio run -d examples/size_benchmark` (or the equivalent
`pio run -e benchmark_*` environments in the root `platformio.ini`) on a
`genericCH32V003F4P6` with PlatformIO `ch32v 1.1.0`, the NoneOS SDK, and a
release build. The minimal benchmark deliberately excludes `printf`, UART, and
bus scanning while retaining `i2c_init`, `i2c_write_register`,
`i2c_read_register`, and `i2c_deinit` in the link.

| Profile | Build flags | Flash | RAM | Flash change |
|---|---|---:|---:|---:|
| Full | — | 2200 B | 284 B | — |
| No recovery | `I2C_DISABLE_BUS_RECOVERY` | 1900 B | 284 B | **−300 B** |
| No error counter | `I2C_DISABLE_ERROR_COUNTER` | 2124 B | 284 B | −76 B |
| No buffer API | `I2C_DISABLE_BUFFER_API` | 2168 B | 284 B | −32 B |
| Lite | `I2C_LITE=1` | 1808 B | 284 B | **−392 B** |

All `I2C_DISABLE_*` flags can be used independently: the no-error-counter
profile keeps recovery, but performs it immediately after a hardware error
instead of after two consecutive `BERR`/`ARLO` errors. Disabling GPIO bus recovery provides the largest reduction. The benchmark does
not call the buffer API, so linker garbage collection already removes nearly all
of it from the Full build. An application that calls `i2c_read_buffer()` or
`i2c_write_buffer()` will save more with `I2C_DISABLE_BUFFER_API`, but those APIs
will no longer be available.

For register-I/O-only applications (sensors, EEPROM, DACs), Lite mode frees
about 0.5 KiB of Flash. Re-run the measurement with your SDK/compiler version
from `examples/size_benchmark`.

### Compatibility

The `examples/i2c_scanner` example requires `i2c_probe_address` and **does not compile** in Lite mode — this is intentional: the user gets a clear compile/link error (`undefined reference to 'i2c_probe_address'`) rather than a silently degraded API.

## License

This library is released under the permissive MIT License. Use, modification, and redistribution are permitted in both open-source non-commercial and closed-source commercial industrial products with no royalties or restrictions.

[LICENSE](LICENSE)
