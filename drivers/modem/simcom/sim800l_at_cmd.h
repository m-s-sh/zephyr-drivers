// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem driver - AT Command handlers
 */

#ifndef ZEPHYR_DRIVERS_MODEM_SIMCOM_SIM800L_AT_CMD_H_
#define ZEPHYR_DRIVERS_MODEM_SIMCOM_SIM800L_AT_CMD_H_

#include "sim800l.h"

/* Command arrays exported for use in sim800l.c */
extern const struct modem_cmd response_cmds[];
extern const size_t response_cmds_len;

extern const struct modem_cmd unsolicited_cmds[];
extern const size_t unsolicited_cmds_len;

extern const struct setup_cmd setup_cmds[];
extern const size_t setup_cmds_len;

/* Command handler declarations for local use in sim800l.c */
MODEM_CMD_DECLARE(on_cmd_cdnsgip);
MODEM_CMD_DIRECT_DECLARE(on_cmd_cifsr);

/* Helper function to get CIFSR command array */
const struct modem_cmd *sim800l_get_cifsr_cmd(void);

#endif /* ZEPHYR_DRIVERS_MODEM_SIMCOM_SIM800L_AT_CMD_H_ */
