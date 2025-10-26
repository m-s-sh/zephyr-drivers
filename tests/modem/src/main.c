// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem sample application
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>

const struct device *modem = DEVICE_DT_GET(DT_ALIAS(modem));

int main(void)
{
	printk("Starting SIM800L modem sample application\n");

	/* Application code to initialize and use the SIM800L modem goes here */
	printk("Powering on\n");
	//pm_device_action_run(modem, PM_DEVICE_ACTION_RESUME);

	while(1) {
		k_sleep(K_SECONDS(10));
		printk("Modem running...\n");
	}
	return 0;
}
