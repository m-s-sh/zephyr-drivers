// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Status LED driver
 *
 */

#define DT_DRV_COMPAT status_led

#include <drivers/status_led.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_led, LOG_LEVEL_DBG);

struct status_led_config {
	const struct gpio_dt_spec led;
	const char *label;
};

struct status_led_data {
	const struct device *dev;
	const struct status_led_config *config;
	struct k_work_delayable work;
	enum status_led_state current_state;
	bool blink_toggle;
	uint32_t heartbeat_counter;
};

static int status_led_set_state(const struct device *dev, enum status_led_state state)
{
	const struct status_led_config *config = dev->config;
	struct status_led_data *data = dev->data;

	LOG_DBG("Setting LED to state %d", state);

	/* Update state */
	data->current_state = state;

	/* Handle immediate state changes */
	switch (state) {
	case STATUS_LED_OFF:
		gpio_pin_set_dt(&config->led, 0);
		break;
	case STATUS_LED_ON:
		gpio_pin_set_dt(&config->led, 1);
		break;
	case STATUS_LED_BLINK_SLOW:
	case STATUS_LED_BLINK_FAST:
	case STATUS_LED_HEARTBEAT:
		/* Start blinking if not already active */
		k_work_reschedule(&data->work, K_MSEC(100));
		break;
	default:
		LOG_WRN("Unknown LED state: %d", state);
		return -EINVAL;
	}

	return 0;
}

static enum status_led_state status_led_get_state(const struct device *dev)
{
	struct status_led_data *data = dev->data;

	return data->current_state;
}

static void status_led_blink_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct status_led_data *data = CONTAINER_OF(dwork, struct status_led_data, work);
	const struct status_led_config *config = data->config;

	/* Increment heartbeat counter */
	data->heartbeat_counter++;

	/* Toggle blink state */
	data->blink_toggle = !data->blink_toggle;

	int delay = 0;

	switch (data->current_state) {
	case STATUS_LED_BLINK_FAST:
		gpio_pin_set_dt(&config->led, data->blink_toggle);
		delay = 250; /* 4Hz */
		break;
	case STATUS_LED_BLINK_SLOW:
		gpio_pin_set_dt(&config->led, data->blink_toggle);
		delay = 1000; /* 1Hz */
		break;
	case STATUS_LED_HEARTBEAT:
		/* Heartbeat pattern: double-beat every 12 cycles (3 seconds at 250ms) */
		{
			uint32_t cycle = data->heartbeat_counter % 12;
			bool heartbeat_on = (cycle == 0 || cycle == 1 || cycle == 3 || cycle == 4);

			gpio_pin_set_dt(&config->led, heartbeat_on);
			delay = 250; /* 4Hz for heartbeat timing */
		}
		break;
	default:
		break;
	}
	if (delay == 0) {
		/* No blinking needed, stop work */
		return;
	}
	k_work_reschedule(&data->work, K_MSEC(delay));
}

static int status_led_init(const struct device *dev)
{
	const struct status_led_config *config = dev->config;
	struct status_led_data *data = dev->data;
	int err = 0;

	LOG_INF("Initializing status LED device: %s", dev->name);

	/* Store device reference for blink handler */
	data->dev = dev;
	data->config = config;

	/* Initialize all LED pins */

	const struct gpio_dt_spec *led = &config->led;

	if (!device_is_ready(led->port)) {
		LOG_ERR("%s: GPIO device not ready for LED", dev->name);
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Cannot configure GPIO for LED (err %d)", err);
		return err;
	}
	LOG_DBG("Configured LED on GPIO %s pin %d", led->port->name, led->pin);

	/* Initialize work item */
	k_work_init_delayable(&data->work, status_led_blink_work_handler);

	/* Initialize all LEDs to OFF state */
	data->current_state = STATUS_LED_OFF;

	LOG_INF("Status LED device %s initialized successfully", dev->name);
	return err;
}

static const struct status_led_driver_api status_led_api = {
	.get_state = status_led_get_state,
	.set_state = status_led_set_state,
};

#define STATUS_LED_DEFINE(i)                                                                       \
	static const struct status_led_config status_led_config_##i = {                            \
		.led = GPIO_DT_SPEC_INST_GET(i, led_gpios),                                        \
		.label = DT_INST_PROP_OR(i, label, "Status LED"),                                  \
	};                                                                                         \
	static struct status_led_data status_led_data_##i;                                         \
	DEVICE_DT_INST_DEFINE(i, status_led_init, NULL, &status_led_data_##i,                      \
			      &status_led_config_##i, POST_KERNEL, CONFIG_GPIO_INIT_PRIORITY,      \
			      &status_led_api);

DT_INST_FOREACH_STATUS_OKAY(STATUS_LED_DEFINE)
