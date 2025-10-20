// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem driver
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/cellular.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/modem/chat.h>
//#include <zephyr/modem/cmux.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/pipelink.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_sim800l, LOG_LEVEL_DBG);

#include <string.h>
#include <stdlib.h>

#define DT_DRV_COMPAT simcom_sim800l

/* Modem cellular data and configuration structures */
struct modem_cellular_data {
	char *chat_delimiter;
	char *chat_filter;
	struct modem_ppp *ppp;
};

struct modem_cellular_user_pipe {
	struct modem_pipe *pipe;
	uint8_t dlci;
};

/* Device instance structures */
//#define MODEM_CELLULAR_INST_NAME(name, inst) _CONCAT(_CONCAT(modem_cellular_, name), inst)

// #define MODEM_DT_INST_PPP_DEFINE(inst, name, user_data, buf_len, mtu, mru)                         \
// 	static uint8_t _CONCAT(ppp_recv_buf, inst)[buf_len];                                       \
// 	static struct modem_ppp name = {                                                           \
// 		.receive_buf = _CONCAT(ppp_recv_buf, inst),                                        \
// 		.receive_buf_size = sizeof(_CONCAT(ppp_recv_buf, inst)),                           \
// 		.mtu = mtu,                                                                        \
// 		.mru = mru,                                                                        \
	};

/* SIM800L specific state enum */
enum sim800l_state {
	SIM800L_STATE_IDLE = 0,
	SIM800L_STATE_RESET,
	SIM800L_STATE_INIT,
	SIM800L_STATE_READY,
	SIM800L_STATE_ERROR,
};

/* SIM800L Device Definition */
// MODEM_DT_INST_PPP_DEFINE(0, MODEM_CELLULAR_INST_NAME(ppp, 0), NULL, 98, 1500, 64);

// static struct modem_cellular_data MODEM_CELLULAR_INST_NAME(data, 0) = {
// 	.chat_delimiter = "\r",
// 	.chat_filter = "\n",
// 	.ppp = &MODEM_CELLULAR_INST_NAME(ppp, 0),
// };

// /* User pipes for CMUX channels */
// static struct modem_cellular_user_pipe MODEM_CELLULAR_INST_NAME(user_pipes, 0)[] = {
// 	{ .pipe = NULL, .dlci = 3 }, /* User pipe 0 on DLCI 3 */
// };

/* Power management and GPIO control */
struct sim800l_data {
	const struct gpio_dt_spec reset_gpio;
	bool powered;
	struct k_work_delayable timeout_work;
	enum sim800l_state state;
	// struct modem_pipe *uart_pipe;
	// struct modem_cmux cmux;
	// struct modem_chat chat;
};

struct sim800l_config {
	const struct device *uart;
};


static int modem_reset(const struct device *dev)
{
	struct sim800l_data *data = dev->data;
	int ret;

	LOG_DBG("Resetting modem");

	if (data->reset_gpio.port) {
		/* Assert reset */
		ret = gpio_pin_set_dt(&data->reset_gpio, 0);
		if (ret < 0) {
			LOG_ERR("Failed to assert reset GPIO: %d", ret);
			return ret;
		}

		/* Hold reset for 100ms */
		k_sleep(K_MSEC(100));

		/* Release reset */
		ret = gpio_pin_set_dt(&data->reset_gpio, 1);
		if (ret < 0) {
			LOG_ERR("Failed to release reset GPIO: %d", ret);
			return ret;
		}

		/* Wait for modem to boot after reset */
		k_sleep(K_MSEC(3000));
	}

	LOG_DBG("Modem reset complete");
	return 0;
}

static int modem_power_on(const struct device *dev)
{
	struct sim800l_data *data = dev->data;

	if (data->powered)
		return 0;

	LOG_DBG("Enabling modem");

	/* SIM800L doesn't have a power pin - it's always powered when VCC is applied */
	/* We can optionally perform a reset to ensure clean state */
	if (data->reset_gpio.port) {
		modem_reset(dev);
	} else {
		/* If no reset pin, just wait for module to be ready */
		k_sleep(K_MSEC(3000));
	}

	data->powered = true;
	LOG_DBG("Modem enabled");

	return 0;
}

static int modem_power_off(const struct device *dev)
{
	struct sim800l_data *data = dev->data;

	if (!data->powered)
		return 0;


	LOG_DBG("Disabling modem");

	/* SIM800L doesn't have a power pin - we can only put it in reset state */
	/* or send AT+CPOWD=1 command to software power down */
	if (data->reset_gpio.port) {
		/* Hold in reset state */
		gpio_pin_set_dt(&data->reset_gpio, 0);
		LOG_DBG("SIM800L held in reset state");
	}

	data->powered = false;
	LOG_DBG("Modem disabled");

	return 0;
}

#ifdef CONFIG_PM_DEVICE
static int modem_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret = 0;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		ret = modem_power_off(dev);
		if (ret < 0)
			LOG_ERR("Failed to suspend SIM800L: %d", ret);

		break;

	case PM_DEVICE_ACTION_RESUME:
		ret = modem_power_on(dev);
		if (ret < 0)
			LOG_ERR("Failed to resume SIM800L: %d", ret);

		break;

	default:
		ret = -ENOTSUP;
		break;
	}

	return ret;
}
#endif /* CONFIG_PM_DEVICE */

/* Cellular API implementation */
static int sim800l_get_signal(const struct device *dev,
			     const enum cellular_signal_type type,
			     int16_t *value)
{
	/* Implementation would query signal strength via AT+CSQ */
	*value = -70; /* Dummy value */
	return 0;
}

static int sim800l_get_modem_info(const struct device *dev,
				 const enum cellular_modem_info_type type,
				 char *info, size_t size)
{
	/* Implementation would query modem info via AT commands */
	switch (type) {
	case CELLULAR_MODEM_INFO_IMEI:
		strncpy(info, "123456789012345", size);
		break;
	case CELLULAR_MODEM_INFO_MODEL_ID:
		strncpy(info, "SIM800L", size);
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static const struct cellular_driver_api sim800l_driver_api = {
	.get_signal = sim800l_get_signal,
	.get_modem_info = sim800l_get_modem_info,
};

static int modem_init(const struct device *dev)
{
	struct sim800l_data *data = dev->data;
	const struct sim800l_config *config = dev->config;
	int ret;

	LOG_INF("Initializing SIM800L modem");

	/* Initialize reset GPIO */
	if (data->reset_gpio.port) {
		if (!gpio_is_ready_dt(&data->reset_gpio)) {
			LOG_ERR("Reset GPIO device not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&data->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure reset GPIO: %d", ret);
			return ret;
		}
	}

	/* Check UART device */
	if (!device_is_ready(config->uart)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	/* Auto-start if configured */
	if (DT_INST_PROP_OR(0, autostarts, false)) {
		ret = modem_power_on(dev);
		if (ret < 0) {
			LOG_ERR("Auto-start failed: %d", ret);
			return ret;
		}
	}
#ifdef CONFIG_PM_DEVICE
	pm_device_init_suspended(dev);
#endif /* CONFIG_PM_DEVICE */

	LOG_INF("Modem initialized successfully");
	return 0;
}

/* Device instance definition */
static struct sim800l_data sim800l_data_0 = {
	.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(0, mdm_reset_gpios, {}),
	.powered = false,
};

static const struct sim800l_config sim800l_config_0 = {
	 .uart = DEVICE_DT_GET(DT_INST_BUS(0)),
};

#ifdef CONFIG_PM_DEVICE
PM_DEVICE_DT_INST_DEFINE(0, modem_pm_action);
#endif

DEVICE_DT_INST_DEFINE(0, modem_init,
#ifdef CONFIG_PM_DEVICE
		      PM_DEVICE_DT_INST_GET(0),
#else
		      NULL,
#endif
		      &sim800l_data_0, &sim800l_config_0,
		      POST_KERNEL, CONFIG_MODEM_SIM800L_INIT_PRIORITY,
		      &sim800l_driver_api);
