// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem sample application
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

LOG_MODULE_REGISTER(sim800l_test, LOG_LEVEL_DBG);

const struct device *modem = DEVICE_DT_GET(DT_ALIAS(modem));

/* Simple HTTP GET request test */
static int test_http_get(void)
{
	int sock;
	int ret;
	struct sockaddr_in addr;
	char request[] = "12345678910\n";
	char response[512];

	LOG_INF("Testing HTTP GET...");

	/* Create TCP socket */
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("Failed to create socket: %d", errno);
		return -1;
	}
	LOG_INF("Socket created: %d", sock);

	/* Connect to tcpbin.com:4242 */

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(4242);
	/* tcpbin.com IP - you may need to update this */
	zsock_inet_pton(AF_INET, "45.79.112.203", &addr.sin_addr);

	LOG_INF("Connecting to tcpbin.com:4242...");
	ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERR("Failed to connect: %d", errno);
		zsock_close(sock);
		return -1;
	}
	LOG_INF("Connected successfully!");

	/* Send HTTP request */
	LOG_INF("Sending HTTP request...");
	ret = zsock_send(sock, request, strlen(request), 0);
	if (ret < 0) {
		LOG_ERR("Failed to send: %d", errno);
		zsock_close(sock);
		return -1;
	}
	LOG_INF("Sent %d bytes", ret);

	/* Receive response */
	LOG_INF("Waiting for response...");
	ret = zsock_recv(sock, response, sizeof(response) - 1, 0);
	if (ret < 0) {
		LOG_ERR("Failed to receive: %d", errno);
		zsock_close(sock);
		return -1;
	}

	response[ret] = '\0';
	LOG_INF("Received %d bytes:", ret);
	LOG_INF("%s", response);

	/* Close socket */
	zsock_close(sock);
	LOG_INF("Socket closed");
	/* Compare sent and received data */
	if (ret != strlen(request) || memcmp(request, response, ret) != 0) {
		LOG_ERR("Data mismatch!");
		return -1;
	}
	return 0;
}

/* Simple socket creation test */
static int test_socket_create(void)
{
	int sock;

	LOG_INF("Testing socket creation...");

	/* Create TCP socket */
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("Failed to create TCP socket: %d", errno);
		return -1;
	}
	LOG_INF("TCP socket created: %d", sock);
	zsock_close(sock);

	/* Create UDP socket */
	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Failed to create UDP socket: %d", errno);
		return -1;
	}
	LOG_INF("UDP socket created: %d", sock);
	zsock_close(sock);

	LOG_INF("Socket creation test PASSED");
	return 0;
}

int main(void)
{

	int ret;

	LOG_INF("SIM800L Modem Driver Test");

	/* Resume modem device (power on) */
	pm_device_action_run(modem, PM_DEVICE_ACTION_RESUME);
	/* Check if modem device is ready */
	if (!device_is_ready(modem)) {
		LOG_ERR("Modem device not ready!");
		return -1;
	}
	LOG_INF("Modem device is ready");

	// /* Test 1: Socket creation */
	// ret = test_socket_create();
	// if (ret < 0) {
	// 	LOG_ERR("Socket creation test FAILED");
	// }

	k_sleep(K_SECONDS(2));

	/* Test 2: HTTP GET (comment out if no internet connection) */
	ret = test_http_get();
	if (ret < 0) {
		LOG_ERR("HTTP GET test FAILED");
	} else {
		LOG_INF("HTTP GET test PASSED");
	}

	LOG_INF("\n=== Test completed ===\n");

	/* Keep running */
	while (1) {
		k_sleep(K_SECONDS(30));
	}

	return 0;
}
