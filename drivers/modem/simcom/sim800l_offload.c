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
#include <zephyr/net_buf.h>

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

/* Response format: <socket_id>, "CONNECT OK" or "CONNECT FAIL" */
MODEM_CMD_DEFINE(on_cmd_cipstart)
{
	if (argc < 2) {
		return -EINVAL;
	}

	int socket_id = atoi(argv[0]);
	char *status = argv[1];

	/* strip leading whitespace */
	while (*status == ' ') {
		status++;
	}

	if (strcmp(status, "CONNECT OK") == 0) {
		modem_cmd_handler_set_error(data, 0);
	} else if (strcmp(status, "CONNECT FAIL") == 0) {
		modem_cmd_handler_set_error(data, -ECONNREFUSED);
	} else {
		return -ENOMSG; /* not our URC */
	}

	k_sem_give(&mdata.sem_sock_conn);
	return 0;
}

/* Response format: <socket_id>, "CLOSE OK" or "CLOSE FAIL" */
MODEM_CMD_DEFINE(on_cmd_cipclose)
{
	if (argc < 2) {
		return -EINVAL;
	}

	int socket_id = atoi(argv[0]);
	char *status = argv[1];

	/* strip leading whitespace */
	while (*status == ' ') {
		status++;
	}

	if (strcmp(status, "CLOSE OK") == 0) {
		modem_cmd_handler_set_error(data, 0);
	} else if (strcmp(status, "CLOSE FAIL") == 0) {
		modem_cmd_handler_set_error(data, -EIO);
	} else {
		return -ENOMSG; /* not our URC */
	}

	k_sem_give(&mdata.sem_response);
	LOG_DBG("Socket %d closed", socket_id);
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

	LOG_DBG("Socket %d: SEND OK", socket_id);
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

static int get_inx_form_fd(struct modem_socket_config *cfg, int sock_fd)
{
	int i;

	k_sem_take(&cfg->sem_lock, K_FOREVER);

	for (i = 0; i < cfg->sockets_len; i++) {
		if (cfg->sockets[i].sock_fd == sock_fd) {
			k_sem_give(&cfg->sem_lock);
			return i;
		}
	}

	k_sem_give(&cfg->sem_lock);

	return -EINVAL;
}

int modem_offload_socket(int family, int type, int proto)
{
	int ret;

	ret = modem_socket_get(&mdata.socket_config, family, type, proto);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	struct modem_socket *sock = modem_socket_from_fd(&mdata.socket_config, ret);

	int i = get_inx_form_fd(&mdata.socket_config, ret);

	if (i < 0) {
		LOG_ERR("Failed to get socket index from fd %d", ret);
		modem_socket_put(&mdata.socket_config, ret);
		errno = EINVAL;
		return -1;
	}

	struct sim800l_socket_data *sock_data = &mdata.socket_data[i];

	memset(sock_data, 0, sizeof(*sock_data));
	k_mutex_init(&sock_data->lock);
	sock->data = sock_data;

	errno = 0;
	LOG_INF("Created socket: %d", ret);
	return ret;
}

static int offload_close(void *obj)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	int ret;
	char buf[32];

	static const struct modem_cmd cmd[] = {
		MODEM_CMD("", on_cmd_cipclose, 2U, ","),
	};

	if (!sock) {
		errno = EBADF;
		return -1;
	}

	LOG_WRN("offload_close called on socket %d (modem ID: %d), is_connected=%d", sock->sock_fd,
		sock->id, sock->is_connected);

	/* If socket is connected, send AT+CIPCLOSE to close connection */
	if (sock->is_connected) {
		/* AT+CIPCLOSE=<n>,<socket_id>
		 * <n> 0 - Slow close, 1 - Quick close
		 */
		snprintf(buf, sizeof(buf), "AT+CIPCLOSE=%d", sock->id - MDM_BASE_SOCKET_NUM);
		ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, cmd, ARRAY_SIZE(cmd),
				     buf, &mdata.sem_response, MDM_CMD_TIMEOUT);
		if (ret < 0 || modem_cmd_handler_get_error(&mdata.cmd_handler_data) != 0) {
			LOG_WRN("Failed to close connection: %d", ret);
		}
		sock->is_connected = false;
	}

	/* Clear any buffered data */
	struct sim800l_socket_data *sock_data = sock->data;

	if (sock_data) {
		k_mutex_lock(&sock_data->lock, K_FOREVER);
		if (sock_data->rx_buf) {
			net_buf_unref(sock_data->rx_buf);
			sock_data->rx_buf = NULL;
		}
		sock_data->buffered = 0;
		k_mutex_unlock(&sock_data->lock);
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

	static const struct modem_cmd cmd[] = {
		MODEM_CMD("", on_cmd_cipstart, 2U, ","),
	};

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

	k_sem_reset(&mdata.sem_sock_conn);

	/* Send connect command */
	ret = modem_cmd_send(&mdata.ctx.iface, &mdata.ctx.cmd_handler, NULL, 0U, buf,
			     &mdata.sem_response, K_SECONDS(1));

	if (ret < 0) {
		LOG_ERR("Failed to connect: %d", ret);
		errno = -ret;
		return -1;
	}

	/* set command handlers */
	ret = modem_cmd_handler_update_cmds(&mdata.cmd_handler_data, cmd, ARRAY_SIZE(cmd), true);
	if (ret < 0) {
		LOG_ERR("Failed to set command handlers: %d", ret);
		errno = -ret;
		return -1;
	}

	/* Wait for CONNECT OK/CONNECT FAIL */
	ret = k_sem_take(&mdata.sem_sock_conn, MDM_CONN_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Socket connect timeout");
		errno = ETIMEDOUT;
		return -1;
	}

	ret = modem_cmd_handler_get_error(&mdata.cmd_handler_data);
	if (ret != 0) {
		LOG_ERR("Socket connect failed: %d", ret);
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
	k_sem_take(&mdata.cmd_handler_data.sem_tx_lock, K_FOREVER);
	/* '>' will give semaphore */
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

	/* Wait for '>' */
	ret = k_sem_take(&mdata.sem_tx_ready, K_SECONDS(2));
	if (ret < 0) {
		/* Didn't get the data prompt - Exit. */
		LOG_DBG("Timeout waiting for tx");
		errno = EIO;
		return -1;
	}

	/* Send the actual data */
	modem_cmd_send_data_nolock(&mdata.ctx.iface, buf, len);
	modem_cmd_send_data_nolock(&mdata.ctx.iface, &ctrlz, 1);

	/* Wait for 'SEND OK' or 'SEND FAIL' */
	k_sem_reset(&mdata.sem_response);
	ret = k_sem_take(&mdata.sem_response, MDM_CMD_TIMEOUT);

	/* Clean up */
	modem_cmd_handler_update_cmds(&mdata.cmd_handler_data, NULL, 0U, false);

	k_sem_give(&mdata.cmd_handler_data.sem_tx_lock);

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
	size_t total_read = 0;
	int ret;

	if (!sock || !buf || max_len == 0) {
		errno = EINVAL;
		return -1;
	}

	if (flags & ZSOCK_MSG_PEEK) {
		errno = ENOTSUP;
		return -1;
	}

	if (flags & ~(ZSOCK_MSG_DONTWAIT)) {
		errno = ENOTSUP;
		return -1;
	}

	if (!(flags & ZSOCK_MSG_DONTWAIT)) {
		modem_socket_wait_data(&mdata.socket_config, sock);
	}

	uint16_t available = modem_socket_next_packet_size(&mdata.socket_config, sock);

	if (available == 0U) {
		errno = EAGAIN;
		return -1;
	}

	size_t to_read = MIN((size_t)available, max_len);
	struct sim800l_socket_data *sock_data = sock->data;

	if (!sock_data) {
		LOG_ERR("Socket data not initialized for fd %d", sock->sock_fd);
		errno = EIO;
		return -1;
	}

	k_mutex_lock(&sock_data->lock, K_FOREVER);

	if (!sock_data->rx_buf || sock_data->buffered == 0) {
		k_mutex_unlock(&sock_data->lock);
		LOG_DBG("No buffered data for socket %d (modem ID: %d)", sock->sock_fd, sock->id);
		errno = EAGAIN;
		return -1;
	}

	LOG_DBG("Reading %zu bytes from socket %d (modem ID: %d)", to_read, sock->sock_fd,
		sock->id);

	size_t copied = net_buf_linearize(buf, to_read, sock_data->rx_buf, 0, to_read);

	LOG_HEXDUMP_DBG(buf, copied, "Received data:");

	if (copied < to_read) {
		k_mutex_unlock(&sock_data->lock);
		errno = EIO;
		return -1;
	}

	total_read = copied;
	net_buf_pull(sock_data->rx_buf, total_read);
	if (sock_data->buffered >= total_read) {
		sock_data->buffered -= total_read;
	} else {
		sock_data->buffered = 0;
	}

	if (sock_data->rx_buf->len == 0 && sock_data->rx_buf->frags == NULL) {
		net_buf_unref(sock_data->rx_buf);
		sock_data->rx_buf = NULL;
	}

	ret = modem_socket_packet_size_update(&mdata.socket_config, sock, -(int)total_read);
	if (ret < 0) {
		LOG_WRN("Failed to update packet size for socket %d: %d", sock->id, ret);
	}

	k_mutex_unlock(&sock_data->lock);

	if (src_addr && addrlen) {
		size_t copy_len = MIN((size_t)*addrlen, sizeof(sock->dst));

		memcpy(src_addr, &sock->dst, copy_len);
		*addrlen = (socklen_t)copy_len;
	}

	if (modem_socket_next_packet_size(&mdata.socket_config, sock) > 0) {
		/* More data pending */
		modem_socket_data_ready(&mdata.socket_config, sock);
	}

	LOG_DBG("Received %zu bytes on socket %d (modem ID: %d)", total_read, sock->sock_fd,
		sock->id);
	errno = 0;
	return (ssize_t)total_read;
}

static ssize_t offload_read(void *obj, void *buf, size_t max_len)
{
	/* Simply call recvfrom with NULL address parameters and no flags */
	return offload_recvfrom(obj, buf, max_len, 0, NULL, NULL);
}

static ssize_t offload_sendmsg(void *obj, const struct msghdr *msg, int flags)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	size_t total_len = 0;
	size_t sent = 0;
	int ret;

	if (!sock || !msg) {
		errno = EINVAL;
		return -1;
	}

	/* Calculate total message length */
	for (size_t i = 0; i < msg->msg_iovlen; i++) {
		total_len += msg->msg_iov[i].iov_len;
	}

	if (total_len == 0) {
		return 0;
	}

	/* If only one iov, send directly */
	if (msg->msg_iovlen == 1) {
		return offload_sendto(obj, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
				      msg->msg_name, msg->msg_namelen);
	}

	/* Multiple iovs - need to send each one */
	for (size_t i = 0; i < msg->msg_iovlen; i++) {
		if (msg->msg_iov[i].iov_len == 0) {
			continue;
		}

		ret = offload_sendto(obj, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, flags,
				     (i == 0) ? msg->msg_name : NULL,
				     (i == 0) ? msg->msg_namelen : 0);

		if (ret < 0) {
			if (sent > 0) {
				/* Some data was sent before error */
				return sent;
			}
			return ret;
		}

		sent += ret;

		/* If partial send, stop here */
		if ((size_t)ret < msg->msg_iov[i].iov_len) {
			return sent;
		}
	}

	return sent;
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

static int offload_ioctl(void *obj, unsigned int request, va_list args)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(request);
	ARG_UNUSED(args);

	errno = ENOTSUP;
	return 0;
}

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
			.ioctl = offload_ioctl,
		},
	.bind = NULL,
	.connect = offload_connect,
	.sendto = offload_sendto,
	.recvfrom = offload_recvfrom,
	.listen = NULL,
	.accept = NULL,
	.sendmsg = offload_sendmsg,
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
