/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2025 Blue Vending
 * Status LED driver
 *
 */

#ifndef BV_DRIVERS_STATUS_LED_H_
#define BV_DRIVERS_STATUS_LED_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <zephyr/device.h>

/**
 * @brief Status LED States
 */
enum status_led_state {
	STATUS_LED_OFF = 0,
	STATUS_LED_ON,
	STATUS_LED_BLINK_SLOW,
	STATUS_LED_BLINK_FAST,
	STATUS_LED_HEARTBEAT
};

/**
 * @brief Status LED Pattern definition
 */
struct status_led_pattern {
	enum status_led_state state;
};

/**
 * @brief Status LED Driver API
 */
struct status_led_driver_api {
	int (*set_state)(const struct device *dev, enum status_led_state state);
	enum status_led_state (*get_state)(const struct device *dev);
};

/**
 * @brief Set status LED state
 *
 * @param dev Status LED device
 * @param color LED color to control
 * @param state LED state to set
 * @return 0 on success, negative error code on failure
 */
static inline int z_status_led_set_state(const struct device *dev, enum status_led_state state)
{
	const struct status_led_driver_api *api = dev->api;

	if (!api || !api->set_state) {
		return -ENOTSUP;
	}

	return api->set_state(dev, state);
}

/**
 * @brief Get status LED state
 *
 * @param dev Status LED device
 * @param color LED color to query
 * @return Current LED state
 */
static inline enum status_led_state z_status_led_get_state(const struct device *dev)
{
	const struct status_led_driver_api *api = dev->api;

	if (!api || !api->get_state) {
		return STATUS_LED_OFF;
	}

	return api->get_state(dev);
}

#ifdef __cplusplus
}
#endif

#endif /* BV_DRIVERS_STATUS_LED_H_ */
