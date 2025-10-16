# Status LED Driver for Zephyr

A simple and flexible status LED driver module for Zephyr RTOS, designed for the Raspberry Pi Pico platform.

## Overview

This project provides a reusable status LED driver that supports multiple LED states including solid on/off, blinking patterns, and heartbeat animation. It's built as a Zephyr module and can be easily integrated into any Zephyr application.

## Features

- **Multiple LED States:**
  - `STATUS_LED_OFF` - LED turned off
  - `STATUS_LED_ON` - LED solid on
  - `STATUS_LED_BLINK_SLOW` - Slow blinking (1Hz)
  - `STATUS_LED_BLINK_FAST` - Fast blinking (4Hz)
  - `STATUS_LED_HEARTBEAT` - Double-beat heartbeat pattern

- **Device Tree Integration** - Fully configurable via device tree
- **Work Queue Based** - Non-blocking operation using Zephyr's work queue system
- **Logging Support** - Integrated debug logging
- **Easy to Use API** - Simple get/set state functions

## Hardware Requirements

- Raspberry Pi Pico (or compatible RP2040 board)
- LEDs connected to GPIO pins
- Zephyr SDK 0.17.4 or later

## Project Structure

```text
workspace/
├── drivers/led/           # Status LED driver implementation
│   ├── status_led.c      # Main driver code
│   ├── CMakeLists.txt    # Build configuration
│   └── Kconfig           # Configuration options
├── include/drivers/      # Driver header files
│   └── status_led.h     # API definitions
├── dts/bindings/led/     # Device tree bindings
│   └── status-led.yaml  # Device tree binding
├── samples/status-led/   # Example application
│   └── src/main.c       # Sample test code
└── README.md            # This file
```

## Building and Running

### Prerequisites

1. Install the Zephyr SDK and dependencies
2. Set up your development environment
3. Ensure your board is connected

### Build Commands

```bash
# Clean previous build
west build -p auto -b rpi_pico samples/status-led

# Flash to device
west flash
```

Or use the provided VS Code tasks:

- `Build - Select Target` - Build with target selection
- `Flash` - Flash the built application
- `Clean` - Clean build directory

## Device Tree Configuration

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

## Usage Example

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
z_status_led_set_state(led_dev, STATUS_LED_ON);           // Solid on
z_status_led_set_state(led_dev, STATUS_LED_BLINK_FAST);   // Fast blink
z_status_led_set_state(led_dev, STATUS_LED_HEARTBEAT);    // Heartbeat pattern
z_status_led_set_state(led_dev, STATUS_LED_OFF);          // Turn off

/* Get current state */
enum status_led_state current = z_status_led_get_state(led_dev);
```

## API Reference

### Functions

- `z_status_led_set_state(dev, state)` - Set LED to specified state
- `z_status_led_get_state(dev)` - Get current LED state

### States

- `STATUS_LED_OFF` - LED off
- `STATUS_LED_ON` - LED on
- `STATUS_LED_BLINK_SLOW` - 1Hz blink
- `STATUS_LED_BLINK_FAST` - 4Hz blink
- `STATUS_LED_HEARTBEAT` - Heartbeat pattern (double pulse every 3 seconds)

## Testing

The `samples/status-led` directory contains a comprehensive test application that cycles through all LED states with 10-second intervals for easy visual confirmation.

## License

Copyright (c) 2025 Blue Vending
SPDX-License-Identifier: Apache-2.0
