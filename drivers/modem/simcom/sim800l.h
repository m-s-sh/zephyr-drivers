/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SIM800L modem driver
 *
 * This driver provides an interface for controlling the SIM800L modem
 * and managing its power states.
 *
 * Copyright (c) 2025 Blue Vending
 *
 */

#ifndef SIMCOM_SIM800L_H
#define SIMCOM_SIM800L_H

#include <modem_context.h>
#include <modem_iface_uart.h>
#include <modem_cmd_handler.h>
#include <modem_socket.h>

#define BUF_ALLOC_TIMEOUT        K_SECONDS(1)
#define MDM_DNS_TIMEOUT          K_SECONDS(210)
#define MDM_REGISTRATION_TIMEOUT K_SECONDS(180)
#define MDM_CMD_TIMEOUT          K_SECONDS(10)
#define MDM_WAIT_FOR_RSSI_DELAY  K_SECONDS(2)
#define MDM_RSSI_TIMEOUT_SECS    30
#define MDM_MAX_CGATT_WAITS      30

#define MDM_MAX_AUTOBAUD    5
#define MDM_MAX_DATA_LENGTH 1024

#define MDM_IMEI_LENGTH     16
#define MDM_MODEL_LENGTH    16
#define MDM_REVISION_LENGTH 64
/* SIM800L supports total 5 connections (socket IDs 0-4).
 * When acting as TCP server, one socket is used for listening,
 * leaving 4 sockets for client connections.
 * For client-only mode, all 5 sockets (0-4) can be used.
 */
#define MDM_MAX_SOCKETS     5 /* Total sockets: IDs 0-4 */
#define MDM_BASE_SOCKET_NUM 0 /* First socket ID */
#define MDM_RECV_MAX_BUF    30
#define MDM_RECV_BUF_SIZE   1024
#define MDM_BOOT_TRIES      3

#define MDM_WAIT_FOR_RSSI_COUNT 30

#define MDM_MANUFACTURER_LENGTH 12

/* SIM800L specific state enum */
enum sim800l_state {
	SIM800L_STATE_IDLE = 0,
	SIM800L_STATE_RESET,
	SIM800L_STATE_INIT,
	SIM800L_STATE_READY,
	SIM800L_STATE_ERROR,
};

enum sim800l_status_flags {
	SIM800L_STATUS_FLAG_POWERED = 0x01,
	SIM800L_STATUS_FLAG_CPIN_READY = 0x02,
	SIM800L_STATUS_FLAG_ATTACHED = 0x04,
	SIM800L_STATUS_FLAG_PDP_ACTIVE = 0x08,
};

struct sim800l_data {
	struct modem_context ctx;
	/* Modem status flags */
	uint32_t status_flags;
	/*
	 * Network interface of the sim module.
	 */
	struct net_if *netif;
	uint8_t mac_addr[6];

	/*
	 * Uart interface of the modem.
	 */
	struct modem_iface_uart_data iface_data;
	uint8_t iface_rb_buf[MDM_MAX_DATA_LENGTH];

	/*
	 * Modem socket data.
	 */
	struct modem_socket_config socket_config;
	struct modem_socket sockets[MDM_MAX_SOCKETS];

	/* modem cmds */
	struct modem_cmd_handler_data cmd_handler_data;
	uint8_t cmd_match_buf[MDM_RECV_BUF_SIZE + 1];

	/*
	 * Uart interface of the modem.
	 */
	const struct gpio_dt_spec reset_gpio;

	/* DNS related variables */
	struct {
		/* Number of DNS retries */
		uint8_t recount;
		/* Timeout in milliseconds */
		uint16_t timeout;
	} dns;

	/*
	 * Information over the modem.
	 */
	char manufacturer[MDM_MANUFACTURER_LENGTH];
	char model[MDM_MODEL_LENGTH];
	char revision[MDM_REVISION_LENGTH];
	char imei[MDM_IMEI_LENGTH];
	int rssi;
	uint8_t network_registration;
	char ip_addr[16]; /* IPv4 address string */

	bool powered;
	struct k_work_delayable timeout_work;
	enum sim800l_state state;
	struct k_work_delayable rssi_query_work;

	/* Received data tracking */
	int rx_len;       /* Length of received data */
	int rx_socket_id; /* Socket ID that received data */

	/*
	 * Semaphore(s).
	 */
	struct k_sem sem_tx_ready;
	struct k_sem sem_rx_data;
	struct k_sem sem_response;
	struct k_sem sem_dns;
	struct k_sem boot_sem;
};

struct sim800l_config {
	const struct device *uart;
};

int modem_pdp_activate(void);
void modem_net_iface_init(struct net_if *iface);
void modem_query_rssi(void);

bool modem_offload_is_supported(int family, int type, int proto);
int modem_offload_socket(int family, int type, int proto);

extern struct sim800l_data mdata;
extern const struct socket_op_vtable offload_socket_fd_op_vtable;
#endif /* SIMCOM_SIM800L_H */
