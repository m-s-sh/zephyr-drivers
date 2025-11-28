# Zephyr Drivers Collection

<img width="512" height="512" alt="logo" src="https://github.com/user-attachments/assets/ad247ae4-dd0d-491e-bb02-0653d4592d0b" />

A collection of reusable drivers and modules for Zephyr RTOS, designed for embedded systems.

## Overview

This project provides production-ready drivers for various hardware components, built as Zephyr modules that can be easily integrated into any Zephyr application.

## Drivers

### Status LED Driver

A simple and flexible status LED driver module supporting multiple LED states including solid on/off, blinking patterns, and heartbeat animation.

**Features:**

- Multiple LED states (off, on, slow/fast blink, heartbeat)
- Device tree integration
- Work queue based non-blocking operation
- Easy to use API

### SIM800L Modem Driver

Full-featured cellular modem driver for the SIMCOM SIM800L GSM/GPRS module with socket offloading support.

**Features:**

- BSD socket API offloading (TCP/UDP)
- DNS resolution
- Multi-socket support (5 concurrent connections)
- AT command interface
- Network registration and status monitoring
- Signal strength (RSSI) monitoring
- Power management support

### Raspberry Pi Pico PIO UART Enhanced Driver

A software UART driver using the RP2040 PIO, supporting configurable data bits and interrupt-driven RX with buffering.

**Features:**

- Configurable data bits (8/9/10/11/12/13/14/15/16)
- RX and TX via PIO state machines
- Device tree integration
- Interrupt-driven RX with software buffer
- RX interrupt enable/disable via API
- Efficient RX FIFO draining in ISR
- Zephyr UART API compatible (`poll_in_u16`, `poll_out_u16`, IRQ API)
- Multiple UART instances supported

## Hardware Requirements

- Raspberry Pi Pico (or compatible RP2040 board)
- Zephyr SDK 0.17.4 or later
- For SIM800L: SIMCOM SIM800L module with UART connection

## Project Structure

```text
workspace/
├── drivers/
│   ├── led/                      # Status LED driver
│   │   ├── status_led.c
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   ├── modem/simcom/             # SIM800L modem driver
│   │   ├── sim800l.c
│   │   ├── sim800l.h
│   │   ├── sim800l_offload.c
│   │   ├── sim800l_pdp.c
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   └── serial/                   # UART/PIO drivers
│       ├── uart_rpi_pico_pio_enhanced.c
│       ├── CMakeLists.txt
│       └── Kconfig
├── include/drivers/              # Driver header files
│   └── status_led.h
├── dts/bindings/                 # Device tree bindings
│   ├── led/
│   │   └── status-led.yaml
│   └── serial/
│       └── uart-rpi-pico-pio-enhanced.yaml
├── tests/                        # Test applications
│   ├── status-led/
│   ├── modem/
│   └── uart-pio/
└── README.md
```

## Building and Running

### Prerequisites

1. Install the Zephyr SDK and dependencies
2. Set up your development environment
3. Ensure your board is connected

### Build Commands

**Status LED Driver:**

```bash
west build -p auto -b rpi_pico tests/status-led
west flash
```

**SIM800L Modem Driver:**

```bash
west build -p auto -b rpi_pico tests/modem -S uart_serial_port
west flash
```

**RP2040 PIO UART Enhanced Driver:**

```bash
west build -p auto -b rpi_pico tests/uart-pio
west flash
```

Or use the provided VS Code tasks:

- `Build - Select Target` - Build with target selection
- `Flash` - Flash the built application
- `Clean` - Clean build directory

## Configuration

### Status LED Device Tree

Define your status LEDs in your board's device tree:

```dts
/ {
    status_led_a: status-led-a {
        compatible = "status-led";
        led-gpios = <&pico_header 25 GPIO_ACTIVE_HIGH>;
        label = "Status LED A";
    };

    status_led_b: status-led-b {
        compatible = "status-led";
        led-gpios = <&pico_header 24 GPIO_ACTIVE_HIGH>;
        label = "Status LED B";
    };
};
```

### SIM800L Modem Device Tree

Configure the SIM800L modem in your device tree overlay:

```dts
&uart0 {
    status = "okay";
    current-speed = <115200>;

    sim800l: sim800l {
        compatible = "simcom,sim800l";
        mdm-reset-gpios = <&gpio0 15 GPIO_ACTIVE_LOW>;
        status = "okay";
    };
};
```

### RP2040 PIO UART Enhanced Device Tree

Add a UART node using the enhanced PIO UART driver:

```dts
&pio0 {
    uart0: uart@0 {
        compatible = "raspberrypi,pico-uart-pio-enhanced";
        tx-pins = <0>;
        rx-pins = <1>;
        current-speed = <115200>;
        data-bits = <9>;
        status = "okay";
    };
};
```

### Kconfig Options

**SIM800L Driver:**

```kconfig
CONFIG_MODEM_SIM800L=y
CONFIG_MODEM_SIM800L_LOG_LEVEL_DBG=y
CONFIG_NET_SOCKETS_OFFLOAD=y
CONFIG_NET_OFFLOAD=y
```

**RP2040 PIO UART Enhanced:**

```kconfig
CONFIG_UART_RPI_PICO_PIO_ENHANCED=y
CONFIG_UART_RPI_PICO_PIO_ENHANCED_LOG_LEVEL_DBG=y
```

## Usage Examples

### Status LED Driver

```c
#include <drivers/status_led.h>

/* Get device from device tree */
const struct device *led_dev = DEVICE_DT_GET(DT_NODELABEL(status_led_a));

/* Check if device is ready */
if (!device_is_ready(led_dev)) {
    printk("LED device not ready\n");
    return;
}

/* Set LED states */
status_led_set_state(led_dev, STATUS_LED_ON);           // Solid on
status_led_set_state(led_dev, STATUS_LED_BLINK_FAST);   // Fast blink
status_led_set_state(led_dev, STATUS_LED_HEARTBEAT);    // Heartbeat pattern
status_led_set_state(led_dev, STATUS_LED_OFF);          // Turn off

// Get current state
enum status_led_state current = status_led_get_state(led_dev);
```

### SIM800L Modem Driver

```c
#include <zephyr/net/socket.h>

/* Create a TCP socket */
int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

/* Connect to remote server */
struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(80),
};
inet_pton(AF_INET, "93.184.216.34", &addr.sin_addr);
connect(sock, (struct sockaddr *)&addr, sizeof(addr));

/* Send HTTP request */
const char *request = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
send(sock, request, strlen(request), 0);

/* Receive response */
char buffer[512];
int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
buffer[len] = '\0';
printk("Response: %s\n", buffer);

/* Close socket */
close(sock);
```

### RP2040 PIO UART Enhanced Driver

```c
#include <zephyr/drivers/uart.h>

const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));

if (!device_is_ready(uart_dev)) {
    printk("UART device not ready\n");
    return;
}

uint16_t rx_word;
if (uart_poll_in_u16(uart_dev, &rx_word) == 0) {
    printk("Received: 0x%04x\n", rx_word);
}

uart_poll_out_u16(uart_dev, 0x1A2B);

// Enable RX interrupt
uart_irq_rx_enable(uart_dev);
// ... handle callback as usual ...
```

## API Reference

### Status LED Driver

**Functions:**

- `status_led_set_state(dev, state)` - Set LED to specified state
- `status_led_get_state(dev)` - Get current LED state

**States:**

- `STATUS_LED_OFF` - LED off
- `STATUS_LED_ON` - LED on
- `STATUS_LED_BLINK_SLOW` - 1Hz blink
- `STATUS_LED_BLINK_FAST` - 4Hz blink
- `STATUS_LED_HEARTBEAT` - Heartbeat pattern (double pulse every 3 seconds)

### SIM800L Modem Driver

**Socket API (Offloaded):**

- `socket()` - Create socket (TCP/UDP)
- `connect()` - Connect to remote host
- `send()` / `sendto()` - Send data
- `recv()` / `recvfrom()` - Receive data
- `close()` - Close socket
- `getaddrinfo()` - DNS resolution

**Features:**

- Up to 5 concurrent TCP/UDP connections
- Automatic network registration
- Signal strength monitoring
- PDP context management

### RP2040 PIO UART Enhanced Driver

**Functions:**

- `uart_poll_in_u16(dev, &word)` - Receive a word (8-16 bits)
- `uart_poll_out_u16(dev, word)` - Send a word (8-16 bits)
- `uart_irq_rx_enable(dev)` / `uart_irq_rx_disable(dev)` - Enable/disable RX interrupt
- `uart_irq_callback_set(dev, cb, user_data)` - Set IRQ callback

**Features:**

- RX interrupt only enabled when requested
- RX FIFO is drained in ISR and buffered in software
- Supports multiple UART instances

## Testing

**Status LED:**
The `tests/status-led` directory contains a comprehensive test application that cycles through all LED states with 10-second intervals for easy visual confirmation.

**SIM800L Modem:**
The `tests/modem` directory contains a test application that demonstrates:

- Network registration and connection
- DNS resolution
- TCP socket communication
- HTTP request/response
- Signal strength monitoring

**RP2040 PIO UART Enhanced:**
The `tests/uart-pio` directory contains a test application that demonstrates:

- Sending and receiving 8/9/10/11/12/13/14/15/16-bit words
- RX interrupt-driven reception and buffering
- Device tree and Kconfig configuration

## License

Copyright (c) 2025 Blue Vending
SPDX-License-Identifier: Apache-2.0
