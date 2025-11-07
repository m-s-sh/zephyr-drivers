// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem driver
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket_offload.h>

#include "sim800l.h"

LOG_MODULE_REGISTER(modem_simcom_sim800l_pdp, CONFIG_MODEM_SIM800L_LOG_LEVEL);

/*
 * Parse IP address from AT+CIFSR response.
 * Response format: <IP address> (e.g., "10.123.45.67")
 */
MODEM_CMD_DEFINE(on_cmd_cifsr)
{

	size_t out_len;

	/* Extract IP address from response */
	out_len = net_buf_linearize(mdata.ip_addr, sizeof(mdata.ip_addr) - 1, data->rx_buf, 0, len);
	mdata.ip_addr[out_len] = '\0';

	/* Remove any trailing whitespace or newlines */
	while (out_len > 0 &&
	       (mdata.ip_addr[out_len - 1] == '\r' || mdata.ip_addr[out_len - 1] == '\n' ||
		mdata.ip_addr[out_len - 1] == ' ')) {
		mdata.ip_addr[--out_len] = '\0';
	}

	LOG_INF("Local IP address: %s", mdata.ip_addr);

	/* TODO: Set the IP address on the network interface */
	/* This would involve parsing the IP and calling net_if_ipv4_addr_add() */
	k_sem_give(&mdata.sem_response);
	return 0;
}

/*
 * Handler for CGATT query.
 */
MODEM_CMD_DEFINE(on_cmd_cgatt)
{
	int cgatt = atoi(argv[0]);

	if (cgatt) {
		mdata.status_flags |= SIM800L_STATUS_FLAG_ATTACHED;
	} else {
		mdata.status_flags &= ~SIM800L_STATUS_FLAG_ATTACHED;
	}

	LOG_INF("CGATT: %d", cgatt);
	return 0;
}

/*
 * Parses the non urc C(E)REG and updates registration status.
 */
MODEM_CMD_DEFINE(on_cmd_cereg)
{
	mdata.network_registration = atoi(argv[1]);
	LOG_INF("CREG: %u", mdata.network_registration);
	return 0;
}

int modem_pdp_activate(void)
{
	/* PDP activation not implemented for SIM800L */
	int ret = 0;
	int counter = 0;
	const char *buf = "AT+CREG?";
	struct modem_cmd cmds[] = {MODEM_CMD("+CREG: ", on_cmd_cereg, 2U, ",")};

	const struct modem_cmd cifsr_cmd[] = {
		MODEM_CMD("", on_cmd_cifsr, 0U, ""),
	};

	/* Wait for acceptable rssi values. */
	modem_query_rssi();
	k_sleep(MDM_WAIT_FOR_RSSI_DELAY);

	counter = 0;
	while (counter++ < MDM_WAIT_FOR_RSSI_COUNT && (mdata.rssi >= 0 || mdata.rssi <= -1000)) {
		modem_query_rssi();
		k_sleep(MDM_WAIT_FOR_RSSI_DELAY);
	}

	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, cmds, ARRAY_SIZE(cmds), buf,
			     &mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to query registration.");
		return ret;
	}

	struct modem_cmd cgatt_cmd[] = {MODEM_CMD("+CGATT: ", on_cmd_cgatt, 1U, "")};

	/* Wait for GPRS Service's status to be attached */
	counter = 0;
	while (counter++ < MDM_MAX_CGATT_WAITS &&
	       (mdata.status_flags & SIM800L_STATUS_FLAG_ATTACHED) == 0) {
		ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, cgatt_cmd,
				     ARRAY_SIZE(cgatt_cmd), "AT+CGATT?", &mdata.sem_response,
				     MDM_CMD_TIMEOUT);
		if (ret < 0) {
			LOG_ERR("Failed to query cgatt.");
			return ret;
		}
		k_sleep(K_SECONDS(1));
	}

	if ((mdata.status_flags & SIM800L_STATUS_FLAG_CPIN_READY) == 0 ||
	    (mdata.status_flags & SIM800L_STATUS_FLAG_ATTACHED) == 0) {
		LOG_ERR("Fatal: Modem is not attached to GPRS network");
		return -ENETUNREACH;
	}

	/* Enable multi connection */
	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, NULL, 0U, "AT+CIPMUX=1",
			     &mdata.sem_response, K_SECONDS(5));
	if (ret < 0) {
		LOG_ERR("Failed to set multi connection");
		return ret;
	}

	/* Get the APN from config */
	const char *apn = CONFIG_MODEM_SIM800L_APN;
	/* Set APN if provided */
	// check if apn is null or empty and if so return error
	if (!apn || !*apn) {
		LOG_WRN("No APN configured");
		return -EINVAL;
	}

	char apn_cmd[64];

	snprintk(apn_cmd, sizeof(apn_cmd), "AT+CSTT=\"%s\"", apn);
	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, NULL, 0U, apn_cmd,
			     &mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to set APN");
		return ret;
	}

	/* Bring up wireless connection (GPRS or CSD)*/
	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, NULL, 0U, "AT+CIICR",
			     &mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to bring up wireless connection");
		return ret;
	}

	/* Get local IP address with custom handler */
	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, cifsr_cmd,
			     ARRAY_SIZE(cifsr_cmd), "AT+CIFSR", &mdata.sem_response,
			     MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to get local IP address");
		return ret;
	}

	return 0;
}
