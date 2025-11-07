// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2025 Blue Vending
 * Simcom SIM800L modem driver
 *
 */

#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket_offload.h>

#include "sim800l.h"

LOG_MODULE_REGISTER(modem_simcom_sim800l_offload, CONFIG_MODEM_SIM800L_LOG_LEVEL);

static struct zsock_addrinfo dns_result;
static struct sockaddr dns_result_addr;
static char dns_result_canonname[DNS_MAX_NAME_SIZE + 1];

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
	k_sem_give(&mdata.sem_dns);
	return ret;
}

/* Socket response handlers */
MODEM_CMD_DEFINE(on_cmd_cipstart)
{
	int socket_id, state;

	LOG_DBG("data len: %d, argc: %d", len, argc);
	k_sem_give(&mdata.sem_dns);
	socket_id = atoi(argv[0]);
	mdata.sockets[socket_id].is_connected = true;
	LOG_DBG("on_cmd_cipstart called");
	return 0;
	/* Response format: <socket_id>, "CONNECT OK" or "CONNECT FAIL" */

	if (socket_id < 0 || socket_id >= MDM_MAX_SOCKETS) {
		LOG_ERR("Invalid socket_id: %d", socket_id);
		return -EINVAL;
	}

	if (argc > 1 && strcmp(argv[1], "CONNECT OK") == 0) {
		mdata.sockets[socket_id].is_connected = true;
		LOG_DBG("Socket %d connected", socket_id);
	} else {
		mdata.sockets[socket_id].is_connected = false;
		LOG_ERR("Socket %d connection failed", socket_id);
		modem_cmd_handler_set_error(data, -ECONNREFUSED);
	}

	// k_sem_give(&mdata.sockets[socket_id].tx_sem);
	k_sem_give(&mdata.sem_dns);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_ciprxget)
{
	/* Response: +CIPRXGET: 2,<length>,<data> */
	struct sim800l_data *d = (struct sim800l_data *)data;

	if (argc >= 2) {
		int mode = atoi(argv[0]);
		int len = atoi(argv[1]);

		if (mode == 2 && len > 0) {
			/* Data follows, will be read by net_buf */
			d->rx_len = len;
			modem_cmd_handler_set_error(data, 0);
		}
	}
	return 0;
}

static const struct modem_cmd response_cmds_ciprxget[] = {
	MODEM_CMD("+CIPRXGET:", on_cmd_ciprxget, 2U, ","),
};

/*
 * Unlock the tx ready semaphore if '>' is received.
 */
MODEM_CMD_DIRECT_DEFINE(on_cmd_tx_ready)
{
	LOG_DBG("'> ' prompt received");
	k_sem_give(&mdata.sem_tx_ready);
	return len;
}

/* Separate handlers for OK vs FAIL */
MODEM_CMD_DEFINE(on_cmd_cipsend_ok)
{
	int socket_id = (argc > 0) ? atoi(argv[0]) : 0;

	LOG_INF("Socket %d: SEND OK", socket_id);
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_cipsend_fail)
{
	int socket_id = (argc > 0) ? atoi(argv[0]) : 0;

	LOG_ERR("Socket %d: SEND FAIL", socket_id);
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

int modem_offload_socket(int family, int type, int proto)
{
	int ret;

	ret = modem_socket_get(&mdata.socket_config, family, type, proto);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	errno = 0;
	LOG_INF("Created socket: %d", ret);
	return ret;
}

static int offload_close(void *obj)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	int ret;

	if (!sock) {
		errno = EBADF;
		return -1;
	}

	LOG_INF("Closing socket %d", sock->sock_fd);

	/* If socket is connected, send AT+CIPCLOSE to close connection */
	if (sock->is_connected) {
		ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, NULL, 0U,
				     "AT+CIPCLOSE", &mdata.sem_response, MDM_CMD_TIMEOUT);
		if (ret < 0) {
			LOG_WRN("Failed to close connection: %d", ret);
		}
		sock->is_connected = false;
	}

	/* Put socket back to pool */
	modem_socket_put(&mdata.socket_config, sock->sock_fd);

	errno = 0;
	return 0;
}

static int offload_connect(void *obj, const struct sockaddr *addr, socklen_t addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	char buf[128];
	char ip_str[INET_ADDRSTRLEN];
	uint16_t port;
	const char *proto;
	int ret;

	if (!addr) {
		errno = EINVAL;
		return -1;
	}

	/* Only support IPv4 for now */
	if (addr->sa_family != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	const struct sockaddr_in *addr_in = (const struct sockaddr_in *)addr;

	if (modem_socket_is_allocated(&mdata.socket_config, sock) == false) {
		LOG_ERR("Invalid socket id %d from fd %d", sock->id, sock->sock_fd);
		errno = EINVAL;
		return -1;
	}

	if (sock->is_connected == true) {
		LOG_ERR("Socket is already connected! id: %d, fd: %d", sock->id, sock->sock_fd);
		errno = EISCONN;
		return -1;
	}

	/* Extract IP, protocol and port */
	ret = modem_context_sprint_ip_addr(addr, ip_str, sizeof(ip_str));
	if (ret < 0) {
		LOG_ERR("Failed to format IP!");
		errno = ENOMEM;
		return -1;
	}

	port = ntohs(addr_in->sin_port);

	/* Determine protocol type */
	if (sock->type == SOCK_STREAM) {
		proto = "TCP";
	} else if (sock->type == SOCK_DGRAM) {
		proto = "UDP";
	} else {
		errno = EPROTONOSUPPORT;
		return -1;
	}

	LOG_INF("Connecting socket %d to %s:%u via %s", sock->sock_fd, ip_str, port, proto);

	/* Build AT+CIPSTART command */
	snprintf(buf, sizeof(buf), "AT+CIPSTART=%d,\"%s\",\"%s\",%u", sock->id, proto, ip_str,
		 port);

	const struct modem_cmd cipstart_cmd[] = {
		MODEM_CMD("", on_cmd_cipstart, 0U, ""),
	};
	/* Send connect command */
	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, cipstart_cmd,
			     ARRAY_SIZE(cipstart_cmd), buf, &mdata.sem_dns, K_SECONDS(30));

	if (ret < 0) {
		LOG_ERR("Failed to connect: %d", ret);
		errno = -ret;
		return -1;
	}

	/* Mark socket as connected */
	sock->is_connected = true;
	LOG_INF("Socket %d connected successfully", sock->sock_fd);
	errno = 0;
	return 0;
}

/*
 * Send data over a given socket.
 *
 * First we signal the module that we want to send data over a socket.
 * This is done by sending AT+CIPSEND=<socket_id>,<length>\r\n.
 * If multi IP connection is established (+CIPMUX=1)
 * If connection is not established or module is disconnected:
 * If error is related to ME functionality:
 * +CME ERROR <err>
 * If sending is successful:
 * When +CIPQSEND=0
 * <n>,SEND OK
 * When +CIPQSEND=1
 * DATA ACCEPT:<n>,<length>
 * If sending fails:
 * <n>,SEND FAIL
 */
static ssize_t offload_sendto(void *obj, const void *buf, size_t len, int flags,
			      const struct sockaddr *dest_addr, socklen_t addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	char ctrlz = 0x1A; /* Ctrl+Z character to indicate end of data */
	char cmd[32];
	int ret;
	/* Only need to catch the '>' prompt - send confirmation handlers are in unsolicited array
	 */
	struct modem_cmd handler_cmds[] = {
		MODEM_CMD_DIRECT(">", on_cmd_tx_ready),
		/* TODO: Currently is a hack to match socket ID, and finish the sending in preoper
		 * way. This should be improved to properly handle multiple sockets.
		 */
		MODEM_CMD("0, SEND OK", on_cmd_cipsend_ok, 0U,
			  ","), /* Socket: <n>, SEND OK (note leading space) */
		/* TODO: Not working correctly for multiple sockets */
		MODEM_CMD("SEND FAIL", on_cmd_cipsend_fail, 1U,
			  ","), /* Socket: <n>, SEND FAIL (note leading space) */
	};

	if (!buf || len == 0) {
		errno = EINVAL;
		return -1;
	}

	/* Check if socket is connected for TCP */
	if (sock->type == SOCK_STREAM && !sock->is_connected) {
		errno = ENOTCONN;
		return -1;
	}

	/* For UDP, connection is optional but dest_addr must be provided if not connected */
	if (sock->type == SOCK_DGRAM && !sock->is_connected && !dest_addr) {
		errno = EDESTADDRREQ;
		return -1;
	}

	/* Limit send size to avoid buffer overflow */
	if (len > MDM_MAX_DATA_LENGTH) {
		len = MDM_MAX_DATA_LENGTH;
	}

	LOG_DBG("Sending %zu bytes on socket %d", len, sock->sock_fd);

	/* Build AT+CIPSEND command with socket ID (multi-IP mode requires socket ID) */
	snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%zu", sock->id, len);

	/* Reset semaphore before sending */
	k_sem_reset(&mdata.sem_tx_ready);

	ret = modem_cmd_send_nolock(&mdata.ctx.iface, &mdata.ctx.cmd_handler, NULL, 0U, cmd, NULL,
				    K_NO_WAIT);

	if (ret < 0) {
		LOG_ERR("Failed to initiate send or get prompt: %d", ret);
		errno = EIO;
		return -1;
	}

	/* set command handlers */
	ret = modem_cmd_handler_update_cmds(&mdata.cmd_handler_data, handler_cmds,
					    ARRAY_SIZE(handler_cmds), true);
	if (ret < 0) {
		LOG_ERR("Failed to set command handlers: %d", ret);
		errno = EIO;
		return -1;
	}

	/* Wait for 'SEND OK' or 'SEND FAIL' */
	k_sem_reset(&mdata.sem_response);

	/* Wait for '>' */
	ret = k_sem_take(&mdata.sem_tx_ready, K_MSEC(5000));
	if (ret < 0) {
		/* Didn't get the data prompt - Exit. */
		LOG_DBG("Timeout waiting for tx");
		errno = EIO;
		return -1;
	}

	/* Send the actual data */
	modem_cmd_send_data_nolock(&mdata.ctx.iface, buf, len);
	modem_cmd_send_data_nolock(&mdata.ctx.iface, &ctrlz, 1);

	ret = k_sem_take(&mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Timeout waiting for send confirmation");
		errno = ETIMEDOUT;
		return -1;
	}
	LOG_DBG("Successfully sent %zu bytes", len);
	return (ssize_t)len;
}

/*
 * Offloads write by writing to a given socket.
 */
static ssize_t offload_write(void *obj, const void *buffer, size_t count)
{
	return offload_sendto(obj, buffer, count, 0, NULL, 0);
}

static ssize_t offload_recvfrom(void *obj, void *buf, size_t max_len, int flags,
				struct sockaddr *src_addr, socklen_t *addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;

	LOG_WRN("recvfrom not yet implemented for socket %d", sock->sock_fd);
	errno = ENOTSUP;
	return -1;
}

static ssize_t offload_read(void *obj, void *buf, size_t max_len)
{
	/* Simply call recvfrom with NULL address parameters and no flags */
	return offload_recvfrom(obj, buf, max_len, 0, NULL, NULL);
}
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
	if (mdata.state != SIM800L_STATE_READY) {
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

	ret = snprintk(sendbuf, sizeof(sendbuf), "AT+CDNSGIP=\"%s\",%u,%u", node, mdata.dns.recount,
		       mdata.dns.timeout);
	if (ret < 0) {
		LOG_ERR("Formatting dns query failed");
		return ret;
	}

	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, cmd, ARRAY_SIZE(cmd),
			     sendbuf, &mdata.sem_dns, MDM_DNS_TIMEOUT);
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

bool modem_offload_is_supported(int family, int type, int proto)
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

static inline uint8_t *modem_get_mac(const struct device *dev)
{
	struct sim800l_data *data = dev->data;
	uint32_t hash_value;

	data->mac_addr[0] = 0x00;
	data->mac_addr[1] = 0x10;

	/* use IMEI for mac_addr */
	hash_value = hash32(data->imei, strlen(data->imei));

	UNALIGNED_PUT(hash_value, (uint32_t *)(data->mac_addr + 2));

	return data->mac_addr;
}

const struct socket_op_vtable offload_socket_fd_op_vtable = {
	.fd_vtable =
		{
			.read = offload_read,
			.write = offload_write,
			.close = offload_close,
			.ioctl = NULL,
		},
	.bind = NULL,
	.connect = offload_connect,
	.sendto = offload_sendto,
	.recvfrom = offload_recvfrom,
	.listen = NULL,
	.accept = NULL,
	.sendmsg = NULL,
	.getsockopt = NULL,
	.setsockopt = NULL,
};

/* Setup the Modem NET Interface. */
void modem_net_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);

	net_if_set_link_addr(iface, modem_get_mac(dev), sizeof(mdata.mac_addr), NET_LINK_ETHERNET);

	mdata.netif = iface;

	socket_offload_dns_register(&offload_dns_ops);

	net_if_socket_offload_set(iface, modem_offload_socket);
}
