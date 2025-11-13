// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem driver
 *
 */

#define DT_DRV_COMPAT simcom_sim800l

#include <zephyr/logging/log.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(modem_simcom_sim800l, CONFIG_MODEM_SIM800L_LOG_LEVEL);

#include <string.h>
#include <stdlib.h>

#include "sim800l.h"

static struct k_thread modem_rx_thread;
static K_KERNEL_STACK_DEFINE(modem_rx_stack, 2048);
NET_BUF_POOL_DEFINE(mdm_recv_pool, MDM_RECV_MAX_BUF, MDM_RECV_BUF_SIZE, 0, NULL);

/* Device instance definition */
struct sim800l_data mdata = {
	.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(0, mdm_reset_gpios, {}),
	.powered = false,
};

static const struct sim800l_config mconfig = {
	.uart = DEVICE_DT_GET(DT_INST_BUS(0)),
};

MODEM_CMD_DEFINE(on_cmd_ok)
{
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_error)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_exterror)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/**
 * Handles the ftpget urc.
 *
 * +FTPGET: <mode>,<error>
 *
 * Mode can be 1 for opening a session and
 * reporting that data is available or 2 for
 * reading data. This urc handler will only handle
 * mode 1 because 2 will not occur as urc.
 *
 * Error can be either:
 *  - 1 for data available/opened session.
 *  - 0 If transfer is finished.
 *  - >0 for some error.
 */
MODEM_CMD_DEFINE(on_urc_ftpget)
{
	int error = atoi(argv[0]);

	LOG_DBG("+FTPGET: 1,%d", error);
	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_urc_rdy)
{
	LOG_DBG("RDY received");
	k_sem_give(&mdata.boot_sem);
	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_urc_pwr_down)
{
	LOG_DBG("POWER DOWN received");
	return 0;
}

MODEM_CMD_DEFINE(on_psuttz)
{
	int year, month, day, hour, minute, second;
	char timezone[16];
	int dst;

	return 0;
	/*
	 * Parse PSUTTZ message format:
	 * *PSUTTZ: <year>,<month>,<day>,<hour>,<minute>,<second>,"<timezone>",<dst>
	 * Example: *PSUTTZ: 25,1,15,10,30,45,"+08",0
	 *
	 * Note: The timezone is quoted and needs to be parsed from argv[6]
	 */
	if (argc < 7) {
		LOG_ERR("Invalid PSUTTZ message format, argc=%d (expected at least 7)", argc);
		return -EINVAL;
	}

	year = atoi(argv[0]);   /* Year (2 digits, e.g., 25 for 2025) */
	month = atoi(argv[1]);  /* Month (1-12) */
	day = atoi(argv[2]);    /* Day (1-31) */
	hour = atoi(argv[3]);   /* Hour (0-23) */
	minute = atoi(argv[4]); /* Minute (0-59) */
	second = atoi(argv[5]); /* Second (0-59) */

	/* Parse timezone - remove quotes if present */
	size_t tz_len = strlen(argv[6]);
	if (tz_len > 0 && argv[6][0] == '"') {
		/* Skip leading quote */
		size_t copy_len = (tz_len > 2) ? (tz_len - 2) : 0;
		if (copy_len >= sizeof(timezone)) {
			copy_len = sizeof(timezone) - 1;
		}
		memcpy(timezone, argv[6] + 1, copy_len);
		timezone[copy_len] = '\0';
		/* Remove trailing quote if it exists */
		if (copy_len > 0 && timezone[copy_len - 1] == '"') {
			timezone[copy_len - 1] = '\0';
		}
	} else {
		strncpy(timezone, argv[6], sizeof(timezone) - 1);
		timezone[sizeof(timezone) - 1] = '\0';
	}

	/* DST might be in argv[7] if timezone parsing worked correctly */
	dst = (argc >= 8) ? atoi(argv[7]) : 0;

	LOG_DBG("Network time: 20%02d-%02d-%02d %02d:%02d:%02d TZ=%s DST=%d", year, month, day,
		hour, minute, second, timezone, dst);

	/* TODO: Update system time if needed */
	/* This could be used to synchronize the system clock with network time */

	return 0;
}

/*
 * Read manufacturer identification.
 */
MODEM_CMD_DEFINE(on_cmd_cgmi)
{
	size_t out_len = net_buf_linearize(mdata.manufacturer, sizeof(mdata.manufacturer) - 1,
					   data->rx_buf, 0, len);
	mdata.manufacturer[out_len] = '\0';
	LOG_DBG("Manufacturer: %s", mdata.manufacturer);
	return 0;
}

/*
 * Read model identification.
 */
MODEM_CMD_DEFINE(on_cmd_cgmm)
{
	size_t out_len =
		net_buf_linearize(mdata.model, sizeof(mdata.model) - 1, data->rx_buf, 0, len);

	mdata.model[out_len] = '\0';
	LOG_DBG("Model: %s", mdata.model);
	return 0;
}

/*
 * Read software release.
 *
 * Response will be in format RESPONSE: <revision>.
 */
MODEM_CMD_DEFINE(on_cmd_cgmr)
{
	size_t out_len;
	char *p;

	out_len =
		net_buf_linearize(mdata.revision, sizeof(mdata.revision) - 1, data->rx_buf, 0, len);
	mdata.revision[out_len] = '\0';

	/* The module prepends a Revision: */
	p = strchr(mdata.revision, ':');
	if (p) {
		out_len = strlen(p + 1);
		memmove(mdata.revision, p + 1, out_len + 1);
	}

	LOG_DBG("Revision: %s", mdata.revision);
	return 0;
}

/*
 * Read serial number identification.
 */
MODEM_CMD_DEFINE(on_cmd_cgsn)
{
	size_t out_len =
		net_buf_linearize(mdata.imei, sizeof(mdata.imei) - 1, data->rx_buf, 0, len);

	mdata.imei[out_len] = '\0';
	LOG_DBG("IMEI: %s", mdata.imei);
	return 0;
}

MODEM_CMD_DEFINE(on_urc_ciev)
{
	LOG_DBG("+CIEV received");
	return 0;
}

MODEM_CMD_DEFINE(on_urc_creg)
{
	int reg_state = atoi(argv[0]);

	LOG_DBG("+CREG: %d", reg_state);

	if (reg_state == 1 || reg_state == 5) {
		/* Registered on home network or roaming */
		mdata.state = SIM800L_STATE_READY;
		k_sem_give(&mdata.boot_sem);
	} else {
		/* Not registered */
		mdata.state = SIM800L_STATE_INIT;
	}

	return 0;
}

MODEM_CMD_DEFINE(on_urc_cpin)
{
	if (strcmp(argv[0], "READY") == 0) {
		mdata.status_flags |= SIM800L_STATUS_FLAG_CPIN_READY;
	} else {
		mdata.status_flags &= ~SIM800L_STATUS_FLAG_CPIN_READY;
	}
	k_sem_give(&mdata.boot_sem);

	LOG_DBG("CPIN: %s", argv[0]);
	return 0;
}

MODEM_CMD_DEFINE(on_urc_pdp_deact)
{
	mdata.status_flags &= ~SIM800L_STATUS_FLAG_PDP_ACTIVE;
	LOG_DBG("PDP context deactivated by network");
	return 0;
}

/* URC: +RECEIVE,<n>,<data length>:\r\n<data> */
MODEM_CMD_DEFINE(on_urc_receive)
{
	struct modem_socket *sock;
	int ret;
	int sock_id;
	int data_len;
	uint8_t chunk[128];

	sock_id = atoi(argv[0]);
	data_len = atoi(argv[1]);

	LOG_DBG("+RECEIVE: socket %d, length %d", sock_id, data_len);

	/* Find the socket */
	sock = modem_socket_from_id(&mdata.socket_config, sock_id);
	if (!sock) {
		LOG_WRN("Received data for unknown socket %d", sock_id);
		return 0;
	}

	struct sim800l_socket_data *sock_data = (struct sim800l_socket_data *)sock->data;

	k_mutex_lock(&sock_data->lock, K_FOREVER);

	if (!sock_data->rx_buf) {
		sock_data->rx_buf = net_buf_alloc(&mdm_recv_pool, K_NO_WAIT);
		if (!sock_data->rx_buf) {
			LOG_ERR("Socket %d RX buffer alloc failed", sock_id);
			k_mutex_unlock(&sock_data->lock);
			return 0;
		}
	}

	LOG_DBG("rx_buf has %d bytes before header skip", data->rx_buf ? data->rx_buf->len : 0);

	/* Read remaining data from UART with retry logic */
	int retry_count = 0;
	const int max_retries = 5;
	int skip = 1;

	while (data_len > 0) {
		size_t to_read = MIN(data_len + skip, sizeof(chunk));
		size_t bytes_read;

		ret = mdata.ctx.iface.read(&mdata.ctx.iface, chunk, to_read, &bytes_read);
		if (ret < 0) {
			LOG_ERR("Socket %d read error: %d", sock_id, ret);
			break;
		}

		if (bytes_read == 0) {
			/* Data may still be arriving at 9600 baud */
			if (retry_count < max_retries) {
				retry_count++;
				k_sleep(K_MSEC(10));
				continue;
			} else {
				LOG_WRN("Socket %d no more data after %d retries", sock_id,
					max_retries);
				break;
			}
		}

		/* Got data - reset retry counter */
		retry_count = 0;

		/* Append to socket RX buffer */
		if (net_buf_tailroom(sock_data->rx_buf) < bytes_read) {
			LOG_ERR("Socket %d RX buffer overflow", sock_id);
			break;
		}
		LOG_HEXDUMP_DBG(chunk, bytes_read, "Received chunk:");
		net_buf_add_mem(sock_data->rx_buf, chunk + skip, bytes_read - skip);
		sock_data->buffered += bytes_read - skip;
		data_len -= bytes_read - skip;
		skip = 0;
	}

	k_mutex_unlock(&sock_data->lock);

	LOG_DBG("Socket %d buffered %zu bytes", sock_id, sock_data->buffered);
	if (sock_data->buffered > 0) {
		/* Signal data is ready */
		modem_socket_packet_size_update(&mdata.socket_config, sock, sock_data->buffered);
		modem_socket_data_ready(&mdata.socket_config, sock);
	}

	return 0;
}

/*
 * Handler for RSSI query.
 *
 * +CSQ: <rssi>,<ber>
 *  rssi: 0,-115dBm; 1,-111dBm; 2...30,-110...-54dBm; 31,-52dBm or greater.
 *        99, ukn
 *  ber: Not used.
 */
MODEM_CMD_DEFINE(on_cmd_csq)
{
	int rssi = atoi(argv[0]);

	if (rssi == 0) {
		mdata.rssi = -115;
	} else if (rssi == 1) {
		mdata.rssi = -111;
	} else if (rssi > 1 && rssi < 31) {
		mdata.rssi = -114 + 2 * rssi;
	} else if (rssi == 31) {
		mdata.rssi = -52;
	} else {
		mdata.rssi = -1000;
	}

	LOG_DBG("RSSI: %d", mdata.rssi);
	return 0;
}

/*
 * Possible responses by the sim800l.
 */
static const struct modem_cmd response_cmds[] = {
	MODEM_CMD("OK", on_cmd_ok, 0U, ""),
	MODEM_CMD("ERROR", on_cmd_error, 0U, ""),
	MODEM_CMD("+CME ERROR: ", on_cmd_exterror, 1U, ""),

};

static const struct modem_cmd unsolicited_cmds[] = {
	MODEM_CMD("+PDP: DEACT", on_urc_pdp_deact, 0U, ""),
	MODEM_CMD("+FTPGET: 1,", on_urc_ftpget, 1U, ""),
	MODEM_CMD("RDY", on_urc_rdy, 0U, ""),
	MODEM_CMD("NORMAL POWER DOWN", on_urc_pwr_down, 0U, ""),
	// MODEM_CMD("*PSUTTZ: ", on_psuttz, 7U, ","),
	MODEM_CMD("+CIEV: ", on_urc_ciev, 0U, ","),
	MODEM_CMD("+CREG: ", on_urc_creg, 1U, ","),
	MODEM_CMD("+CPIN: ", on_urc_cpin, 1U, ","),
	MODEM_CMD("+RECEIVE,", on_urc_receive, 2U, ","),
};

/*
 * Commands to be sent at setup.
 */
static const struct setup_cmd setup_cmds[] = {
	SETUP_CMD("AT+CGMI", "", on_cmd_cgmi, 0U, ""),
	SETUP_CMD("AT+CGMM", "", on_cmd_cgmm, 0U, ""),
	SETUP_CMD("AT+CGMR", "", on_cmd_cgmr, 0U, ""),
	SETUP_CMD("AT+CGSN", "", on_cmd_cgsn, 0U, ""),

};

static int modem_reset(const struct device *dev)
{
	struct sim800l_data *data = dev->data;
	int ret;

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

	if (data->powered) {
		return 0;
	}

	/* SIM800L doesn't have a power pin - it's always powered when VCC is applied */
	/* We can optionally perform a reset to ensure clean state */
	if (data->reset_gpio.port) {
		modem_reset(dev);
	} else {
		/* If no reset pin, just wait for module to be ready */
		k_sleep(K_MSEC(3000));
	}

	data->powered = true;
	LOG_DBG("Modem powered on");

	return 0;
}

static int modem_power_off(const struct device *dev)
{
	struct sim800l_data *data = dev->data;

	if (!data->powered) {
		return 0;
	}

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

void modem_query_rssi(void)
{
	struct modem_cmd cmd[] = {MODEM_CMD("+CSQ: ", on_cmd_csq, 2U, ",")};
	static char *send_cmd = "AT+CSQ";
	int ret;

	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, cmd, ARRAY_SIZE(cmd),
			     send_cmd, &mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("AT+CSQ ret:%d", ret);
	}
}

#ifdef CONFIG_PM_DEVICE
static int modem_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret = 0;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		ret = modem_power_off(dev);
		if (ret < 0) {
			LOG_ERR("Failed to suspend SIM800L: %d", ret);
		}

		break;

	case PM_DEVICE_ACTION_RESUME:
		ret = modem_power_on(dev);
		if (ret < 0) {
			LOG_ERR("Failed to resume SIM800L: %d", ret);
		}

		break;

	default:
		ret = -ENOTSUP;
		break;
	}

	return ret;
}
#endif /* CONFIG_PM_DEVICE */

/*
 * Process all messages received from the modem.
 */
static void modem_rx(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		/* Wait for incoming UART data */
		modem_iface_uart_rx_wait(&mdata.ctx.iface, K_FOREVER);

		/* Process AT command responses and unsolicited messages */
		modem_cmd_handler_process(&mdata.ctx.cmd_handler, &mdata.ctx.iface);

		/* give up time if we have a solid stream of data */
		k_yield();
	}
}

/**
 * Performs the autobaud sequence until modem answers or limit is reached.
 *
 * @return On successful boot 0 is returned. Otherwise <0 is returned.
 */
static int modem_autobaud(void)
{
	int boot_tries = 0;
	int counter = 0;
	int ret = 0;

	while (boot_tries++ <= MDM_BOOT_TRIES) {
		modem_reset(mdata.ctx.iface.dev);

		/*
		 * The sim7080 has a autobaud function.
		 * On startup multiple AT's are sent until
		 * a OK is received.
		 */
		counter = 0;
		while (counter < MDM_MAX_AUTOBAUD) {
			ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, NULL, 0U,
					     "AT", &mdata.sem_response, MDM_CMD_TIMEOUT);

			/* OK was received. */
			if (ret == 0) {
				/* Disable echo */
				return modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler,
						      NULL, 0U, "ATE0", &mdata.sem_response,
						      MDM_CMD_TIMEOUT);
			}
			counter++;
		}
	}
	return ret;
}

static int modem_boot(void)
{
	int ret = 0;

	LOG_DBG("Booting modem");

	k_work_cancel_delayable(&mdata.rssi_query_work);

	ret = modem_autobaud();
	if (ret != 0) {
		LOG_ERR("Modem autobaud failed");
		return ret;
	}

	k_sem_reset(&mdata.boot_sem);
	ret = k_sem_take(&mdata.boot_sem, K_SECONDS(5));
	if (ret != 0) {
		LOG_ERR("Timeout while waiting for RDY");
		return ret;
	}

	/* Wait for sim card status */
	ret = k_sem_take(&mdata.boot_sem, K_SECONDS(10));
	if (ret != 0) {
		LOG_ERR("Timeout while waiting for sim status");
		return ret;
	}

	if ((mdata.status_flags & SIM800L_STATUS_FLAG_CPIN_READY) == 0) {
		LOG_ERR("Sim card not ready!");
		return -EIO;
	}

	mdata.state = SIM800L_STATE_READY;

	/* Send setup commands */
	ret = modem_cmd_handler_setup_cmds(&mdata.ctx.iface, &mdata.ctx.cmd_handler, setup_cmds,
					   ARRAY_SIZE(setup_cmds), &mdata.sem_response,
					   MDM_REGISTRATION_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to send init commands!");
		return ret;
	}

	k_sleep(K_SECONDS(3));

	ret = modem_pdp_activate();
	if (ret < 0) {
		LOG_ERR("Failed to activate PDP context: %d", ret);
		return ret;
	}

	LOG_INF("Modem boot complete");
	return ret;
}

static int modem_init(const struct device *dev)
{

	int ret;

	LOG_DBG("Initializing modem");

	mdata.status_flags = 0;

	k_sem_init(&mdata.sem_tx_ready, 0, 1);
	k_sem_init(&mdata.sem_response, 0, 1);
	k_sem_init(&mdata.sem_dns, 0, 1);
	k_sem_init(&mdata.sem_sock_conn, 0, 1);
	k_sem_init(&mdata.boot_sem, 0, 1);

	/* Initialize reset GPIO */
	if (mdata.reset_gpio.port) {
		if (!gpio_is_ready_dt(&mdata.reset_gpio)) {
			LOG_ERR("Reset GPIO device not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&mdata.reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure reset GPIO: %d", ret);
			return ret;
		}
	}

	/* Socket config. */
	ret = modem_socket_init(&mdata.socket_config, &mdata.sockets[0], ARRAY_SIZE(mdata.sockets),
				MDM_BASE_SOCKET_NUM, true, &offload_socket_fd_op_vtable);
	if (ret < 0) {
		return ret;
	}

	/* Command handler. */
	const struct modem_cmd_handler_config cmd_handler_config = {
		.match_buf = &mdata.cmd_match_buf[0],
		.match_buf_len = sizeof(mdata.cmd_match_buf),
		.buf_pool = &mdm_recv_pool,
		.alloc_timeout = BUF_ALLOC_TIMEOUT,
		.eol = "\r\n",
		.user_data = NULL,
		.response_cmds = response_cmds,
		.response_cmds_len = ARRAY_SIZE(response_cmds),
		.unsol_cmds = unsolicited_cmds,
		.unsol_cmds_len = ARRAY_SIZE(unsolicited_cmds),
	};

	ret = modem_cmd_handler_init(&mdata.ctx.cmd_handler, &mdata.cmd_handler_data,
				     &cmd_handler_config);
	if (ret < 0) {
		return ret;
	}

	/* Uart handler. */
	const struct modem_iface_uart_config uart_config = {
		.rx_rb_buf = &mdata.iface_rb_buf[0],
		.rx_rb_buf_len = sizeof(mdata.iface_rb_buf),
		.dev = mconfig.uart,
		.hw_flow_control = false,
	};

	ret = modem_iface_uart_init(&mdata.ctx.iface, &mdata.iface_data, &uart_config);
	if (ret < 0) {
		return ret;
	}

	ret = modem_power_on(dev);
	if (ret < 0) {
		LOG_ERR("Failed to power on modem: %d", ret);
		return ret;
	}
#ifdef CONFIG_PM_DEVICE
	pm_device_init_suspended(dev);
#endif /* CONFIG_PM_DEVICE */

	/* Modem data storage. */
	mdata.ctx.data_manufacturer = mdata.manufacturer;
	mdata.ctx.data_model = mdata.model;
	mdata.ctx.data_revision = mdata.revision;
	mdata.ctx.data_imei = mdata.imei;
	mdata.ctx.driver_data = &mdata;
	ret = modem_context_register(&mdata.ctx);
	if (ret < 0) {
		LOG_ERR("Error registering modem context: %d", ret);
		return ret;
	}

	k_tid_t tid = k_thread_create(&modem_rx_thread, modem_rx_stack,
				      K_KERNEL_STACK_SIZEOF(modem_rx_stack), modem_rx, NULL, NULL,
				      NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	k_thread_name_set(tid, "modem_rx");
	k_sleep(K_MSEC(100));

	return modem_boot();
}

#ifdef CONFIG_PM_DEVICE
PM_DEVICE_DT_INST_DEFINE(0, modem_pm_action);
#endif

#define MODEM_SIMCOM_SIM800L_INIT_PRIORITY 90

static struct offloaded_if_api api_funcs = {
	.iface_api.init = modem_net_iface_init,
};

// /* Register device with the networking stack. */
NET_DEVICE_DT_INST_OFFLOAD_DEFINE(0, modem_init, PM_DEVICE_DT_INST_GET(0), &mdata, &mconfig,
				  CONFIG_GPIO_INIT_PRIORITY, &api_funcs, MDM_MAX_DATA_LENGTH);

/* Register NET sockets. */
NET_SOCKET_OFFLOAD_REGISTER(simcom_sim800l, CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY, AF_UNSPEC,
			    modem_offload_is_supported, modem_offload_socket);
