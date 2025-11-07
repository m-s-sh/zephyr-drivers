// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem driver - AT Command handlers
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <stdlib.h>

#include "sim800l.h"
#include "sim800l_at_cmd.h"

LOG_MODULE_DECLARE(simcom_sim800l, CONFIG_MODEM_SIM800L_LOG_LEVEL);

/* External references to data in sim800l.c */
extern struct sim800l_data sim800l_data_0;
extern struct zsock_addrinfo dns_result;
extern struct sockaddr dns_result_addr;

/* Response command handlers */

MODEM_CMD_DEFINE(on_cmd_ok)
{
	struct sim800l_data *d = data->user_data;

	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&d->sem_response);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_error)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&sim800l_data_0.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_exterror)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&sim800l_data_0.sem_response);
	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_cmd_tx_ready)
{
	k_sem_give(&sim800l_data_0.sem_tx_ready);
	return len;
}

/* Unsolicited command handlers */

MODEM_CMD_DEFINE(on_urc_ftpget)
{
	int error = atoi(argv[0]);

	LOG_DBG("+FTPGET: 1,%d", error);
	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_urc_rdy)
{
	LOG_DBG("RDY received");
	struct sim800l_data *d = data->user_data;

	k_sem_give(&d->boot_sem);
	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_urc_pwr_down)
{
	LOG_DBG("POWER DOWN received");
	return 0;
}

MODEM_CMD_DEFINE(on_urc_cpin)
{
	struct sim800l_data *d = data->user_data;

	if (strcmp(argv[0], "READY") == 0) {
		d->status_flags |= SIM800L_STATUS_FLAG_CPIN_READY;
	} else {
		d->status_flags &= ~SIM800L_STATUS_FLAG_CPIN_READY;
	}
	k_sem_give(&d->boot_sem);

	LOG_INF("CPIN: %s", argv[0]);
	return 0;
}

MODEM_CMD_DEFINE(on_psuttz)
{
	int year, month, day, hour, minute, second;
	char timezone[16];
	int dst;

	if (argc < 7) {
		LOG_ERR("Invalid PSUTTZ message format, argc=%d (expected at least 7)", argc);
		return -EINVAL;
	}

	year = atoi(argv[0]);
	month = atoi(argv[1]);
	day = atoi(argv[2]);
	hour = atoi(argv[3]);
	minute = atoi(argv[4]);
	second = atoi(argv[5]);

	/* Parse timezone - remove quotes if present */
	size_t tz_len = strlen(argv[6]);
	if (tz_len > 0 && argv[6][0] == '"') {
		size_t copy_len = (tz_len > 2) ? (tz_len - 2) : 0;
		if (copy_len >= sizeof(timezone)) {
			copy_len = sizeof(timezone) - 1;
		}
		memcpy(timezone, argv[6] + 1, copy_len);
		timezone[copy_len] = '\0';
		if (copy_len > 0 && timezone[copy_len - 1] == '"') {
			timezone[copy_len - 1] = '\0';
		}
	} else {
		strncpy(timezone, argv[6], sizeof(timezone) - 1);
		timezone[sizeof(timezone) - 1] = '\0';
	}

	dst = (argc >= 8) ? atoi(argv[7]) : 0;

	LOG_INF("Network time: 20%02d-%02d-%02d %02d:%02d:%02d TZ=%s DST=%d", year, month, day,
		hour, minute, second, timezone, dst);

	return 0;
}

MODEM_CMD_DEFINE(on_urc_ciev)
{
	LOG_INF("+CIEV received");
	return 0;
}

MODEM_CMD_DEFINE(on_urc_creg)
{
	struct sim800l_data *d = data->user_data;
	int reg_state = atoi(argv[0]);

	LOG_INF("+CREG: %d", reg_state);

	if (reg_state == 1 || reg_state == 5) {
		d->state = SIM800L_STATE_READY;
		k_sem_give(&d->boot_sem);
	} else {
		d->state = SIM800L_STATE_INIT;
	}

	return 0;
}

/* Setup command handlers */

MODEM_CMD_DEFINE(on_cmd_cdnsgip)
{
	int state;
	char ips[256];
	size_t out_len;
	int ret = -1;

	state = atoi(argv[0]);
	if (state == 0) {
		LOG_ERR("DNS lookup failed with error %s", argv[1]);
		goto exit;
	}

	/* Offset to skip the leading " */
	out_len = net_buf_linearize(ips, sizeof(ips) - 1, data->rx_buf, 1, len);
	ips[out_len] = '\0';

	/* find trailing " */
	char *ipv4 = strstr(ips, "\"");

	if (!ipv4) {
		LOG_ERR("Malformed DNS response!!");
		goto exit;
	}

	*ipv4 = '\0';
	net_addr_pton(dns_result.ai_family, ips,
		      &((struct sockaddr_in *)&dns_result_addr)->sin_addr);
	ret = 0;

exit:
	k_sem_give(&sim800l_data_0.sem_dns);
	return ret;
}

MODEM_CMD_DEFINE(on_cmd_cgmi)
{
	struct sim800l_data *d = data->user_data;

	size_t out_len = net_buf_linearize(d->manufacturer, sizeof(d->manufacturer) - 1,
					   data->rx_buf, 0, len);
	d->manufacturer[out_len] = '\0';
	LOG_INF("Manufacturer: %s", d->manufacturer);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_cgmm)
{
	struct sim800l_data *d = data->user_data;

	size_t out_len = net_buf_linearize(d->model, sizeof(d->model) - 1, data->rx_buf, 0, len);

	d->model[out_len] = '\0';
	LOG_INF("Model: %s", d->model);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_cgmr)
{
	size_t out_len;
	char *p;

	struct sim800l_data *d = data->user_data;

	out_len = net_buf_linearize(d->revision, sizeof(d->revision) - 1, data->rx_buf, 0, len);
	d->revision[out_len] = '\0';

	/* The module prepends a Revision: */
	p = strchr(d->revision, ':');
	if (p) {
		out_len = strlen(p + 1);
		memmove(d->revision, p + 1, out_len + 1);
	}

	LOG_INF("Revision: %s", d->revision);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_cgsn)
{
	struct sim800l_data *d = data->user_data;

	size_t out_len = net_buf_linearize(d->imei, sizeof(d->imei) - 1, data->rx_buf, 0, len);

	d->imei[out_len] = '\0';
	LOG_INF("IMEI: %s", d->imei);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_cifsr)
{
	struct sim800l_data *d = data->user_data;
	size_t out_len;

	/* Extract IP address from response */
	out_len = net_buf_linearize(d->ip_addr, sizeof(d->ip_addr) - 1, data->rx_buf, 0, len);
	d->ip_addr[out_len] = '\0';

	/* Remove any trailing whitespace or newlines */
	while (out_len > 0 && (d->ip_addr[out_len - 1] == '\r' || d->ip_addr[out_len - 1] == '\n' ||
			       d->ip_addr[out_len - 1] == ' ')) {
		d->ip_addr[--out_len] = '\0';
	}

	LOG_INF("Local IP address: %s", d->ip_addr);

	return 0;
}

/* Command arrays */

const struct modem_cmd response_cmds[] = {
	MODEM_CMD("OK", on_cmd_ok, 0U, ""),
	MODEM_CMD("ERROR", on_cmd_error, 0U, ""),
	MODEM_CMD("+CME ERROR: ", on_cmd_exterror, 1U, ""),
	MODEM_CMD_DIRECT(">", on_cmd_tx_ready),
};

const size_t response_cmds_len = ARRAY_SIZE(response_cmds);

const struct modem_cmd unsolicited_cmds[] = {
	MODEM_CMD("+FTPGET: 1,", on_urc_ftpget, 1U, ""),
	MODEM_CMD("RDY", on_urc_rdy, 0U, ""),
	MODEM_CMD("NORMAL POWER DOWN", on_urc_pwr_down, 0U, ""),
	MODEM_CMD("+CPIN: ", on_urc_cpin, 1U, ","),
	MODEM_CMD("*PSUTTZ: ", on_psuttz, 7U, ","),
	MODEM_CMD("+CIEV: ", on_urc_ciev, 0U, ","),
	MODEM_CMD("+CREG: ", on_urc_creg, 1U, ","),
};

const size_t unsolicited_cmds_len = ARRAY_SIZE(unsolicited_cmds);

const struct setup_cmd setup_cmds[] = {
	SETUP_CMD("AT+CGMI", "", on_cmd_cgmi, 0U, ""),
	SETUP_CMD("AT+CGMM", "", on_cmd_cgmm, 0U, ""),
	SETUP_CMD("AT+CGMR", "", on_cmd_cgmr, 0U, ""),
	SETUP_CMD("AT+CGSN", "", on_cmd_cgsn, 0U, ""),
};

const size_t setup_cmds_len = ARRAY_SIZE(setup_cmds);

/* Helper to provide CIFSR command for local use */
static const struct modem_cmd cifsr_cmd_array[] = {
	MODEM_CMD_DIRECT("", on_cmd_cifsr),
};

const struct modem_cmd *sim800l_get_cifsr_cmd(void)
{
	return cifsr_cmd_array;
}
