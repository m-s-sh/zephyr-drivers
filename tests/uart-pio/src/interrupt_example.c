/*
 * Copyright (c) 2025 Blue Vending
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#define UART_DEVICE_NODE DT_NODELABEL(pio1_uart0)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

/* Buffer for incoming data */
static char rx_buffer[128];
static int rx_index = 0;

/* UART interrupt callback */
static void uart_irq_callback(const struct device *dev, void *user_data)
{
	unsigned char c;

	/* Check if we have received data */
	if (uart_irq_rx_ready(dev)) {
		/* Read all available characters */
		while (uart_poll_in(dev, &c) == 0) {
			if (rx_index < sizeof(rx_buffer) - 1) {
				rx_buffer[rx_index++] = c;

				/* Echo the character back */
				uart_poll_out(dev, c);

				/* Check for newline to process the command */
				if (c == '\n' || c == '\r') {
					rx_buffer[rx_index] = '\0';
					printk("Received: %s", rx_buffer);
					rx_index = 0;
				}
			}
		}
	}
}

int main(void)
{
	int ret;

	printk("PIO UART Interrupt Example\n");

	if (!device_is_ready(uart_dev)) {
		printk("UART device not ready\n");
		return -1;
	}

	/* Set up interrupt callback */
	uart_irq_callback_user_data_set(uart_dev, uart_irq_callback, NULL);

	/* Enable RX interrupts */
	uart_irq_rx_enable(uart_dev);

	printk("UART interrupts enabled. Type something and press Enter:\n");

	/* Send a test message */
	const char *test_msg = "Hello from PIO UART with interrupts!\n";
	for (int i = 0; test_msg[i]; i++) {
		uart_poll_out(uart_dev, test_msg[i]);
	}

	/* Main loop - the real work is done in interrupt handler */
	while (1) {
		k_sleep(K_MSEC(1000));
		printk("Heartbeat - waiting for UART data...\n");
	}

	return 0;
}
