// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem driver
 *
 */

#define DT_DRV_COMPAT simcom_sim800l

#include <zephyr/logging/log.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(simcom_sim800l, CONFIG_LOG_DEFAULT_LEVEL);

#include <string.h>
#include <stdlib.h>

#include "sim800l.h"

/* SIM800L specific state enum */
enum sim800l_state {
	SIM800L_STATE_IDLE = 0,
	SIM800L_STATE_RESET,
	SIM800L_STATE_INIT,
	SIM800L_STATE_READY,
	SIM800L_STATE_ERROR,
};

/* Power management and GPIO control */
struct sim800l_data {
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

	bool powered;
	struct k_work_delayable timeout_work;
	enum sim800l_state state;
	// struct modem_pipe *uart_pipe;
	// struct modem_cmux cmux;
	// struct modem_chat chat;
	struct k_sem sem_tx_ready;
	struct k_sem sem_response;
	struct k_sem sem_dns;
};

struct sim800l_config {
	const struct device *uart;
};

static struct k_thread modem_rx_thread;
static K_KERNEL_STACK_DEFINE(modem_rx_stack, 1028); /* 1024 + 4 sentinel */
NET_BUF_POOL_DEFINE(mdm_recv_pool, MDM_RECV_MAX_BUF, MDM_RECV_BUF_SIZE, 0, NULL);

/* Device instance definition */
static struct sim800l_data sim800l_data_0 = {
	.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(0, mdm_reset_gpios, {}),
	.powered = false,
};

static const struct sim800l_config sim800l_config_0 = {
	.uart = DEVICE_DT_GET(DT_INST_BUS(0)),
};

static struct modem_context mctx;
static struct zsock_addrinfo dns_result;
static struct sockaddr dns_result_addr;
static char dns_result_canonname[DNS_MAX_NAME_SIZE + 1];

static inline uint32_t hash32(char *str, int len)
{
#define HASH_MULTIPLIER 37
	uint32_t h = 0;
	int i;

	for (i = 0; i < len; ++i) {
		h = (h * HASH_MULTIPLIER) + str[i];
	}

	return h;
}

MODEM_CMD_DEFINE(on_cmd_ok)
{
	struct sim800l_data *drv_data = data->user_data;
	LOG_DBG("OK received");
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&drv_data->sem_response);
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

/*
 * Unlock the tx ready semaphore if '> ' is received.
 */
MODEM_CMD_DIRECT_DEFINE(on_cmd_tx_ready)
{
	k_sem_give(&sim800l_data_0.sem_tx_ready);
	return len;
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
	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_urc_pwr_down)
{
	LOG_DBG("POWER DOWN received");
	return 0;
}

MODEM_CMD_DEFINE(on_urc_cpin)
{
	LOG_DBG("CPIN: %s", argv[0]);
	return 0;
}

/*
 * Parses the dns response from the modem.
 *
 * Response on success:
 * +CDNSGIP: 1,<domain name>,<IPv4>[,<IPv6>]
 *
 * Response on failure:
 * +CDNSGIP: 0,<err>
 */
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

// /*
//  * Read manufacturer identification.
//  */
// MODEM_CMD_DEFINE(on_cmd_cgmi)
// {
// 	size_t out_len =
// 		net_buf_linearize(sim800l_data_0.manufacturer,
// 				  sizeof(sim800l_data_0.manufacturer) - 1, data->rx_buf, 0, len);
// 	sim800l_data_0.manufacturer[out_len] = '\0';
// 	LOG_INF("Manufacturer: %s", sim800l_data_0.manufacturer);
// 	return 0;
// }

/*
 * Possible responses by the sim800l.
 */
static const struct modem_cmd response_cmds[] = {
	MODEM_CMD("OK", on_cmd_ok, 0U, ""),
	MODEM_CMD("ERROR", on_cmd_error, 0U, ""),
	MODEM_CMD("+CME ERROR: ", on_cmd_exterror, 1U, ""),
	MODEM_CMD_DIRECT(">", on_cmd_tx_ready),
};

static const struct modem_cmd unsolicited_cmds[] = {
	MODEM_CMD("+FTPGET: 1,", on_urc_ftpget, 1U, ""),
	MODEM_CMD("RDY", on_urc_rdy, 0U, ""),
	MODEM_CMD("NORMAL POWER DOWN", on_urc_pwr_down, 0U, ""),
	MODEM_CMD("+CPIN: ", on_urc_cpin, 1U, ","),

};

// /*
//  * Commands to be sent at setup.
//  */
// static const struct setup_cmd setup_cmds[] = {
// 	SETUP_CMD("AT+CGMI", "", on_cmd_cgmi, 0U, ""),
// 	// SETUP_CMD("AT+CGMM", "", on_cmd_cgmm, 0U, ""),
// 	// SETUP_CMD("AT+CGMR", "", on_cmd_cgmr, 0U, ""),
// 	// SETUP_CMD("AT+CGSN", "", on_cmd_cgsn, 0U, ""),
// };

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

	if (data->powered) {
		return 0;
	}

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

	if (!data->powered) {
		return 0;
	}

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
 * Perform a dns lookup.
 */
static int offload_getaddrinfo(const char *node, const char *service,
			       const struct zsock_addrinfo *hints, struct zsock_addrinfo **res)
{
	struct modem_cmd cmd[] = {MODEM_CMD("+CDNSGIP: ", on_cmd_cdnsgip, 2U, ",")};
	char sendbuf[sizeof("AT+CDNSGIP=\"\",##,#####") + 128];
	uint32_t port = 0;
	int ret;

	/* Modem is not attached to the network. */
	if (sim800l_data_0.state != SIM800L_STATE_READY) {
		LOG_ERR("Modem currently not attached to the network!");
		return DNS_EAI_AGAIN;
	}

	/* init result */
	(void)memset(&dns_result, 0, sizeof(dns_result));
	(void)memset(&dns_result_addr, 0, sizeof(dns_result_addr));

	/* Currently only support IPv4. */
	dns_result.ai_family = AF_INET;
	dns_result_addr.sa_family = AF_INET;
	dns_result.ai_addr = &dns_result_addr;
	dns_result.ai_addrlen = sizeof(dns_result_addr);
	dns_result.ai_canonname = dns_result_canonname;
	dns_result_canonname[0] = '\0';

	if (service) {
		port = atoi(service);
		if (port < 1 || port > USHRT_MAX) {
			return DNS_EAI_SERVICE;
		}
	}

	if (port > 0U) {
		if (dns_result.ai_family == AF_INET) {
			net_sin(&dns_result_addr)->sin_port = htons(port);
		}
	}

	/* Check if node is an IP address */
	if (net_addr_pton(dns_result.ai_family, node,
			  &((struct sockaddr_in *)&dns_result_addr)->sin_addr) == 0) {
		*res = &dns_result;
		return 0;
	}

	/* user flagged node as numeric host, but we failed net_addr_pton */
	if (hints && hints->ai_flags & AI_NUMERICHOST) {
		return DNS_EAI_NONAME;
	}

	ret = snprintk(sendbuf, sizeof(sendbuf), "AT+CDNSGIP=\"%s\",%u,%u", node,
		       sim800l_data_0.dns.recount, sim800l_data_0.dns.timeout);
	if (ret < 0) {
		LOG_ERR("Formatting dns query failed");
		return ret;
	}

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, cmd, ARRAY_SIZE(cmd), sendbuf,
			     &sim800l_data_0.sem_dns, MDM_DNS_TIMEOUT);
	if (ret < 0) {
		return ret;
	}

	*res = (struct zsock_addrinfo *)&dns_result;
	return 0;
}

/*
 * Free addrinfo structure.
 */
static void offload_freeaddrinfo(struct zsock_addrinfo *res)
{
	/* No need to free static memory. */
	ARG_UNUSED(res);
}

/*
 * DNS vtable.
 */
const struct socket_dns_offload offload_dns_ops = {
	.getaddrinfo = offload_getaddrinfo,
	.freeaddrinfo = offload_freeaddrinfo,
};

static inline uint8_t *modem_get_mac(const struct device *dev)
{
	struct sim800l_data *data = dev->data;
	uint32_t hash_value;

	data->mac_addr[0] = 0x00;
	data->mac_addr[1] = 0x10;

	/* use IMEI for mac_addr */
	hash_value = hash32(sim800l_data_0.imei, strlen(sim800l_data_0.imei));

	UNALIGNED_PUT(hash_value, (uint32_t *)(data->mac_addr + 2));

	return data->mac_addr;
}

// static int modem_boot(bool allow_autobaud)
// {
// 	/* SIM800L does not support autobaud - assume fixed baudrate */
// 	return 0;
// }

static int modem_setup(struct sim800l_data *data)
{
	uint8_t boot_tries = 0;
	int ret = 0;

	LOG_DBG("Setting up modem");

	// /* Disable echo on successful boot */
	// ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "ATE0",
	// &data->sem_response, 		     K_MSEC(500)); if (ret != 0) {
	// LOG_ERR("Disabling echo failed"); 	return ret;
	// }

	/* Try boot multiple times in case modem was already on */
	while (boot_tries++ <= MDM_BOOT_TRIES) {
		/* Reset modem and wait for ready indication */
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "AT+CFUN=1,1",
				     &data->sem_response, K_MSEC(500));
		if (ret != 0) {
			LOG_ERR("Reset failed");
			return ret;
		}
		LOG_DBG("Modem booted successfully");
		break;
	}
	// 	k_work_cancel_delayable(&mdata.rssi_query_work);

	// 	ret = modem_boot(true);
	// 	if (ret < 0) {
	// 		LOG_ERR("Booting modem failed!!");
	// 		return ret;
	// 	}

	// 	ret = modem_cmd_handler_setup_cmds(&mctx.iface, &mctx.cmd_handler, setup_cmds,
	// 					   ARRAY_SIZE(setup_cmds), &mdata.sem_response,
	// 					   MDM_REGISTRATION_TIMEOUT);
	// 	if (ret < 0) {
	// 		LOG_ERR("Modem setup commands failed!!");
	// 		return ret;
	// 	}
	// 	return ret;

	return ret;
}

static ssize_t offload_read(void *obj, void *buffer, size_t count)
{
	errno = ENOTSUP;
	return -1;
}

static ssize_t offload_write(void *obj, const void *buffer, size_t count)
{
	errno = ENOTSUP;
	return -1;
}

static int offload_close(void *obj)
{
	errno = ENOTSUP;
	return -1;
}

static int offload_ioctl(void *obj, unsigned int request, va_list args)
{
	errno = ENOTSUP;
	return -1;
}

static int offload_connect(void *obj, const struct sockaddr *addr, socklen_t addrlen)
{
	errno = ENOTSUP;
	return -1;
}

static ssize_t offload_sendto(void *obj, const void *buf, size_t len, int flags,
			      const struct sockaddr *dest_addr, socklen_t addrlen)
{
	errno = ENOTSUP;
	return -1;
}

static ssize_t offload_recvfrom(void *obj, void *buf, size_t max_len, int flags,
				struct sockaddr *src_addr, socklen_t *addrlen)
{
	errno = ENOTSUP;
	return -1;
}

const struct socket_op_vtable offload_socket_fd_op_vtable = {
	.fd_vtable =
		{
			.read = offload_read,
			.write = offload_write,
			.close = offload_close,
			.ioctl = offload_ioctl,
		},
	.connect = offload_connect,
	.sendto = offload_sendto,
	.recvfrom = offload_recvfrom,
};

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
		modem_iface_uart_rx_wait(&mctx.iface, K_FOREVER);

		/* Process AT command responses and unsolicited messages */
		modem_cmd_handler_process(&mctx.cmd_handler, &mctx.iface);
	}
}

static int modem_init(const struct device *dev)
{
	struct sim800l_data *data = dev->data;
	const struct sim800l_config *config = dev->config;
	int ret;

	LOG_DBG("Initializing modem");

	k_sem_init(&data->sem_tx_ready, 0, 1);
	k_sem_init(&data->sem_response, 0, 1);
	k_sem_init(&data->sem_dns, 0, 1);

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

	/* Socket config. */
	ret = modem_socket_init(&data->socket_config, &data->sockets[0], ARRAY_SIZE(data->sockets),
				MDM_BASE_SOCKET_NUM, true, &offload_socket_fd_op_vtable);
	if (ret < 0) {
		return ret;
	}

	/* Command handler. */
	const struct modem_cmd_handler_config cmd_handler_config = {
		.match_buf = &data->cmd_match_buf[0],
		.match_buf_len = sizeof(data->cmd_match_buf),
		.buf_pool = &mdm_recv_pool,
		.alloc_timeout = BUF_ALLOC_TIMEOUT,
		.eol = "\r\n",
		.user_data = data,
		.response_cmds = response_cmds,
		.response_cmds_len = ARRAY_SIZE(response_cmds),
		.unsol_cmds = unsolicited_cmds,
		.unsol_cmds_len = ARRAY_SIZE(unsolicited_cmds),
	};

	ret = modem_cmd_handler_init(&mctx.cmd_handler, &data->cmd_handler_data,
				     &cmd_handler_config);
	if (ret < 0) {
		return ret;
	}

	/* Uart handler. */
	const struct modem_iface_uart_config uart_config = {
		.rx_rb_buf = &data->iface_rb_buf[0],
		.rx_rb_buf_len = sizeof(data->iface_rb_buf),
		.dev = config->uart,
		.hw_flow_control = false,
	};

	ret = modem_iface_uart_init(&mctx.iface, &data->iface_data, &uart_config);
	if (ret < 0) {
		return ret;
	}

	LOG_DBG("Powering on modem");
	ret = modem_power_on(dev);
	if (ret < 0) {
		LOG_ERR("Failed to power on modem: %d", ret);
		return ret;
	}
#ifdef CONFIG_PM_DEVICE
	pm_device_init_suspended(dev);
#endif /* CONFIG_PM_DEVICE */

	/* Modem data storage. */
	mctx.data_manufacturer = data->manufacturer;
	mctx.data_model = data->model;
	mctx.data_revision = data->revision;
	mctx.data_imei = data->imei;
	mctx.driver_data = data;
	ret = modem_context_register(&mctx);
	if (ret < 0) {
		LOG_ERR("Error registering modem context: %d", ret);
		return ret;
	}

	k_tid_t tid = k_thread_create(&modem_rx_thread, modem_rx_stack,
				      K_KERNEL_STACK_SIZEOF(modem_rx_stack), modem_rx, NULL, NULL,
				      NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	k_thread_name_set(tid, "modem_rx");
	k_sleep(K_MSEC(100));
	return modem_setup(data);
}

#ifdef CONFIG_PM_DEVICE
PM_DEVICE_DT_INST_DEFINE(0, modem_pm_action);
#endif

#define MODEM_SIMCOM_SIM800L_INIT_PRIORITY 90

static bool offload_is_supported(int family, int type, int proto)
{
	if (family != AF_INET && family != AF_INET6) {
		return false;
	}

	if (type != SOCK_DGRAM && type != SOCK_STREAM) {
		return false;
	}

	if (proto != IPPROTO_TCP && proto != IPPROTO_UDP) {
		return false;
	}

	return true;
}

static int offload_socket(int family, int type, int proto)
{
	int ret;

	ret = modem_socket_get(&sim800l_data_0.socket_config, family, type, proto);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	errno = 0;
	return ret;
}

/* Setup the Modem NET Interface. */
static void modem_net_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct sim800l_data *data = dev->data;

	net_if_set_link_addr(iface, modem_get_mac(dev), sizeof(data->mac_addr), NET_LINK_ETHERNET);

	data->netif = iface;

	socket_offload_dns_register(&offload_dns_ops);

	net_if_socket_offload_set(iface, offload_socket);
}

static struct offloaded_if_api api_funcs = {
	.iface_api.init = modem_net_iface_init,
};

// /* Register device with the networking stack. */
NET_DEVICE_DT_INST_OFFLOAD_DEFINE(0, modem_init, NULL, &sim800l_data_0, &sim800l_config_0,
				  CONFIG_GPIO_INIT_PRIORITY, &api_funcs, MDM_MAX_DATA_LENGTH);
NET_SOCKET_OFFLOAD_REGISTER(simcom_sim800l, CONFIG_GPIO_INIT_PRIORITY, AF_UNSPEC,
			    offload_is_supported, offload_socket);
