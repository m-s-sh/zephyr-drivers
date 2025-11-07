// ...existing code...

/* Socket state tracking */
struct sim800l_socket_data {
	int id;
	int type;
	int proto;
	bool connected;
	bool in_use;
	struct sockaddr remote_addr;
	uint16_t local_port;
	uint16_t remote_port;
	struct k_sem rx_sem;
	struct k_sem tx_sem;
	uint8_t rx_buf[1024];
	size_t rx_len;
	size_t rx_offset;
};

/* Add to sim800l_data structure */
struct sim800l_socket_data socket_data[MDM_MAX_SOCKETS];

/* Socket response handlers */
MODEM_CMD_DEFINE(on_cmd_cipstart)
{
	int link_id, state;

	/* Response format: <link_id>, "CONNECT OK" or "CONNECT FAIL" */
	link_id = atoi(argv[0]);

	if (link_id < 0 || link_id >= MDM_MAX_SOCKETS) {
		LOG_ERR("Invalid link_id: %d", link_id);
		return -EINVAL;
	}

	if (argc > 1 && strcmp(argv[1], "CONNECT OK") == 0) {
		sim800l_data_0.socket_data[link_id].connected = true;
		LOG_DBG("Socket %d connected", link_id);
	} else {
		sim800l_data_0.socket_data[link_id].connected = false;
		LOG_ERR("Socket %d connection failed", link_id);
		modem_cmd_handler_set_error(data, -ECONNREFUSED);
	}

	k_sem_give(&sim800l_data_0.socket_data[link_id].tx_sem);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_cipsend)
{
	/* Response: SEND OK or SEND FAIL */
	if (argc > 0 && strcmp(argv[0], "SEND OK") == 0) {
		modem_cmd_handler_set_error(data, 0);
	} else {
		modem_cmd_handler_set_error(data, -EIO);
	}
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_cipclose)
{
	int link_id;

	/* Response format: <link_id>, "CLOSE OK" */
	link_id = atoi(argv[0]);

	if (link_id < 0 || link_id >= MDM_MAX_SOCKETS) {
		LOG_ERR("Invalid link_id: %d", link_id);
		return -EINVAL;
	}

	sim800l_data_0.socket_data[link_id].connected = false;
	sim800l_data_0.socket_data[link_id].in_use = false;

	LOG_DBG("Socket %d closed", link_id);
	return 0;
}

/* URC handlers for socket events */
MODEM_CMD_DEFINE(on_urc_receive)
{
	int link_id, len;

	/* +RECEIVE: <link_id>,<length> */
	link_id = atoi(argv[0]);
	len = atoi(argv[1]);

	if (link_id < 0 || link_id >= MDM_MAX_SOCKETS) {
		LOG_ERR("Invalid link_id: %d", link_id);
		return -EINVAL;
	}

	LOG_DBG("Data available on socket %d: %d bytes", link_id, len);
	k_sem_give(&sim800l_data_0.socket_data[link_id].rx_sem);

	return 0;
}

MODEM_CMD_DEFINE(on_urc_closed)
{
	int link_id;

	/* <link_id>, "CLOSED" */
	link_id = atoi(argv[0]);

	if (link_id < 0 || link_id >= MDM_MAX_SOCKETS) {
		LOG_ERR("Invalid link_id: %d", link_id);
		return -EINVAL;
	}

	LOG_WRN("Socket %d closed by remote", link_id);
	sim800l_data_0.socket_data[link_id].connected = false;

	return 0;
}

/* Socket operation implementations */
static ssize_t offload_read(void *obj, void *buffer, size_t count)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	struct sim800l_socket_data *sock_data;
	ssize_t ret;

	if (!sock || sock->id < MDM_BASE_SOCKET_NUM ||
	    sock->id >= MDM_BASE_SOCKET_NUM + MDM_MAX_SOCKETS) {
		errno = EBADF;
		return -1;
	}

	sock_data = &sim800l_data_0.socket_data[sock->id - MDM_BASE_SOCKET_NUM];

	if (!sock_data->connected) {
		errno = ENOTCONN;
		return -1;
	}

	/* If no data buffered, wait for data */
	if (sock_data->rx_len == 0 || sock_data->rx_offset >= sock_data->rx_len) {
		ret = k_sem_take(&sock_data->rx_sem, K_SECONDS(30));
		if (ret < 0) {
			errno = ETIMEDOUT;
			return -1;
		}

		/* Read data using AT+CIPRXGET */
		char cmd[32];
		snprintk(cmd, sizeof(cmd), "AT+CIPRXGET=2,%d,%zu", sock->id - MDM_BASE_SOCKET_NUM,
			 count);

		/* TODO: Implement actual data reading */
		sock_data->rx_len = 0;
		sock_data->rx_offset = 0;
	}

	/* Copy buffered data */
	size_t to_copy = MIN(count, sock_data->rx_len - sock_data->rx_offset);
	memcpy(buffer, sock_data->rx_buf + sock_data->rx_offset, to_copy);
	sock_data->rx_offset += to_copy;

	errno = 0;
	return to_copy;
}

static ssize_t offload_write(void *obj, const void *buffer, size_t count)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	struct sim800l_socket_data *sock_data;
	char cmd[32];
	int ret;

	if (!sock || sock->id < MDM_BASE_SOCKET_NUM ||
	    sock->id >= MDM_BASE_SOCKET_NUM + MDM_MAX_SOCKETS) {
		errno = EBADF;
		return -1;
	}

	sock_data = &sim800l_data_0.socket_data[sock->id - MDM_BASE_SOCKET_NUM];

	if (!sock_data->connected) {
		errno = ENOTCONN;
		return -1;
	}

	/* AT+CIPSEND=<link_id>,<length> */
	snprintk(cmd, sizeof(cmd), "AT+CIPSEND=%d,%zu", sock->id - MDM_BASE_SOCKET_NUM, count);

	ret = modem_cmd_send(&sim800l_data_0.ctx.iface, &sim800l_data_0.ctx.cmd_handler, NULL, 0U,
			     cmd, &sim800l_data_0.sem_tx_ready, K_SECONDS(5));
	if (ret < 0) {
		errno = EIO;
		return -1;
	}

	/* Send actual data */
	ret = modem_iface_uart_write(&sim800l_data_0.ctx.iface, buffer, count);
	if (ret < 0) {
		errno = EIO;
		return -1;
	}

	/* Wait for SEND OK */
	k_sleep(K_MSEC(100));

	errno = 0;
	return count;
}

static int offload_close(void *obj)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	struct sim800l_socket_data *sock_data;
	char cmd[32];
	int ret;

	if (!sock || sock->id < MDM_BASE_SOCKET_NUM ||
	    sock->id >= MDM_BASE_SOCKET_NUM + MDM_MAX_SOCKETS) {
		errno = EBADF;
		return -1;
	}

	sock_data = &sim800l_data_0.socket_data[sock->id - MDM_BASE_SOCKET_NUM];

	/* AT+CIPCLOSE=<link_id> */
	snprintk(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", sock->id - MDM_BASE_SOCKET_NUM);

	ret = modem_cmd_send(&sim800l_data_0.ctx.iface, &sim800l_data_0.ctx.cmd_handler, NULL, 0U,
			     cmd, &sim800l_data_0.sem_response, K_SECONDS(5));

	sock_data->connected = false;
	sock_data->in_use = false;

	modem_socket_put(&sim800l_data_0.socket_config, sock->id);

	errno = (ret < 0) ? EIO : 0;
	return (ret < 0) ? -1 : 0;
}

static int offload_connect(void *obj, const struct sockaddr *addr, socklen_t addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	struct sim800l_socket_data *sock_data;
	char cmd[128];
	char ip_str[INET_ADDRSTRLEN];
	uint16_t port;
	const char *proto_str;
	int ret;

	if (!sock || sock->id < MDM_BASE_SOCKET_NUM ||
	    sock->id >= MDM_BASE_SOCKET_NUM + MDM_MAX_SOCKETS) {
		errno = EBADF;
		return -1;
	}

	sock_data = &sim800l_data_0.socket_data[sock->id - MDM_BASE_SOCKET_NUM];

	if (addr->sa_family != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
	inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
	port = ntohs(addr_in->sin_port);

	/* Determine protocol */
	proto_str = (sock_data->type == SOCK_STREAM) ? "TCP" : "UDP";

	/* AT+CIPSTART=<link_id>,"<protocol>","<IP>",<port> */
	snprintk(cmd, sizeof(cmd), "AT+CIPSTART=%d,\"%s\",\"%s\",%u",
		 sock->id - MDM_BASE_SOCKET_NUM, proto_str, ip_str, port);

	LOG_DBG("Connecting socket %d to %s:%u via %s", sock->id - MDM_BASE_SOCKET_NUM, ip_str,
		port, proto_str);

	ret = modem_cmd_send(&sim800l_data_0.ctx.iface, &sim800l_data_0.ctx.cmd_handler, NULL, 0U,
			     cmd, &sock_data->tx_sem, K_SECONDS(30));
	if (ret < 0 || !sock_data->connected) {
		errno = ECONNREFUSED;
		return -1;
	}

	memcpy(&sock_data->remote_addr, addr, addrlen);
	sock_data->remote_port = port;

	errno = 0;
	return 0;
}

static ssize_t offload_sendto(void *obj, const void *buf, size_t len, int flags,
			      const struct sockaddr *dest_addr, socklen_t addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	struct sim800l_socket_data *sock_data;

	if (!sock || sock->id < MDM_BASE_SOCKET_NUM ||
	    sock->id >= MDM_BASE_SOCKET_NUM + MDM_MAX_SOCKETS) {
		errno = EBADF;
		return -1;
	}

	sock_data = &sim800l_data_0.socket_data[sock->id - MDM_BASE_SOCKET_NUM];

	/* For UDP, connect if not already connected */
	if (sock_data->type == SOCK_DGRAM && !sock_data->connected && dest_addr) {
		if (offload_connect(obj, dest_addr, addrlen) < 0) {
			return -1;
		}
	}

	return offload_write(obj, buf, len);
}

static ssize_t offload_recvfrom(void *obj, void *buf, size_t max_len, int flags,
				struct sockaddr *src_addr, socklen_t *addrlen)
{
	struct modem_socket *sock = (struct modem_socket *)obj;
	struct sim800l_socket_data *sock_data;
	ssize_t ret;

	if (!sock || sock->id < MDM_BASE_SOCKET_NUM ||
	    sock->id >= MDM_BASE_SOCKET_NUM + MDM_MAX_SOCKETS) {
		errno = EBADF;
		return -1;
	}

	sock_data = &sim800l_data_0.socket_data[sock->id - MDM_BASE_SOCKET_NUM];

	ret = offload_read(obj, buf, max_len);

	if (ret > 0 && src_addr && addrlen) {
		memcpy(src_addr, &sock_data->remote_addr,
		       MIN(*addrlen, sizeof(sock_data->remote_addr)));
		*addrlen = sizeof(sock_data->remote_addr);
	}

	return ret;
}

// ...existing code...

static int offload_socket(int family, int type, int proto)
{
	int ret;
	int sock_id;

	ret = modem_socket_get(&sim800l_data_0.socket_config, family, type, proto);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	sock_id = ret - MDM_BASE_SOCKET_NUM;

	/* Initialize socket data */
	sim800l_data_0.socket_data[sock_id].id = ret;
	sim800l_data_0.socket_data[sock_id].type = type;
	sim800l_data_0.socket_data[sock_id].proto = proto;
	sim800l_data_0.socket_data[sock_id].connected = false;
	sim800l_data_0.socket_data[sock_id].in_use = true;
	sim800l_data_0.socket_data[sock_id].rx_len = 0;
	sim800l_data_0.socket_data[sock_id].rx_offset = 0;

	k_sem_init(&sim800l_data_0.socket_data[sock_id].rx_sem, 0, 1);
	k_sem_init(&sim800l_data_0.socket_data[sock_id].tx_sem, 0, 1);

	errno = 0;
	return ret;
}

static int modem_init(const struct device *dev)
{
	// ...existing initialization code...

	/* Initialize socket data structures */
	for (int i = 0; i < MDM_MAX_SOCKETS; i++) {
		sim800l_data_0.socket_data[i].in_use = false;
		sim800l_data_0.socket_data[i].connected = false;
	}

	// ...rest of initialization...
}
