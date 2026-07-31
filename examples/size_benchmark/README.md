# I2C size benchmark

This PlatformIO project measures a minimal firmware that links `i2c_init()`,
`i2c_write_register()`, `i2c_read_register()`, and `i2c_deinit()`.

It intentionally excludes `printf`, UART initialization, and I2C bus scanning.
Run all profiles from the repository root:

```sh
pio run -d examples/size_benchmark
```

The resulting size is reported by PlatformIO for each environment:

- `full` — all features enabled;
- `no_recovery` — `I2C_DISABLE_BUS_RECOVERY`;
- `no_error_counter` — `I2C_DISABLE_ERROR_COUNTER` (recovery stays enabled);
- `no_buffer` — `I2C_DISABLE_BUFFER_API`;
- `lite` — `I2C_LITE=1`.

`main.c` reads a zero-initialized `volatile` guard before invoking the register
functions. Therefore the linker retains those functions for size measurement,
while a normal run does not issue an I2C transaction or need a connected device.