# PIO UART Interrupts Usage Guide

## Overview

The PIO UART driver supports interrupt-driven operation for efficient data reception without polling. This is particularly useful for applications that need to handle incoming UART data while performing other tasks.

## How PIO Interrupts Work

### 1. PIO Assembly Program

The RX assembly program includes an interrupt instruction:

```assembly
irq    nowait 4 rel     ; Trigger interrupt 4 when error detected
```

This instruction is triggered when:

- A framing error is detected (start bit not properly aligned)
- Invalid stop bit detected
- Any other reception error

### 2. Interrupt Flow

```
PIO State Machine → PIO Interrupt 4 → PIO0_IRQ_0 → Zephyr IRQ System → Driver Callback
```

### 3. Driver Implementation

The driver includes:

- **Interrupt Handler**: Reads data from PIO FIFO and buffers it
- **RX Buffer**: Circular buffer for storing received data
- **Callback System**: Notifies application when data arrives

## Using PIO Interrupts in Your Application

### 1. Basic Setup

```c
#include <zephyr/drivers/uart.h>

static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(pio1_uart0));

static void uart_callback(const struct device *dev, void *user_data)
{
    unsigned char c;

    if (uart_irq_rx_ready(dev)) {
        while (uart_poll_in(dev, &c) == 0) {
            // Process received character
            printk("Received: %c\n", c);
        }
    }
}

int main(void)
{
    // Set callback
    uart_irq_callback_user_data_set(uart_dev, uart_callback, NULL);

    // Enable RX interrupts
    uart_irq_rx_enable(uart_dev);

    // Your main application logic here
    return 0;
}
```

### 2. Advanced Usage with Buffering

```c
static char command_buffer[128];
static int cmd_index = 0;

static void uart_callback(const struct device *dev, void *user_data)
{
    unsigned char c;

    if (uart_irq_rx_ready(dev)) {
        while (uart_poll_in(dev, &c) == 0) {
            if (c == '\n' || c == '\r') {
                // Process complete command
                command_buffer[cmd_index] = '\0';
                process_command(command_buffer);
                cmd_index = 0;
            } else if (cmd_index < sizeof(command_buffer) - 1) {
                command_buffer[cmd_index++] = c;
            }
        }
    }
}
```

## Configuration

### Device Tree

```dts
&pio1 {
    status = "okay";

    pio1_uart0: uart0 {
        compatible = "raspberrypi,pico-uart-pio-enhanced";
        pinctrl-0 = <&pio1_uart0_default>;
        pinctrl-names = "default";
        current-speed = <9600>;
        data-bits = <9>;
        tx-pin = <26>;
        rx-pin = <27>;
        status = "okay";
    };
};
```

### Kconfig

Make sure these options are enabled:

```kconfig
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_PIO_RPI_PICO=y
```

## Performance Considerations

### Interrupt Latency

- PIO interrupts have very low latency (< 1µs typical)
- The driver buffers incoming data to prevent overflow
- Buffer size is 64 bytes by default

### CPU Usage

- Interrupt-driven mode uses significantly less CPU than polling
- Only processes data when it arrives
- Allows main application to run uninterrupted

### Error Handling

The RX program detects and signals:

- Framing errors (improper start/stop bits)
- Timing violations
- Buffer overflow conditions

## Debugging Tips

### 1. Check Interrupt Status

```c
if (uart_irq_is_pending(uart_dev)) {
    printk("UART interrupt pending\n");
}
```

### 2. Monitor RX Ready State

```c
if (uart_irq_rx_ready(uart_dev)) {
    printk("Data available in RX buffer\n");
}
```

### 3. Verify Device Tree

Ensure your device tree correctly defines:

- Pin assignments (tx-pin, rx-pin)
- Baud rate (current-speed)
- Data bits (data-bits)

### 4. Check PIO Resource Allocation

The driver uses:

- PIO0 block
- State machines 0 (TX) and 1 (RX)
- Interrupt 4

## Example Applications

- Serial command interface
- Data logging from sensors
- Protocol parsing (GPS, sensor data)
- Real-time communication

## Limitations

1. **TX Interrupts**: Not implemented in this version (uses polling for TX)
2. **Flow Control**: Hardware flow control not supported
3. **Multi-instance**: Limited by available PIO state machines

## See Also

- `tests/uart-pio/src/interrupt_example.c` - Complete working example
- Zephyr UART API documentation
- Raspberry Pi Pico PIO documentation
