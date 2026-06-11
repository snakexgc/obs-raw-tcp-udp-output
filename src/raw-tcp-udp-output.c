/*
 * Raw video TCP/UDP output for OBS Studio.
 *
 * Sends encoded video packets to a remote host as a byte stream of
 * [17-byte header][payload] records. Header layout (all integers
 * big-endian):
 *
 *   offset 0,  4 bytes: magic word 0x52415756 ("RAWV")
 *   offset 4,  4 bytes: payload size
 *   offset 8,  1 byte : frame type (0 = non-keyframe, 1 = keyframe/IDR)
 *   offset 9,  8 bytes: DTS
 *
 * TCP: records are written back-to-back on the stream.
 * UDP: the same byte stream is split into datagrams of at most
 *      UDP_CHUNK_SIZE bytes. Datagrams carry no sequence numbers, so the
 *      receiver must reassemble them in arrival order; this is only
 *      reliable on networks where reordering/loss is negligible (e.g.
 *      localhost or a LAN).
 */

/* winsock2.h must precede any header that may pull in windows.h, which
 * would otherwise include the conflicting winsock 1.1 declarations */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#endif

#include <obs-module.h>
#include <util/threading.h>
#include <util/bmem.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("raw-tcp-udp-output", "en-US")

#define OUTPUT_NAME "Raw Video TCP/UDP Output"
#define LOG_TAG "[raw-tcp-udp-output]"

#define FRAME_MAGIC 0x52415756u /* "RAWV" */
#define VIDEO_HEADER_SIZE 17

/* Largest single UDP datagram we emit. Keeps datagrams under a typical
 * 1500-byte MTU and far below the 65507-byte UDP payload limit that
 * encoded keyframes would otherwise exceed. */
#define UDP_CHUNK_SIZE 1400

struct raw_output {
	obs_output_t *output;

	socket_t sock;
	bool is_tcp;
	bool connected;

	/* scratch buffer so header+payload go out as one byte stream */
	uint8_t *buf;
	size_t buf_capacity;

	uint64_t total_bytes;

	pthread_mutex_t mutex;
	volatile bool active;
};

/* ========== helpers ========== */

static void write_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void write_be64(uint8_t *p, uint64_t v)
{
	write_be32(p, (uint32_t)(v >> 32));
	write_be32(p + 4, (uint32_t)v);
}

static bool parse_url(const char *url, bool *is_tcp, char *host,
		      size_t host_size, char *port, size_t port_size)
{
	if (strncmp(url, "tcp://", 6) == 0) {
		*is_tcp = true;
	} else if (strncmp(url, "udp://", 6) == 0) {
		*is_tcp = false;
	} else {
		return false;
	}
	url += 6;

	const char *host_start = url;
	const char *host_end;
	const char *port_start;

	if (*url == '[') { /* bracketed IPv6 literal: [::1]:9999 */
		host_start = url + 1;
		host_end = strchr(host_start, ']');
		if (!host_end || host_end[1] != ':')
			return false;
		port_start = host_end + 2;
	} else {
		host_end = strrchr(url, ':');
		if (!host_end)
			return false;
		port_start = host_end + 1;
	}

	size_t host_len = (size_t)(host_end - host_start);
	if (host_len == 0 || host_len >= host_size)
		return false;

	char *end = NULL;
	long port_num = strtol(port_start, &end, 10);
	if (port_num < 1 || port_num > 65535 || !end || *end != '\0')
		return false;

	memcpy(host, host_start, host_len);
	host[host_len] = '\0';
	snprintf(port, port_size, "%ld", port_num);
	return true;
}

static bool connect_socket(struct raw_output *out, const char *host,
			   const char *port)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = out->is_tcp ? SOCK_STREAM : SOCK_DGRAM;

	int err = getaddrinfo(host, port, &hints, &res);
	if (err != 0 || !res) {
		blog(LOG_ERROR, LOG_TAG " Failed to resolve %s:%s (%d)", host,
		     port, err);
		return false;
	}

	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		socket_t fd = socket(ai->ai_family, ai->ai_socktype,
				     ai->ai_protocol);
		if (fd == INVALID_SOCKET)
			continue;

#ifdef SO_NOSIGPIPE
		int on = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&on,
			   sizeof(on));
#endif

		if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) {
			out->sock = fd;
			break;
		}
		closesocket(fd);
	}
	freeaddrinfo(res);

	if (out->sock == INVALID_SOCKET) {
		blog(LOG_ERROR, LOG_TAG " Could not connect to %s:%s", host,
		     port);
		return false;
	}

	out->connected = true;
	blog(LOG_INFO, LOG_TAG " %s connected to %s:%s",
	     out->is_tcp ? "TCP" : "UDP", host, port);
	return true;
}

/* Sends the whole buffer, handling partial TCP sends and splitting the
 * stream into MTU-sized datagrams for UDP. Caller holds the mutex. */
static bool send_all(struct raw_output *out, const uint8_t *data, size_t size)
{
	while (size > 0) {
		size_t chunk = size;
		if (!out->is_tcp && chunk > UDP_CHUNK_SIZE)
			chunk = UDP_CHUNK_SIZE;

#ifdef _WIN32
		int ret = send(out->sock, (const char *)data, (int)chunk, 0);
		if (ret == SOCKET_ERROR)
			return false;
#else
		ssize_t ret = send(out->sock, data, chunk, MSG_NOSIGNAL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
#endif
		data += ret;
		size -= (size_t)ret;
		out->total_bytes += (uint64_t)ret;
	}
	return true;
}

/* ========== OBS output callbacks ========== */

static const char *raw_output_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return OUTPUT_NAME;
}

static void *raw_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct raw_output *out = bzalloc(sizeof(struct raw_output));
	out->output = output;
	out->sock = INVALID_SOCKET;
	pthread_mutex_init(&out->mutex, NULL);
	return out;
}

static void raw_output_destroy(void *data)
{
	struct raw_output *out = data;
	if (out->sock != INVALID_SOCKET)
		closesocket(out->sock);
	pthread_mutex_destroy(&out->mutex);
	bfree(out->buf);
	bfree(out);
}

static bool raw_output_start(void *data)
{
	struct raw_output *out = data;

	if (!obs_output_can_begin_data_capture(out->output, 0))
		return false;
	if (!obs_output_initialize_encoders(out->output, 0))
		return false;

	char host[256], port[8];
	bool valid;

	obs_data_t *settings = obs_output_get_settings(out->output);
	const char *url = obs_data_get_string(settings, "url");
	valid = url && parse_url(url, &out->is_tcp, host, sizeof(host), port,
				 sizeof(port));
	if (!valid)
		blog(LOG_ERROR, LOG_TAG " Invalid URL: %s",
		     url ? url : "(null)");
	obs_data_release(settings);

	if (!valid) {
		obs_output_set_last_error(
			out->output,
			"Invalid URL, expected tcp://host:port or udp://host:port");
		return false;
	}

	if (!connect_socket(out, host, port)) {
		obs_output_set_last_error(out->output,
					  "Could not connect to destination");
		return false;
	}

	out->total_bytes = 0;
	out->active = true;
	obs_output_begin_data_capture(out->output, 0);
	blog(LOG_INFO, LOG_TAG " Video output started");
	return true;
}

static void raw_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct raw_output *out = data;

	out->active = false;
	obs_output_end_data_capture(out->output);

	pthread_mutex_lock(&out->mutex);
	if (out->sock != INVALID_SOCKET) {
		closesocket(out->sock);
		out->sock = INVALID_SOCKET;
	}
	out->connected = false;
	pthread_mutex_unlock(&out->mutex);

	blog(LOG_INFO, LOG_TAG " Output stopped");
}

static void raw_output_packet(void *data, struct encoder_packet *packet)
{
	struct raw_output *out = data;
	bool failed = false;

	if (!out->active || !packet || packet->type != OBS_ENCODER_VIDEO)
		return;

	size_t total = VIDEO_HEADER_SIZE + packet->size;

	pthread_mutex_lock(&out->mutex);

	if (!out->connected) {
		pthread_mutex_unlock(&out->mutex);
		return;
	}

	if (out->buf_capacity < total) {
		out->buf = brealloc(out->buf, total);
		out->buf_capacity = total;
	}

	write_be32(out->buf, FRAME_MAGIC);
	write_be32(out->buf + 4, (uint32_t)packet->size);
	out->buf[8] = packet->keyframe ? 1 : 0;
	write_be64(out->buf + 9, (uint64_t)packet->dts);
	memcpy(out->buf + VIDEO_HEADER_SIZE, packet->data, packet->size);

	if (!send_all(out, out->buf, total)) {
		blog(LOG_WARNING, LOG_TAG " Send failed, disconnecting");
		closesocket(out->sock);
		out->sock = INVALID_SOCKET;
		out->connected = false;
		out->active = false;
		failed = true;
	}

	pthread_mutex_unlock(&out->mutex);

	if (failed)
		obs_output_signal_stop(out->output, OBS_OUTPUT_DISCONNECTED);
}

static uint64_t raw_output_total_bytes(void *data)
{
	struct raw_output *out = data;
	return out->total_bytes;
}

static obs_properties_t *raw_output_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "url", obs_module_text("URL"),
				OBS_TEXT_DEFAULT);
	return props;
}

static void raw_output_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "udp://127.0.0.1:9999");
}

/* ========== module registration ========== */

struct obs_output_info raw_output_info = {
	.id = "raw_video_tcp_udp_output",
	.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED,
	.get_name = raw_output_get_name,
	.create = raw_output_create,
	.destroy = raw_output_destroy,
	.start = raw_output_start,
	.stop = raw_output_stop,
	.encoded_packet = raw_output_packet,
	.get_total_bytes = raw_output_total_bytes,
	.get_properties = raw_output_properties,
	.get_defaults = raw_output_defaults,
};

bool obs_module_load(void)
{
#ifdef _WIN32
	WSADATA wsa_data;
	int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
	if (err != 0) {
		blog(LOG_ERROR, LOG_TAG " WSAStartup failed (%d)", err);
		return false;
	}
#endif
	obs_register_output(&raw_output_info);
	blog(LOG_INFO, LOG_TAG " Module loaded");
	return true;
}

void obs_module_unload(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}

const char *obs_module_description(void)
{
	return "Sends encoded video packets to a remote host over raw TCP or UDP";
}
