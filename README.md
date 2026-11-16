# Reliable I2C (I2C1) Bus Driver for CH32V003 (RISC-V) — Version 7.2.0

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

[🇷🇺 Русский](README.ru.md)

Hardware I2C1 driver for **CH32V003**, with bounded polling waits, error reporting and single-master bus recovery. Uses WCH NoneOS SDK registers and headers. Tested scenarios and limitations are documented below and in the hardware test report.

---

## What changed in 7.2.0

- Hardware errors take precedence over ready flags; probe propagates START and STOP failures.
- Receive calls wait for STOP completion; short critical sections restore the incoming IRQ mask.
- CCR rounds up, so the nominal bus frequency never exceeds the request. PCLK/HCLK support is conservatively limited to 8–48 MHz.
- Recovery checks SCL/SDA, uses at most nine pulses, performs SWRST only after physical release, and reports `I2C_ERR_RECOVERY` (6) if it fails. PE stays disabled until a successful init.
- ARLO returns without STOP or GPIO recovery. Automatic recovery is supported only on a single-master bus.
- A conservative NOP guard is applied after successful STOP; polling timeout budgets remain approximate and are per wait, not per transaction.
- The host model covers coexisting ready/error flags, POS, RX-stage faults, delayed STOP and IRQ masks. The standalone benchmark selects exactly one main file.
- Actual CH32V003F4P6 + APDS-compatible clone (ID 0x9E) results and limitations are recorded in [hardware results](test/hardware/RESULTS.md).

## Architectural Highlights

1. **C++ Compatibility:** Header `i2c.h` is enclosed in `extern "C"` blocks for C++ linkage. Integration requires compatible WCH NoneOS SDK headers and runtime; `extern "C"` alone does not provide support for arbitrary Arduino cores.
2. **Verified STOP:** writes and reads wait for idle, followed by a conservative tBUF guard. The full transaction is not limited to a single polling budget.
3. **Bounded polling waits:** loop counts scale with HCLK 8–48 MHz; actual time depends on build options and IRQ load. This is not an exact wall-clock deadline.
4. **Read-Only Workload Protection:** `consecutive_errors` counter resets upon successful `ADDR` phase ACK, preventing sporadic transient hardware errors from accumulating into false bus recovery triggers during long read sessions.
5. **Raw & 16-Bit Register APIs:** Native `i2c_write_raw()` / `i2c_read_raw()` for register-less devices (DAC7571) and `i2c_write_buffer16()` / `i2c_read_buffer16()` for memories with 16-bit word addresses (for example, 24LC64; bank selection remains the caller's responsibility).
6. **Granular Return Codes:** Distinct status codes (`I2C_ERR_TIMEOUT`, `I2C_ERR_BERR`, `I2C_ERR_ARLO`, `I2C_ERR_CLK`, `I2C_ERR_RECOVERY`, `I2C_NACK`, `I2C_OK`) with optional legacy fallback `-DI2C_LEGACY_STATUS=1`.
7. **Bus Speed Validation:** `i2c_init()` validates speed up to 400 kHz and returns `I2C_ERR_CLK` for out-of-spec configurations.
8. **Symbol encapsulation:** The `i2c_bus_recovery` function is declared `static`, isolated inside `i2c.c` and not exported externally.
9. **Instant timeout recovery:** Stuck bus loops immediately trigger hardware peripheral recovery without waiting for multiple attempts.
10. **Separate transmission and ACK waits:** `i2c_write_byte()` combines `i2c_send_byte()` and `i2c_wait_ack()`; the wrapper does not mask interrupts or make the driver reentrant.
11. **RX error state:** receive cleanup restores ACK/POS; failed recovery leaves PE disabled and requires successful init.
12. **Buffer lengths:** the API accepts `uint16_t` (0–65535); buffers belong to the caller and the driver allocates no heap memory.

---

## Usage Contracts

Single-master bus only. PCLK/HCLK must be 8–48 MHz; reinitialize after a clock change. `I2C_ERR_RECOVERY` leaves PE off and GPIO outputs released. All nonzero returns invalidate a read result, even if part of its buffer was filled. `I2C_LEGACY_STATUS=1` maps recovery failures to NACK too.


These are hard requirements, not recommendations. They are also documented at the top of `i2c.h`.

1. **Single execution context.** The driver keeps state in static variables and owns the I2C1 block exclusively. Functions are **not reentrant** and **must not be called from an ISR** while another call is in progress. Concurrency, if needed, is the caller's responsibility.
2. **Interrupts during reads.** The `ADDR`-clear → `STOP`/`ACK=0` sequences must finish before the byte currently on the wire completes. Default builds leave them unprotected, which is safe for pure polling. If your application has interrupts longer than one byte time (≈90 µs at 100 kHz, ≈23 µs at 400 kHz), build with `-DI2C_ATOMIC_CRITICAL=1`.
3. **Pins.** Default I2C1 mapping is assumed (`AFIO_PCFR1.I2C1_RM[22,1] = 00`, SDA = PC1, SCL = PC2). The driver never reads or writes AFIO remap registers; with remap enabled it would configure the wrong pins.
4. **Pointers are not NULL-checked** (except the explicitly optional `i2c_probe_address` outputs). Passing an invalid buffer with `len > 0` is undefined behavior.
5. **Call order.** A successful `i2c_init()` is required before bus operations; internal timeouts and delays are computed there.
6. **`len == 0` semantics.** Reads (`i2c_read_buffer` / `i2c_read_raw` / `i2c_read_buffer16`) return `I2C_OK` without touching the bus. Writes perform a full `START` + device address `[+ register address]` + `STOP` transaction, which is the normal way to position a register pointer or probe for a device.

---

## Physical Layer and Wiring

The driver operates with the **I2C1** hardware block on the controller's dedicated pins:

* **PC1 — SDA** (Mode: Alternate Function Open-Drain, 30MHz)
* **PC2 — SCL** (Mode: Alternate Function Open-Drain, 30MHz)

> **Pull-ups:** external pull-ups must use a voltage acceptable to every device. Choose resistance from bus capacitance, sink current and rise-time limits; 2.2–4.7 kΩ does not guarantee Fast-mode at every load. AF_OD does not enable an internal pull-up.

---

## Status Codes

| Code | Value | Description |
|---|:---:|---|
| `I2C_OK` | `0` | Success / Acknowledged (ACK) |
| `I2C_NACK` | `1` | Not acknowledged (NACK from slave) or generic error |
| `I2C_ERR_TIMEOUT` | `2` | Software timeout waiting for bus release or peripheral flag |
| `I2C_ERR_CLK` | `3` | Invalid clock configuration (PCLK1 outside 8..48 MHz, speed > 400 kHz, speed == 0, or CCR outside its hardware range) |
| `I2C_ERR_BERR` | `4` | Bus Error (misplaced START or STOP condition) |
| `I2C_ERR_ARLO` | `5` | Arbitration Lost (multi-master collision) |
| `I2C_ERR_RECOVERY` | `6` | Recovery failed or PE is disabled; remove the fault and successfully reinitialize |

*All non-zero codes evaluate to true on `if (err != I2C_OK)` checks.* To map `TIMEOUT`, `BERR`, `ARLO` and `RECOVERY` into `I2C_NACK` (`ERR_CLK` stays distinct), define `-DI2C_LEGACY_STATUS=1`.

For the precise hardware cause of a failure use `i2c_get_last_star1()`: it returns the `STAR1` snapshot taken *before* the driver cleared the error flags. Reading `I2C1->STAR1` yourself after a failed call gives a clean register, because `AF`/`BERR`/`ARLO` have already been acknowledged.

```c
uint8_t value = 0;
uint8_t res = i2c_read_register(0x39, 0x92, &value);
if (res != I2C_OK) {
    uint16_t star1 = i2c_get_last_star1();
    if (star1 & I2C_STAR1_AF)   { /* slave did not acknowledge */ }
    if (star1 & I2C_STAR1_BERR) { /* misplaced START/STOP */ }
    if (star1 & I2C_STAR1_ARLO) { /* arbitration lost */ }
}
```

---

## Build-Time Configuration

| Macro | Default | Effect |
|---|:---:|---|
| `I2C_TIMEOUT_MS` | `40` | Approximate per-wait polling budget in ms; build/IRQ dependent |
| `I2C_STRETCH_TIMEOUT_US` | `1000` | Approximate SCL-high polling budget during recovery, microseconds |
| `I2C_MAX_ERROR_COUNT` | `2` | BERR events between successful phases before recovery; ARLO is separate |
| `I2C_ATOMIC_CRITICAL` | `0` | `1` — run `ADDR`-clear sequences with interrupts disabled |
| `I2C_INTER_FRAME_DELAY_US` | `50` | Spacing between probes in `i2c_probe_address()` |
| `I2C_LEGACY_STATUS` | `0` | `1` — map timeout/BERR/ARLO/recovery to NACK; ERR_CLK remains distinct |
| `I2C_FIXED_PCLK_HZ` | unset | Compile against a fixed `PCLK1`; skips `SystemCoreClock` read; clock range is checked at compile time |
| `I2C_LITE` | `0` | `1` — aggressive size mode, see [Lite Mode](#lite-mode-flash-savings) |
| `I2C_DISABLE_BUS_RECOVERY` | unset | Remove GPIO clock recovery |
| `I2C_DISABLE_SCANNER` | unset | Remove `i2c_probe_address()` |
| `I2C_DISABLE_BUFFER_API` | unset | Remove buffer/raw/buffer16 API |
| `I2C_DISABLE_ERROR_COUNTER` | unset | Remove `consecutive_errors` (recovery on every fault) |
| `I2C_DISABLE_LAST_ERROR` | unset | Remove `i2c_get_last_star1()` and its storage |

`I2C_TIMEOUT` and `I2C_STRETCH_TIMEOUT` remain defined for source compatibility but are unused by the driver; they will be removed in 8.0.0.

---

## API Reference

### Low-Level Bus Management and Initialization Functions
* `uint8_t i2c_init(uint32_t bound);`
  Enables RCC/GPIO, verifies physical release before SWRST and configures I2C1. PCLK/HCLK must be 8–48 MHz; CCR rounds up for rates up to 400 kHz. Returns OK, ERR_CLK or a bus readiness/recovery error.
* `void i2c_deinit(void);`
  Call on an idle bus. Disables I2C1 and its APB1 clock, and puts PC1/PC2 into a high-impedance state. There is no return status; an internal wait failure is not reported.
* `uint8_t i2c_wait_bus_free(void);`
  Waits for BUSY/MSL/STOP clear with hardware-error precedence. ARLO returns without STOP/recovery; a timeout invokes checked recovery in full builds and returns an error in Lite.
* `uint8_t i2c_start(void);`
  Generates a START condition on the bus with a preliminary bus availability check. Timeout-limited.
* `uint8_t i2c_repeated_start(void);`
  Generates a Repeated START without checking the `BUSY` flag. Used when switching from register address write to data read.
* `uint8_t i2c_stop(void);`
  Sets the `STOP` bit and waits for the bus to clear. Returns `I2C_OK` on success. Timeout recovery depends on the build; ARLO returns without STOP/recovery.
* `uint8_t i2c_probe_address(uint8_t addr, uint16_t *p_star1, uint16_t *p_star2);`
  Probes a 7-bit address; returns OK only after a successful STOP, otherwise NACK or the START/STOP/recovery failure. Optional output pointers may be NULL. Applies the scanner interval after the attempt.
* `uint16_t i2c_get_last_star1(void);`
  Returns the snapshot of the last detected STAR1/timeout failure before clearing flags. Clock/lifecycle failures without a STAR1 poll do not update it. Removed by I2C_DISABLE_LAST_ERROR/Lite.

### Data Transfer Functions
* `uint8_t i2c_send_addr(uint8_t addr, uint8_t direction);`
  Sends a 7-bit device address shifted left, combined with the direction bit (`I2C_DIR_TX` or `I2C_DIR_RX`). Enforces `direction & 1` masking. Clears the `ADDR` flag by reading `STAR1` and `STAR2` registers and resets `consecutive_errors`.
  Use the high-level read functions for one/two-byte reads: they require special ACK/POS ordering around ADDR clearing.
* `uint8_t i2c_send_byte(uint8_t data);`
  *Low-level function.* Writes a byte to `DATAR` and waits only for transmit buffer release (`TXE`). **Does not check physical reception by the slave!**
* `uint8_t i2c_wait_ack(void);`
  *Low-level function.* Waits for byte transfer complete (`BTF`) or acknowledgment failure (`AF`) flag. Resets the error counter on success.
* `static inline uint8_t i2c_write_byte(uint8_t data);`
  *Inline wrapper.* Combines `i2c_send_byte` and `i2c_wait_ack` without masking interrupts.

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
  Sequential write with a 16-bit big-endian register/word address `reg_addr` (for example, 24LC64).
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
                printf("-- "); // NACK or another error; inspect the status for diagnostics
            }
        }
        printf("\n");
    }
    printf("--- found: %d ---\n", found);
}
```

### Example 2: Reading the APDS-9960 Gesture Sensor FIFO Buffer

This example assumes the gesture engine and FIFO are already configured. Each dataset contains four bytes. FIFO consumption and gesture recognition were not part of the reported clone hardware tests.

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

For EEPROMs, the caller must handle page boundaries, bank-select address bits and the model-specific write cycle. A successful STOP does not mean internal programming has finished; wait for readiness before reading or writing again (for example, ACK polling with an overall timeout). The driver does not split writes into pages. This example was not hardware-tested.

### Example 4: Working with the DAC7571

The DAC7571 (Texas Instruments) has one important quirk: unlike most sensors, it has no internal register addresses. This 12-bit single-channel DAC expects the master to transmit exactly 2 data bytes immediately after its I2C address. Therefore the standard `i2c_write_register()` won't work here (it sends 3 bytes: address → register → data).

#### DAC7571 Write Protocol

After sending the device address (0x4C or 0x4D), the DAC expects two bytes:

- **Byte 1 (MSB):** `[ 0 | 0 | PD1 | PD0 | D11 | D10 | D9 | D8 ]` — power control bits + 4 MSBs of data.
- **Byte 2 (LSB):** `[ D7  | D6  | D5 | D4 | D3  | D2  | D1  | D0 ]` — 8 LSBs of data.

For normal operation, Power-Down bits (PD1, PD0) must be 00. Bit layout: [TI DAC7571, Table 1](https://www.ti.com/lit/ds/symlink/dac7571.pdf). The DAC examples were not hardware-tested.

#### Preferred Approach: Raw API (v7.0.0)

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

#### Alternative: Low-Level API (required in Lite Mode)

Lite builds (`I2C_LITE=1` / `I2C_DISABLE_BUFFER_API`) compile out the raw and buffer APIs. The open low-level sequence remains available and is the recommended path there:

```c
#include "i2c.h"

// DAC7571 address depends on the A0 pin:
// If A0 is tied to GND, 7-bit address = 0x4C (binary: 1001100)
// If A0 is tied to VDD, 7-bit address = 0x4D (binary: 1001101)
#define DAC7571_I2C_ADDR    0x4C

/**
 * @brief Set the output voltage on the DAC7571
 * @param data_12bit: value from 0 to 4095 (12 bits)
 * @return I2C_OK or error code
 */
uint8_t dac7571_set_voltage_ll(uint16_t data_12bit) {
    if (data_12bit > 4095) {
        data_12bit = 4095;
    }

    uint8_t byte_msb = (uint8_t)((data_12bit >> 8) & 0x0F);
    uint8_t byte_lsb = (uint8_t)(data_12bit & 0xFF);

    uint8_t res = i2c_start();
    if (res != I2C_OK) return res;

    if ((res = i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte(byte_msb)) != I2C_OK ||
        (res = i2c_write_byte(byte_lsb)) != I2C_OK) return res;

    return i2c_stop();
}

/**
 * @brief Put the DAC into power-down mode to save energy
 * @param mode: 1 - 1 kΩ pull-down to GND, 2 - 100 kΩ pull-down to GND, 3 - High-Z
 */
uint8_t dac7571_power_down(uint8_t mode) {
    if (mode < 1 || mode > 3) return I2C_NACK;

    // PD1:PD0 occupy bits 5:4 of the first byte
    uint8_t byte_msb = (uint8_t)(mode << 4);

    uint8_t res = i2c_start();
    if (res != I2C_OK) return res;
    if ((res = i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX)) != I2C_OK ||
        (res = i2c_write_byte(byte_msb)) != I2C_OK ||
        (res = i2c_write_byte(0x00)) != I2C_OK) return res;

    return i2c_stop();
}
```

#### Practical Example: Sawtooth Wave Generation

Each successful write waits for STOP completion and the driver's conservative tBUF guard:

```c
#include "i2c.h"

int main(void) {
    SystemCoreClockUpdate();
    if (i2c_init(400000) != I2C_OK) {
        while (1) { /* initialization failed */ }
    }
    
    uint16_t dac_value = 0;

    while(1) {
        if (dac7571_set_voltage(dac_value) != I2C_OK) {
            /* Handle the error; a failed recovery requires reinitialization. */
            break;
        }
        
        dac_value += 4;
        if (dac_value >= 4096) {
            dac_value = 0;
        }
    }
}
```

---

## Bus Recovery Algorithm (Inside)

Only for a single-master bus. The driver disables PE, preloads released GPIO output latches, then selects open-drain GPIO mode. Each SCL-high wait is bounded and checked. While SDA is low it issues at most nine SCL pulses; it proceeds to GPIO STOP and SWRST only after both lines are released. Configuration, PE and idle state are then restored and checked.

If physical release, reconfiguration or idle verification fails, GPIO outputs remain released, PE remains off, and the call returns `I2C_ERR_RECOVERY` (legacy: `I2C_NACK`). Remove the fault and call `i2c_init()` again. A reset cannot fix an externally held line. Lite/no-recovery builds return their error without GPIO recovery and may require explicit reinitialization. The saved STAR1 preserves the primary error even if cleanup fails.

## Installation and Integration

### Option 1: PlatformIO (Recommended)

Add the library to your `platformio.ini`:

```ini
lib_deps =
    https://github.com/tama18101971/I2C-CH32V003.git#v7.2.0
```

Tested environment: PlatformIO Core 6.2.0, `framework = noneos-sdk`, board `genericCH32V003F4P6`. The tested platform and SDK revisions are pinned in the [root platformio.ini](platformio.ini) and example configurations.

### Option 2: Manual Integration

Copy `i2c.h` and `i2c.c` from the `src/` folder into your project.

Include the header:

```c
#include "i2c.h"
```

Initialize the bus in `main()`:

```c
if (i2c_init(400000) != I2C_OK) {
    /* Handle initialization failure before using the bus. */
}
```

And enable LTO in the environment section of `platformio.ini` to save Flash (see the footprint table for measured values):

```ini
board_build.use_lto = yes
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
    -DI2C_DISABLE_LAST_ERROR      ; removes i2c_get_last_star1()
```

Independently of these flags, enable link-time optimization — it costs nothing and saves Flash in the measured builds (264 B for Register full and 124 B for Register Lite):

```ini
board_build.use_lto = yes
```

> Note: on the `ch32v` platform LTO must be requested via `board_build.use_lto`. Passing `-flto` through `build_flags` has no effect, because the flag then reaches the compiler but not the linker.

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
| `i2c_get_last_star1` | **−** | + |
| `i2c_bus_recovery` (Clock Recovery) | **−** | + |
| `consecutive_errors` counter | **−** | + |

### Measured Flash Footprint (v7.2.0)

GCC 12.2.0, CH32V003F4P6, LTO; ELF section sums match the binaries. Conditions and complete table: [results](test/hardware/RESULTS.md).

| Profile | Flash | RAM including 256-byte stack reserve |
|---|---:|---:|
| Register API full | 2536 B | 296 B |
| Register API no recovery | 2076 B | 292 B |
| Register API no error counter | 2464 B | 296 B |
| Register API no buffer | 2536 B | 296 B |
| Register API Lite | 2076 B | 292 B |
| All API full | 2976 B | 296 B |
| All API no buffer | 2684 B | 296 B |
| All API no scanner | 2928 B | 296 B |
| All API Lite | 2156 B | 292 B |
| Register full without LTO | 2800 B | 296 B |
| Register Lite without LTO | 2200 B | 292 B |

## Testing

Run `pwsh test/run_tests.ps1`: up to 46 tests per configuration, 396 test-function executions across ten profiles, with `-Wall -Wextra -Werror`. Boundary reads include 0/1/2/3/4/5/8/16/255/65535 bytes with an extra slave byte and buffer guards. The model also checks ready+error, START/STOP propagation, RX-stage faults, failed SCL/SDA recovery, ARLO ownership and incoming IRQ masks. It is a discrete register model, not a cycle-accurate or electrical simulator.

The hardware commands below target Windows with PlatformIO and pyserial (using PlatformIO's Python environment).

Actual board tests use `test/hardware/apds9960.c` and COM6 through WCH-Link UART (PD5 TX). With the appropriate APDS-compatible module on PC1 SDA / PC2 SCL:

```powershell
# This programs the board and temporarily writes/restores ENABLE + ALS thresholds.
& "$HOME/.platformio/penv/Scripts/python.exe" test/hardware/run_hil.py --env hardware_apds
& "$HOME/.platformio/penv/Scripts/python.exe" test/hardware/run_hil.py --env hardware_apds_polling
```

[Hardware results and raw logs](test/hardware/RESULTS.md) distinguish software assertions from waveform qualification. Real tests cover 15 CPU/rate combinations, reads 1/2/3/4/5/8/16, data readback, IRQ load, NACK, BTF timeout/recovery and sensor-setting restoration. Physical stuck-line injection, multi-master ARLO, 16-bit-address EEPROMs, analog timing and a 24-hour soak require additional fixtures; no such results are claimed.

## License

This library is released under the permissive MIT License. Use, modification and redistribution are permitted, including commercial use, subject to the MIT license notice and disclaimer requirements.

[LICENSE](LICENSE)
