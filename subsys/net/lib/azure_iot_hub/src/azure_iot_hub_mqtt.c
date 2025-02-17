/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <string.h>
#if defined(CONFIG_POSIX_API)
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/poll.h>
#else
#include <zephyr/net/socket.h>
#endif /* CONFIG_POSIX_API */
#include <net/azure_iot_hub.h>
#include <net/azure_iot_hub_dps.h>

#include "azure_iot_hub_mqtt.h"

#if defined(CONFIG_AZURE_IOT_HUB_PROVISION_CERTIFICATES)
#include CONFIG_AZURE_IOT_HUB_CERTIFICATES_FILE
#endif /* CONFIG_AZURE_IOT_HUB_PROVISION_CERTIFICATES */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(azure_iot_hub_mqtt, CONFIG_AZURE_IOT_HUB_LOG_LEVEL);

/* Define a custom AZ_MQTT_STATIC macro that exposes internal variables when unit testing. */
#if defined(CONFIG_UNITY)
#define AZ_MQTT_STATIC
#else
#define AZ_MQTT_STATIC static
#endif

AZ_MQTT_STATIC struct mqtt_client mqtt_client;
static struct sockaddr_storage broker;
static char rx_buffer[CONFIG_AZURE_IOT_HUB_MQTT_RX_TX_BUFFER_LEN];
static char tx_buffer[CONFIG_AZURE_IOT_HUB_MQTT_RX_TX_BUFFER_LEN];
AZ_MQTT_STATIC char payload_buf[CONFIG_AZURE_IOT_HUB_MQTT_PAYLOAD_BUFFER_LEN];
AZ_MQTT_STATIC K_SEM_DEFINE(connection_poll_sem, 0, 1);
static struct mqtt_helper_cfg current_cfg;
AZ_MQTT_STATIC enum mqtt_state mqtt_state = MQTT_STATE_UNINIT;

static const char *state_name_get(enum mqtt_state state)
{
	switch (state) {
	case MQTT_STATE_UNINIT: return "MQTT_STATE_UNINIT";
	case MQTT_STATE_DISCONNECTED: return "MQTT_STATE_DISCONNECTED";
	case MQTT_STATE_CONNECTING: return "MQTT_STATE_CONNECTING";
	case MQTT_STATE_TRANSPORT_CONNECTED: return "MQTT_STATE_TRANSPORT_CONNECTED";
	case MQTT_STATE_CONNECTED: return "MQTT_STATE_CONNECTED";
	case MQTT_STATE_DISCONNECTING: return "MQTT_STATE_DISCONNECTING";
	default: return "MQTT_STATE_UNKNOWN";
	}
}

AZ_MQTT_STATIC enum mqtt_state mqtt_state_get(void)
{
	return mqtt_state;
}

AZ_MQTT_STATIC void mqtt_state_set(enum mqtt_state new_state)
{
	bool notify_error = false;

	if (mqtt_state_get() == new_state) {
		LOG_DBG("Skipping transition to the same state (%s)",
			state_name_get(mqtt_state_get()));
		return;
	}

	/* Check for legal state transitions. */
	switch (mqtt_state_get()) {
	case MQTT_STATE_UNINIT:
		if (new_state != MQTT_STATE_DISCONNECTED) {
			notify_error = true;
		}
		break;
	case MQTT_STATE_DISCONNECTED:
		if (new_state != MQTT_STATE_CONNECTING &&
		    new_state != MQTT_STATE_UNINIT) {
			notify_error = true;
		}
		break;
	case MQTT_STATE_CONNECTING:
		if (new_state != MQTT_STATE_CONNECTED &&
		    new_state != MQTT_STATE_DISCONNECTED) {
			notify_error = true;
		}
		break;
	case MQTT_STATE_TRANSPORT_CONNECTED:
		if (new_state != MQTT_STATE_CONNECTING &&
		    new_state != MQTT_STATE_DISCONNECTED) {
			notify_error = true;
		}
		break;
	case MQTT_STATE_CONNECTED:
		if (new_state != MQTT_STATE_DISCONNECTING &&
		    new_state != MQTT_STATE_DISCONNECTED) {
			notify_error = true;
		}
		break;
	case MQTT_STATE_DISCONNECTING:
		if (new_state != MQTT_STATE_DISCONNECTED) {
			notify_error = true;
		}
		break;
	default:
		LOG_ERR("New state is unknown: %d", new_state);
		notify_error = true;
		break;
	}

	if (notify_error) {
		LOG_ERR("Invalid state transition, %s --> %s",
			state_name_get(mqtt_state),
			state_name_get(new_state));

		__ASSERT(false, "Illegal state transition: %d --> %d", mqtt_state, new_state);
	}

	LOG_DBG("State transition: %s --> %s",
		state_name_get(mqtt_state),
		state_name_get(new_state));

	mqtt_state = new_state;
}

static bool mqtt_state_verify(enum mqtt_state state)
{
	return (mqtt_state_get() == state);
}

#if defined(CONFIG_AZURE_IOT_HUB_PROVISION_CERTIFICATES)
static int certificates_provision(void)
{
	static bool certs_added;
	int err;

	if (!IS_ENABLED(CONFIG_NET_SOCKETS_SOCKOPT_TLS) || certs_added) {
		return 0;
	}

	err = tls_credential_add(CONFIG_AZURE_IOT_HUB_SEC_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate,
				 sizeof(ca_certificate));
	if (err < 0) {
		LOG_ERR("Failed to register CA certificate: %d",
			err);
		return err;
	}

	err = tls_credential_add(CONFIG_AZURE_IOT_HUB_SEC_TAG,
				 TLS_CREDENTIAL_PRIVATE_KEY,
				 private_key,
				 sizeof(private_key));
	if (err < 0) {
		LOG_ERR("Failed to register private key: %d", err);
		return err;
	}

	err = tls_credential_add(CONFIG_AZURE_IOT_HUB_SEC_TAG,
				 TLS_CREDENTIAL_SERVER_CERTIFICATE,
				 device_certificate,
				 sizeof(device_certificate));
	if (err < 0) {
		LOG_ERR("Failed to register public certificate: %d",
			err);
		return err;
	}

	certs_added = true;

	return 0;
}
#endif /* CONFIG_AZURE_IOT_HUB_PROVISION_CERTIFICATES */

static int publish_get_payload(struct mqtt_client *const mqtt_client, size_t length)
{
	if (length > sizeof(payload_buf)) {
		LOG_ERR("Incoming MQTT message too large for payload buffer");
		return -EMSGSIZE;
	}

	return mqtt_readall_publish_payload(mqtt_client, payload_buf, length);
}

static void send_ack(struct mqtt_client *const mqtt_client, uint16_t message_id)
{
	int err;
	const struct mqtt_puback_param ack = {
		.message_id = message_id
	};

	err = mqtt_publish_qos1_ack(mqtt_client, &ack);
	if (err) {
		LOG_WRN("Failed to send MQTT ACK, error: %d", err);
		return;
	}

	LOG_DBG("PUBACK sent for message ID %d", message_id);
}

AZ_MQTT_STATIC void on_publish(const struct mqtt_evt *mqtt_evt)
{
	int err;
	const struct mqtt_publish_param *p = &mqtt_evt->param.publish;
	struct azure_iot_hub_buf topic = {
		.ptr = (char *)p->message.topic.topic.utf8,
		.size = p->message.topic.topic.size,
	};
	struct azure_iot_hub_buf payload = {
		.ptr = payload_buf,
	};

	err = publish_get_payload(&mqtt_client, p->message.payload.len);
	if (err) {
		LOG_ERR("publish_get_payload, error: %d", err);

		if (current_cfg.cb.on_error) {
			current_cfg.cb.on_error(MQTT_HELPER_ERROR_MSG_SIZE);
		}

		return;
	}

	if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		send_ack(&mqtt_client, p->message_id);
	}

	payload.size = p->message.payload.len;

	if (current_cfg.cb.on_publish) {
		current_cfg.cb.on_publish(topic, payload);
	}
}

AZ_MQTT_STATIC void mqtt_evt_handler(struct mqtt_client *const mqtt_client,
			     const struct mqtt_evt *mqtt_evt)
{
	switch (mqtt_evt->type) {
	case MQTT_EVT_CONNACK:
		LOG_DBG("MQTT mqtt_client connected");

		if (mqtt_evt->param.connack.return_code == MQTT_CONNECTION_ACCEPTED) {
			mqtt_state_set(MQTT_STATE_CONNECTED);
		} else {
			mqtt_state_set(MQTT_STATE_DISCONNECTED);
		}

		if (current_cfg.cb.on_connack) {
			current_cfg.cb.on_connack(mqtt_evt->param.connack.return_code);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_DBG("MQTT_EVT_DISCONNECT: result = %d", mqtt_evt->result);

		mqtt_state_set(MQTT_STATE_DISCONNECTED);

		if (current_cfg.cb.on_disconnect) {
			current_cfg.cb.on_disconnect(mqtt_evt->result);
		}
		break;
	case MQTT_EVT_PUBLISH:
		LOG_DBG("MQTT_EVT_PUBLISH, message ID: %d, len = %d",
			mqtt_evt->param.publish.message_id,
			mqtt_evt->param.publish.message.payload.len);
		on_publish(mqtt_evt);
		break;
	case MQTT_EVT_PUBACK:
		LOG_DBG("MQTT_EVT_PUBACK: id = %d result = %d",
			mqtt_evt->param.puback.message_id,
			mqtt_evt->result);

		if (current_cfg.cb.on_puback) {
			current_cfg.cb.on_puback(mqtt_evt->param.puback.message_id,
						 mqtt_evt->result);
		}
		break;
	case MQTT_EVT_SUBACK:
		LOG_DBG("MQTT_EVT_SUBACK: id = %d result = %d",
			mqtt_evt->param.suback.message_id,
			mqtt_evt->result);

		if (current_cfg.cb.on_suback) {
			current_cfg.cb.on_suback(mqtt_evt->param.suback.message_id,
						 mqtt_evt->result);
		}
		break;
	case MQTT_EVT_PINGRESP:
		LOG_DBG("MQTT_EVT_PINGRESP");

		if (current_cfg.cb.on_pingresp) {
			current_cfg.cb.on_pingresp();
		}
		break;
	default:
		break;
	}
}

static int broker_init(struct sockaddr_storage *broker,
		       struct mqtt_helper_conn_params *conn_params)
{
	int err;
	struct addrinfo *result;
	struct addrinfo *addr;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	if (sizeof(CONFIG_AZURE_IOT_HUB_STATIC_IP_ADDRESS) > 1) {
		conn_params->hostname.ptr = CONFIG_AZURE_IOT_HUB_STATIC_IP_ADDRESS;

		LOG_DBG("Using static IP address: %s", CONFIG_AZURE_IOT_HUB_STATIC_IP_ADDRESS);
	} else {
		LOG_DBG("Resolving IP address for %s", conn_params->hostname.ptr);
	}

	err = getaddrinfo(conn_params->hostname.ptr, NULL, &hints, &result);
	if (err) {
		LOG_ERR("getaddrinfo() failed, error %d", err);
		return -err;
	}

	addr = result;

	while (addr != NULL) {
		if (addr->ai_addrlen == sizeof(struct sockaddr_in)) {
			struct sockaddr_in *broker4 = ((struct sockaddr_in *)broker);
			char ipv4_addr[INET_ADDRSTRLEN];

			broker4->sin_addr.s_addr =
				((struct sockaddr_in *)addr->ai_addr)->sin_addr.s_addr;
			broker4->sin_family = AF_INET;
			broker4->sin_port = htons(CONFIG_AZURE_IOT_HUB_PORT);

			inet_ntop(AF_INET, &broker4->sin_addr.s_addr, ipv4_addr,
				  sizeof(ipv4_addr));
			LOG_DBG("IPv4 Address found %s", ipv4_addr);
			break;
		}

		LOG_DBG("ai_addrlen is %u, while it should be %u",
			(unsigned int)addr->ai_addrlen,
			(unsigned int)sizeof(struct sockaddr_in));

		addr = addr->ai_next;
		break;
	}

	freeaddrinfo(result);

	return err;
}

static int client_broker_init(struct mqtt_helper_conn_params *conn_params)
{
	int err;
	sec_tag_t sec_tag_list[] = {
		CONFIG_AZURE_IOT_HUB_SEC_TAG,
#if CONFIG_AZURE_IOT_HUB_SECONDARY_SEC_TAG > -1
		CONFIG_AZURE_IOT_HUB_SECONDARY_SEC_TAG,
#endif
	};
	struct mqtt_sec_config *tls_cfg = &(mqtt_client.transport).tls.config;
	struct mqtt_utf8 user_name = {
		.utf8 = conn_params->user_name.ptr,
		.size = conn_params->user_name.size,
	};

	mqtt_client_init(&mqtt_client);

	err = broker_init(&broker, conn_params);
	if (err) {
		return err;
	}

	mqtt_client.broker	        = &broker;
	mqtt_client.evt_cb	        = mqtt_evt_handler;
	mqtt_client.client_id.utf8      = conn_params->device_id.ptr;
	mqtt_client.client_id.size      = conn_params->device_id.size;
	mqtt_client.password	        = NULL;
	mqtt_client.protocol_version    = MQTT_VERSION_3_1_1;
	mqtt_client.rx_buf	        = rx_buffer;
	mqtt_client.rx_buf_size	        = sizeof(rx_buffer);
	mqtt_client.tx_buf	        = tx_buffer;
	mqtt_client.tx_buf_size	        = sizeof(tx_buffer);
	mqtt_client.transport.type      = MQTT_TRANSPORT_SECURE;
	mqtt_client.user_name	        = conn_params->user_name.size > 0 ? &user_name : NULL;

	tls_cfg->peer_verify	        = TLS_PEER_VERIFY_REQUIRED;
	tls_cfg->cipher_count	        = 0;
	tls_cfg->cipher_list	        = NULL; /* Use default */
	tls_cfg->sec_tag_count	        = ARRAY_SIZE(sec_tag_list);
	tls_cfg->sec_tag_list	        = sec_tag_list;
	tls_cfg->session_cache	        = TLS_SESSION_CACHE_DISABLED;
	tls_cfg->hostname	        = conn_params->hostname.ptr;
	tls_cfg->set_native_tls		= IS_ENABLED(CONFIG_AZURE_IOT_HUB_NATIVE_TLS);

#if defined(CONFIG_AZURE_IOT_HUB_PROVISION_CERTIFICATES)
	err = certificates_provision();
	if (err) {
		LOG_ERR("Could not provision certificates, error: %d", err);
		return err;
	}
#endif /* !defined(CONFIG_NRF_MODEM_LIB) */

	return 0;
}

static int client_connect(struct mqtt_helper_conn_params *conn_params)
{
	int err;

	err = mqtt_connect(&mqtt_client);
	if (err) {
		LOG_ERR("mqtt_connect, error: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_AZURE_IOT_HUB_SEND_TIMEOUT)) {
		struct timeval timeout = {
			.tv_sec = CONFIG_AZURE_IOT_HUB_SEND_TIMEOUT_SEC
		};

		err = setsockopt(mqtt_client.transport.tls.sock,
				 SOL_SOCKET,
				 SO_SNDTIMEO,
				 &timeout,
				 sizeof(timeout));
		if (err == -1) {
			LOG_WRN("Failed to set timeout, errno: %d", errno);

			/* Don't propagate this as an error. */
			err = 0;
		} else {
			LOG_DBG("Using send socket timeout of %d seconds",
				CONFIG_AZURE_IOT_HUB_SEND_TIMEOUT_SEC);
		}
	}

	return 0;
}

/* Public API */

int mqtt_helper_init(struct mqtt_helper_cfg *cfg)
{
	__ASSERT_NO_MSG(cfg != NULL);

	if (!mqtt_state_verify(MQTT_STATE_UNINIT) && !mqtt_state_verify(MQTT_STATE_DISCONNECTED)) {
		LOG_ERR("Library is in the wrong state (%s), %s required",
			state_name_get(mqtt_state_get()),
			state_name_get(MQTT_STATE_UNINIT));

		return -EOPNOTSUPP;
	}

	current_cfg = *cfg;

	mqtt_state_set(MQTT_STATE_DISCONNECTED);

	return 0;
}

int mqtt_helper_connect(struct mqtt_helper_conn_params *conn_params)
{
	int err;

	__ASSERT_NO_MSG(conn_params != NULL);

	if (!mqtt_state_verify(MQTT_STATE_DISCONNECTED)) {
		LOG_ERR("Library is in the wrong state (%s), %s required",
			state_name_get(mqtt_state_get()),
			state_name_get(MQTT_STATE_DISCONNECTED));

		return -EOPNOTSUPP;
	}

	err = client_broker_init(conn_params);
	if (err) {
		LOG_ERR("client_broker_init failed, error: %d", err);
		return err;
	}

	mqtt_state_set(MQTT_STATE_CONNECTING);

	err = client_connect(conn_params);
	if (err) {
		mqtt_state_set(MQTT_STATE_DISCONNECTED);
		return err;
	}

	LOG_DBG("MQTT connection request sent");

	k_sem_give(&connection_poll_sem);

	return 0;
}

int mqtt_helper_disconnect(void)
{
	int err;

	if (!mqtt_state_verify(MQTT_STATE_CONNECTED)) {
		LOG_ERR("Library is in the wrong state (%s), %s required",
			state_name_get(mqtt_state_get()),
			state_name_get(MQTT_STATE_CONNECTED));

		return -EOPNOTSUPP;
	}

	mqtt_state_set(MQTT_STATE_DISCONNECTING);

	err = mqtt_disconnect(&mqtt_client);
	if (err) {
		/* Treat the sitation as an ungraceful disconnect */
		LOG_ERR("Failed to send disconnection request, treating as disconnected");
		mqtt_state_set(MQTT_STATE_DISCONNECTED);

		if (current_cfg.cb.on_disconnect) {
			current_cfg.cb.on_disconnect(err);
		}
	}

	return err;
}

int mqtt_helper_subscribe(struct mqtt_subscription_list *sub_list)
{
	int err;

	__ASSERT_NO_MSG(sub_list != NULL);

	if (!mqtt_state_verify(MQTT_STATE_CONNECTED)) {
		LOG_ERR("Library is in the wrong state (%s), %s required",
			state_name_get(mqtt_state_get()),
			state_name_get(MQTT_STATE_CONNECTED));

		return -EOPNOTSUPP;
	}

	for (size_t i = 0; i < sub_list->list_count; i++) {
		LOG_DBG("Subscribing to: %s", (char *)sub_list->list[i].topic.utf8);
	}

	err = mqtt_subscribe(&mqtt_client, sub_list);
	if (err) {
		return err;
	}

	return 0;
}

int mqtt_helper_publish(const struct mqtt_publish_param *param)
{
	LOG_DBG("Publishing to topic: %.*s",
		param->message.topic.topic.size,
		(char *)param->message.topic.topic.utf8);

	if (!mqtt_state_verify(MQTT_STATE_CONNECTED)) {
		LOG_ERR("Library is in the wrong state (%s), %s required",
			state_name_get(mqtt_state_get()),
			state_name_get(MQTT_STATE_CONNECTED));

		return -EOPNOTSUPP;
	}

	return mqtt_publish(&mqtt_client, param);
}

int mqtt_helper_deinit(void)
{
	if (!mqtt_state_verify(MQTT_STATE_DISCONNECTED)) {
		LOG_ERR("Library is in the wrong state (%s), %s required",
			state_name_get(mqtt_state_get()),
			state_name_get(MQTT_STATE_DISCONNECTED));

		return -EOPNOTSUPP;
	}

	memset(&current_cfg, 0, sizeof(current_cfg));
	memset(&mqtt_client, 0, sizeof(mqtt_client));

	mqtt_state_set(MQTT_STATE_UNINIT);

	return 0;
}

AZ_MQTT_STATIC void mqtt_helper_poll_loop(void)
{
	int ret;
	struct pollfd fds[1];

	LOG_DBG("Waiting for connection_poll_sem");
	k_sem_take(&connection_poll_sem, K_FOREVER);
	LOG_DBG("Took connection_poll_sem");

	fds[0].events = POLLIN;
	fds[0].fd = mqtt_client.transport.tls.sock;

	LOG_DBG("Starting to poll on socket, fd: %d", fds[0].fd);

	while (true) {
		if (mqtt_state_verify(MQTT_STATE_DISCONNECTING)) {
			LOG_DBG("Disconnect has been requested, ending poll loop");
			break;
		}

		ret = poll(fds, ARRAY_SIZE(fds), mqtt_keepalive_time_left(&mqtt_client));
		if (ret < 0) {
			LOG_ERR("poll() returned an error (%d), errno: %d", ret, -errno);
			break;
		}

		/* If poll returns 0, the timeout has expired. */
		if (ret == 0) {
			ret = mqtt_live(&mqtt_client);
			/* -EAGAIN indicates it is not time to ping; try later;
			 * otherwise, connection was closed due to NAT timeout.
			 */
			if (ret && (ret != -EAGAIN)) {
				LOG_ERR("Cloud MQTT keepalive ping failed: %d", ret);
				break;
			}
			continue;
		}

		if ((fds[0].revents & POLLIN) == POLLIN) {
			ret = mqtt_input(&mqtt_client);
			if (ret) {
				LOG_ERR("Cloud MQTT input error: %d", ret);
				if (ret == -ENOTCONN) {
					break;
				}
			}

			/* If connection state is set to STATE_DISCONNECTED at
			 * this point we know that the socket has
			 * been closed and we can break out of poll.
			 */
			if (mqtt_state_verify(MQTT_STATE_DISCONNECTED) ||
			    mqtt_state_verify(MQTT_STATE_UNINIT)) {
				LOG_DBG("The socket is already closed");
				break;
			}
		}

		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			if (mqtt_state_verify(MQTT_STATE_DISCONNECTING)) {
				/* POLLNVAL is to be expected while
				 * disconnecting, as the socket will be closed
				 * by the MQTT library and become invalid.
				 */
				LOG_DBG("POLLNVAL while disconnecting");
			} else if (mqtt_state_verify(MQTT_STATE_DISCONNECTED)) {
				LOG_DBG("POLLNVAL, no active connection");
			} else {
				LOG_ERR("Socket error: POLLNVAL");
				LOG_ERR("The socket was unexpectedly closed");
			}

			break;
		}

		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			LOG_ERR("Socket error: POLLHUP");
			LOG_ERR("Connection was unexpectedly closed");
			break;
		}

		if ((fds[0].revents & POLLERR) == POLLERR) {
			LOG_ERR("Socket error: POLLERR");
			LOG_ERR("Connection was unexpectedly closed");
			break;
		}
	}

	/* Always revert to the initialization state if the socket has been closed. */
	mqtt_state_set(MQTT_STATE_DISCONNECTED);
}

static void mqtt_helper_run(void)
{
	while (true) {
		mqtt_helper_poll_loop();
	}
}

K_THREAD_DEFINE(azure_iot_hub_mqtt_thread, CONFIG_AZURE_IOT_HUB_STACK_SIZE,
		mqtt_helper_run, false, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
