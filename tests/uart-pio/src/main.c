/*
 * Copyright (c) 2025 Blue Vending
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(uart_pio_test, LOG_LEVEL_DBG);

/* Get the PIO UART device */
#define PIO_UART_NODE DT_NODELABEL(pio1_uart0)
static const struct device *const pio_uart_device = DEVICE_DT_GET(PIO_UART_NODE);

/* Test data */
static char rx_buffer[256];
static size_t rx_count;

void uart_irq_callback(const struct device *dev, void *user_data)
{
	uint16_t c;
	ARG_UNUSED(user_data);

	if (uart_poll_in_u16(dev, &c) != 0) {
		return;
	}
	printk("%X\n", c);
}

int main(void)
{
	LOG_INF("PIO UART Test Application Starting");

	/* Check if device is ready */
	if (!device_is_ready(pio_uart_device)) {
		LOG_ERR("PIO UART device not ready");
		return -1;
	}

	LOG_INF("PIO UART device is ready");
	uart_irq_callback_set(pio_uart_device, uart_irq_callback);
	uart_irq_rx_enable(pio_uart_device);
	/* Keep running for continuous testing */
	while (1) {
		// read via polling
		uint16_t c;

		k_sleep(K_MSEC(100));
	}

	return 0;
}
