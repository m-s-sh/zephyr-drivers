/*
 * Copyright (c) 2025 Blue Vending
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>

LOG_MODULE_REGISTER(pico_uart_pio_enhanced, CONFIG_UART_RPI_PICO_PIO_ENHANCED_LOG_LEVEL);

#define DT_DRV_COMPAT raspberrypi_pico_uart_pio_enhanced

#define SIDESET_BIT_COUNT 1 // Fixed: only need 1 bit for TX line
#define CYCLES_PER_BIT    8

RPI_PICO_PIO_DEFINE_PROGRAM(uart_tx, 0, 3,
			    /* .wrap_target */
			    0x9fa0, /*  0: pull   block           side 1 [7]  */
			    0xb722, /*  1: mov    x, y            side 0 [7]  */
			    0x6001, /*  2: out    pins, 1                     */
			    0x0642, /*  3: jmp    x--, 2                 [6]  */
			    /* .wrap */
);

RPI_PICO_PIO_DEFINE_PROGRAM(uart_rx, 0, 10,
			    //     .wrap_target
			    0x2020, //  0: wait   0 pin, 0
			    0xaa22, //  1: mov    x, y                   [10]
			    0x4001, //  2: in     pins, 1
			    0x0642, //  3: jmp    x--, 2                 [6]
			    0x00ca, //  4: jmp    pin, 10
			    0xc014, //  5: irq    nowait 4 rel
			    0xaf22, //  6: mov    x, y                   [15]
			    0x00c9, //  7: jmp    pin, 9
			    0x0047, //  8: jmp    x--, 7
			    0x0000, //  9: jmp    0
			    0x8020, // 10: push   block
				    //     .wrap
);

struct uart_pio_enhanced_config {
	const struct device *piodev;
	const struct pinctrl_dev_config *pcfg;
	const uint32_t tx_pin;
	const uint32_t rx_pin;
	uint8_t data_bits;
	uint32_t baud_rate;
	/* Remove dev pointer - not needed */
};

struct uart_pio_enhanced_data {
	PIO pio;
	uint irq_num;
	uint tx_sm;
	uint rx_sm;
	uint8_t data_bits;
	uart_irq_callback_user_data_t callback;
	void *callback_data;
	bool rx_irq_enabled;
};

#define MAX_UART_PIO_INSTANCES 8

static const struct device *uart_pio_instances[MAX_UART_PIO_INSTANCES];
static size_t uart_pio_instance_count;

static int uart_pio_enhanced_poll_in(const struct device *dev, uint16_t *p_u16)
{
	struct uart_pio_enhanced_data *data = dev->data;

	/* Check PIO FIFO directly */
	if (pio_sm_is_rx_fifo_empty(data->pio, data->rx_sm)) {
		return -1;
	}

	uint32_t raw = pio_sm_get(data->pio, data->rx_sm);
	*p_u16 = (raw >> (32 - data->data_bits)) & ((1 << data->data_bits) - 1);
	return 0;
}

static void uart_pio_enhanced_poll_out(const struct device *dev, uint16_t out_u16)
{
	struct uart_pio_enhanced_data *data = dev->data;

	pio_sm_put_blocking(data->pio, data->tx_sm, (uint32_t)out_u16);
}

/* PIO interrupt handler - called when RX FIFO has data */
static void uart_pio_enhanced_irq_handler(const struct device *dev)
{
	struct uart_pio_enhanced_data *data = dev->data;

	if (data->callback) {
		data->callback(dev, data->callback_data);
	}
}

static void uart_pio_enhanced_irq_handler_pio(const void *arg)
{
	PIO pio = (PIO)arg;

	for (size_t i = 0; i < uart_pio_instance_count; ++i) {
		struct uart_pio_enhanced_data *data = uart_pio_instances[i]->data;

		if (data->pio == pio && !pio_sm_is_rx_fifo_empty(data->pio, data->rx_sm)) {
			pio_interrupt_clear(pio, data->irq_num);
			uart_pio_enhanced_irq_handler(uart_pio_instances[i]);
			return;
		}
	}
}

static int connect_rx_irq(PIO pio, size_t sm)
{
	int irq_num = -EINVAL;

	if (pio == pio0_hw) {
		irq_num = PIO0_IRQ_0;
		IRQ_CONNECT(PIO0_IRQ_0, 0, uart_pio_enhanced_irq_handler_pio, pio0_hw, 0);
	} else if (pio == pio1_hw) {
		irq_num = PIO1_IRQ_0;
		IRQ_CONNECT(PIO1_IRQ_0, 0, uart_pio_enhanced_irq_handler_pio, pio1_hw, 0);
	} else {
		return -EINVAL;
	}

	irq_enable(irq_num);
	return irq_num;
}

// Initialize the rx pio state machine and IRQ 0
static int uart_pio_enhanced_rx_init(PIO pio, uint8_t pin, size_t sm, float div, uint8_t data_bits)
{
	LOG_INF("Initializing RX: pin=%d, sm=%d, div=%f, bits=%d", pin, sm, (double)div, data_bits);

	pio_sm_config c;

	if (!pio_can_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(uart_rx))) {
		return -EBUSY;
	}

	int offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(uart_rx));

	pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);

	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset + RPI_PICO_PIO_GET_WRAP_TARGET(uart_rx),
			   offset + RPI_PICO_PIO_GET_WRAP(uart_rx));

	sm_config_set_in_pins(&c, pin);
	sm_config_set_jmp_pin(&c, pin);
	sm_config_set_in_shift(&c, true, false, 32);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
	sm_config_set_clkdiv(&c, div);

	pio_sm_init(pio, sm, offset, &c);
	pio_sm_exec(pio, sm, pio_encode_set(pio_y, data_bits - 1));
	pio_sm_set_enabled(pio, sm, true);
	return 0;
}

static int uart_pio_enhanced_tx_init(PIO pio, uint8_t pin, size_t sm, float div, uint8_t data_bits)
{
	pio_sm_config c;

	if (!pio_can_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(uart_tx))) {
		return -EBUSY;
	}

	int offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(uart_tx));
	pio_sm_set_pins_with_mask(pio, sm, 1u << pin, 1u << pin);
	pio_sm_set_pindirs_with_mask(pio, sm, 1u << pin, 1u << pin);

	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset + RPI_PICO_PIO_GET_WRAP_TARGET(uart_tx),
			   offset + RPI_PICO_PIO_GET_WRAP(uart_tx));

	sm_config_set_out_shift(&c, true, false, 32);
	sm_config_set_out_pins(&c, pin, 1);
	sm_config_set_sideset_pins(&c, pin);
	sm_config_set_sideset(&c, SIDESET_BIT_COUNT, true, false);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&c, div);

	pio_sm_init(pio, sm, offset, &c);
	pio_sm_exec(pio, sm, pio_encode_set(pio_y, data_bits - 1));
	pio_sm_set_enabled(pio, sm, true);

	return 0;
}

int uart_pio_enhanced_init(const struct device *dev)
{
	const struct uart_pio_enhanced_config *config = dev->config;
	struct uart_pio_enhanced_data *data = dev->data;
	float div;
	size_t tx_sm;
	size_t rx_sm;
	PIO pio;
	int ret;

	LOG_INF("Initializing PIO UART: TX=%d, RX=%d, baud=%d, bits=%d", config->tx_pin,
		config->rx_pin, config->baud_rate, config->data_bits);

	pio = pio_rpi_pico_get_pio(config->piodev);
	div = (float)clock_get_hz(clk_sys) / (CYCLES_PER_BIT * config->baud_rate);

	ret = pio_rpi_pico_allocate_sm(config->piodev, &rx_sm);
	ret |= pio_rpi_pico_allocate_sm(config->piodev, &tx_sm);

	if (ret < 0) {
		return ret;
	}

	data->tx_sm = tx_sm;
	data->rx_sm = rx_sm;

	// Initialize RX
	ret = uart_pio_enhanced_rx_init(pio, config->rx_pin, rx_sm, div, config->data_bits);
	if (ret < 0) {
		LOG_ERR("Failed to initialize RX: %d", ret);
		return ret;
	}

	// Initialize TX
	ret = uart_pio_enhanced_tx_init(pio, config->tx_pin, tx_sm, div, config->data_bits);
	if (ret < 0) {
		LOG_ERR("Failed to initialize TX: %d", ret);
		return ret;
	}

	pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);

	/* Initialize interrupt buffer */
	data->pio = pio;
	data->rx_irq_enabled = false;
	data->callback = NULL;
	data->callback_data = NULL;
	data->data_bits = config->data_bits;

	if (uart_pio_instance_count < MAX_UART_PIO_INSTANCES) {
		uart_pio_instances[uart_pio_instance_count++] = dev;
	}

	/* Enable PIO interrupt when FIFO has data */

	uint irq_num = connect_rx_irq(pio, rx_sm);

	if (irq_num == -EINVAL) {
		return -EINVAL;
	}
	data->irq_num = irq_num;
	return 0;
}

static void uart_pio_enhanced_irq_rx_enable(const struct device *dev)
{
	struct uart_pio_enhanced_data *data = dev->data;

	data->rx_irq_enabled = true;

	// Enable the RX FIFO not empty interrupt source for this SM
	enum pio_interrupt_source source = pio_get_rx_fifo_not_empty_interrupt_source(data->rx_sm);

	pio_interrupt_clear(data->pio, data->irq_num);
	pio_set_irq0_source_enabled(data->pio, source, true);
}

static void uart_pio_enhanced_irq_rx_disable(const struct device *dev)
{
	struct uart_pio_enhanced_data *data = dev->data;

	data->rx_irq_enabled = false;

	// Disable the RX FIFO not empty interrupt source for this SM
	enum pio_interrupt_source source = pio_get_rx_fifo_not_empty_interrupt_source(data->rx_sm);

	pio_set_irq0_source_enabled(data->pio, source, false);
}

static int uart_pio_enhanced_irq_tx_complete(const struct device *dev)
{
	/* Always ready for more TX data in this implementation */
	return 1;
}

static void uart_pio_enhanced_irq_callback_set(const struct device *dev,
					       uart_irq_callback_user_data_t cb, void *user_data)
{
	struct uart_pio_enhanced_data *data = dev->data;

	data->callback = cb;
	data->callback_data = user_data;
}

static int uart_pio_enhanced_irq_update(const struct device *dev)
{
	return 1;
}

static const struct uart_driver_api uart_pio_enhanced_api = {
	.poll_in_u16 = uart_pio_enhanced_poll_in,
	.poll_out_u16 = uart_pio_enhanced_poll_out,
	.irq_rx_enable = uart_pio_enhanced_irq_rx_enable,
	.irq_rx_disable = uart_pio_enhanced_irq_rx_disable,
	.irq_tx_complete = uart_pio_enhanced_irq_tx_complete,
	.irq_callback_set = uart_pio_enhanced_irq_callback_set,
	.irq_update = uart_pio_enhanced_irq_update,
};

#define UART_PIO_ENHANCED_DEFINE(idx)                                                              \
	PINCTRL_DT_INST_DEFINE(idx);                                                               \
	static struct uart_pio_enhanced_data uart_pio_enhanced_data_##idx;                         \
	static const struct uart_pio_enhanced_config uart_pio_enhanced_config_##idx = {            \
		.piodev = DEVICE_DT_GET(DT_INST_PARENT(idx)),                                      \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(idx),                                       \
		.tx_pin = DT_INST_RPI_PICO_PIO_PIN_BY_NAME(idx, default, 0, tx_pins, 0),           \
		.rx_pin = DT_INST_RPI_PICO_PIO_PIN_BY_NAME(idx, default, 0, rx_pins, 0),           \
		.baud_rate = DT_INST_PROP(idx, current_speed),                                     \
		.data_bits = DT_INST_PROP(idx, data_bits),                                         \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(idx, uart_pio_enhanced_init, NULL, &uart_pio_enhanced_data_##idx,    \
			      &uart_pio_enhanced_config_##idx, POST_KERNEL,                        \
			      CONFIG_SERIAL_INIT_PRIORITY, &uart_pio_enhanced_api)

DT_INST_FOREACH_STATUS_OKAY(UART_PIO_ENHANCED_DEFINE);
// SYS_INIT(uart_pio_irq_init, PRE_KERNEL_1,
// CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
/* Remove IRQ_CONNECT from file scope */
