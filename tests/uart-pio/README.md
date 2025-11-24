# PIO UART Test Application

This test application demonstrates and validates the Raspberry Pi Pico PIO UART driver with support for 7, 8, and 9-bit data configurations.

## Features

- **TX Test**: Sends test patterns using different data bit widths
- **RX Test**: Receives and echoes incoming UART data
- **Loopback Test**: Tests TX/RX functionality when pins are connected
- **Multi-bit support**: Tests 7, 8, and 9-bit data modes

## Hardware Setup

### Pin Configuration

- **TX Pin**: GP4
- **RX Pin**: GP5
- **Console UART**: GP0 (TX), GP1 (RX) - for logging

### Loopback Test Setup

For the loopback test to work, connect:

```
GP4 (TX) ⟷ GP5 (RX)
```

## Building and Running

```bash
# Build the test
west build -p auto -b rpi_pico/rp2040/w tests/uart-pio

# Flash to device
west flash

# Monitor console output
picocom /dev/ttyACM0 -b 115200
```

## Test Output

The application will output test results via the console UART:

```
[00:00:00.123,456] <inf> uart_pio_test: PIO UART Test Application Starting
[00:00:00.234,567] <inf> uart_pio_test: PIO UART device is ready
[00:00:01.345,678] <inf> uart_pio_test: === UART PIO TX Test ===
[00:00:01.456,789] <inf> uart_pio_test: Testing 7-bit ASCII mode:
[00:00:01.567,890] <inf> uart_pio_test: Sent: ABCDEFG
...
```

## External Testing

To test with an external device:

1. Connect your UART device to GP4 (RX) and GP5 (TX)
2. Configure the external device for 115200 baud, 8N1
3. The test will send data and echo received characters

## Troubleshooting

- **No output**: Check that the PIO driver is enabled (`CONFIG_UART_RPI_PICO_PIO_EX=y`)
- **Build errors**: Ensure the drivers module is properly included
- **Loopback fails**: Verify the TX-RX pin connection
- **Console issues**: Make sure console UART pins (GP0/GP1) are connected correctly
