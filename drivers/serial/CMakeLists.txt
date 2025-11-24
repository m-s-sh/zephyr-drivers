# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 Blue Vending

# PIO UART driver for Raspberry Pi Pico
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_UART_RPI_PICO_PIO_ENHANCED uart_rpi_pico_pio_enhanced.c)
