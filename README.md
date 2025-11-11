# Zephyr Drivers Collection

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

## Hardware Requirements

- Raspberry Pi Pico (or compatible RP2040 board)
- Zephyr SDK 0.17.4 or later
- For SIM800L: SIMCOM SIM800L module with UART connection

## Project Structure

```text
workspace/
├── drivers/
│   ├── led/              # Status LED driver
│   │   ├── status_led.c
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   └── modem/simcom/     # SIM800L modem driver
│       ├── sim800l.c
│       ├── sim800l.h
│       ├── sim800l_offload.c
│       ├── sim800l_pdp.c
│       ├── CMakeLists.txt
│       └── Kconfig
├── include/drivers/      # Driver header files
│   └── status_led.h
├── dts/bindings/         # Device tree bindings
│   └── led/
│       └── status-led.yaml
├── tests/                # Test applications
│   ├── status-led/
│   └── modem/
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
# Build status LED test
west build -p auto -b rpi_pico tests/status-led
west flash
```

**SIM800L Modem Driver:**

```bash
# Build modem test
west build -p auto -b rpi_pico tests/modem -S uart_serial_port
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

### Kconfig Options

**SIM800L Driver:**

```kconfig
CONFIG_MODEM_SIM800L=y
CONFIG_MODEM_SIM800L_LOG_LEVEL_DBG=y
CONFIG_NET_SOCKETS_OFFLOAD=y
CONFIG_NET_OFFLOAD=y
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

## License

Copyright (c) 2025 Blue Vending
SPDX-License-Identifier: Apache-2.0
