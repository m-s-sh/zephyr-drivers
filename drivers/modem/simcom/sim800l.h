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

#define BUF_ALLOC_TIMEOUT K_SECONDS(1)
#define MDM_DNS_TIMEOUT   K_SECONDS(210)

#define MDM_MAX_DATA_LENGTH 1024

#define MDM_IMEI_LENGTH     16
#define MDM_MODEL_LENGTH    16
#define MDM_REVISION_LENGTH 64
#define MDM_MAX_SOCKETS     5
#define MDM_RECV_MAX_BUF    30
#define MDM_RECV_BUF_SIZE   1024
#define MDM_BOOT_TRIES      3
#define MDM_BASE_SOCKET_NUM 0

#define MDM_MANUFACTURER_LENGTH 12

#endif /* SIMCOM_SIM800L_H */
