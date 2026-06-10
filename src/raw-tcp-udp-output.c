#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define closesocket close
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("raw-tcp-udp-output", "en-US")

#define OUTPUT_NAME "Raw Video TCP/UDP Output"
#define LOG_TAG "[RawVideoOut]"

// 自定义简易视频帧头: [4字节长度][1字节帧类型][8字节DTS][Payload]
// 帧类型: 0 = 非关键帧, 1 = 关键帧(IDR)
#pragma pack(push, 1)
struct video_frame_header {
    uint32_t size;      // 网络字节序(大端)
    uint8_t  keyframe;  // 0 or 1
    uint64_t dts;       // 网络字节序(大端), 解码时间戳
};
#pragma pack(pop)

struct raw_output {
    obs_output_t *output;
    
    int socket_fd;
    struct sockaddr_in dest_addr;
    bool is_tcp;
    bool connected;
    
    pthread_mutex_t mutex;
    volatile bool active;
};

/* ========== 网络工具函数 ========== */

static bool parse_url(const char *url, bool *is_tcp, char *host, size_t host_len, uint16_t *port) {
    if (strncmp(url, "tcp://", 6) == 0) {
        *is_tcp = true; url += 6;
    } else if (strncmp(url, "udp://", 6) == 0) {
        *is_tcp = false; url += 6;
    } else {
        return false;
    }
    
    const char *colon = strrchr(url, ':');
    if (!colon) return false;
    
    size_t len = colon - url;
    if (len >= host_len) return false;
    
    strncpy(host, url, len);
    host[len] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return (*port > 0 && *port < 65536);
}

static bool connect_socket(struct raw_output *out) {
    out->socket_fd = socket(AF_INET, out->is_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (out->socket_fd == INVALID_SOCKET) {
        blog(LOG_ERROR, LOG_TAG " Failed to create socket");
        return false;
    }
    
    if (connect(out->socket_fd, (struct sockaddr *)&out->dest_addr, sizeof(out->dest_addr)) == SOCKET_ERROR) {
        blog(LOG_ERROR, LOG_TAG " Connect failed to %s:%d", 
             inet_ntoa(out->dest_addr.sin_addr), ntohs(out->dest_addr.sin_port));
        closesocket(out->socket_fd);
        out->socket_fd = INVALID_SOCKET;
        return false;
    }
    
    out->connected = true;
    blog(LOG_INFO, LOG_TAG " %s connected to %s:%d",
         out->is_tcp ? "TCP" : "UDP",
         inet_ntoa(out->dest_addr.sin_addr),
         ntohs(out->dest_addr.sin_port));
    return true;
}

static void send_data(struct raw_output *out, const uint8_t *data, size_t size) {
    if (!out->connected || out->socket_fd == INVALID_SOCKET) return;
    
    ssize_t sent = send(out->socket_fd, (const char *)data, size, 0);
    if (sent == SOCKET_ERROR) {
        blog(LOG_WARNING, LOG_TAG " Send error, marking disconnected");
        out->connected = false;
        closesocket(out->socket_fd);
        out->socket_fd = INVALID_SOCKET;
    }
}

/* ========== OBS Output 回调 ========== */

static const char *raw_output_get_name(void *unused) {
    UNUSED_PARAMETER(unused);
    return OUTPUT_NAME;
}

static void *raw_output_create(obs_data_t *settings, obs_output_t *output) {
    UNUSED_PARAMETER(settings);
    struct raw_output *out = bzalloc(sizeof(struct raw_output));
    out->output = output;
    out->socket_fd = INVALID_SOCKET;
    pthread_mutex_init(&out->mutex, NULL);
    return out;
}

static void raw_output_destroy(void *data) {
    struct raw_output *out = data;
    if (out->socket_fd != INVALID_SOCKET) closesocket(out->socket_fd);
    pthread_mutex_destroy(&out->mutex);
    bfree(out);
}

static bool raw_output_start(void *data) {
    struct raw_output *out = data;
    const char *url = obs_output_get_url(out->output);
    
    char host[256]; uint16_t port;
    if (!url || !parse_url(url, &out->is_tcp, host, sizeof(host), &port)) {
        blog(LOG_ERROR, LOG_TAG " Invalid URL: %s", url ? url : "(null)");
        return false;
    }
    
    memset(&out->dest_addr, 0, sizeof(out->dest_addr));
    out->dest_addr.sin_family = AF_INET;
    out->dest_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &out->dest_addr.sin_addr);
    
    if (!connect_socket(out)) return false;
    
    out->active = true;
    // ⚠️ 关键修改：仅捕获视频数据
    obs_output_begin_data_capture(out->output, OBS_OUTPUT_VIDEO);
    blog(LOG_INFO, LOG_TAG " Video-only output started: %s", url);
    return true;
}

static void raw_output_stop(void *data, uint64_t ts) {
    UNUSED_PARAMETER(ts);
    struct raw_output *out = data;
    out->active = false;
    obs_output_end_data_capture(out->output);
    
    pthread_mutex_lock(&out->mutex);
    if (out->socket_fd != INVALID_SOCKET) {
        closesocket(out->socket_fd);
        out->socket_fd = INVALID_SOCKET;
        out->connected = false;
    }
    pthread_mutex_unlock(&out->mutex);
    blog(LOG_INFO, LOG_TAG " Output stopped");
}

/**
 * 仅接收编码后的视频包
 */
static void raw_output_video_data(void *data, struct encoder_packet *packet) {
    struct raw_output *out = data;
    if (!out->active || !packet) return;
    
    // 构建自定义视频帧头
    struct video_frame_header hdr;
    hdr.size = htonl((uint32_t)packet->size);
    hdr.keyframe = packet->keyframe ? 1 : 0;
    hdr.dts = htonll((uint64_t)packet->dts);
    
    pthread_mutex_lock(&out->mutex);
    send_data(out, (const uint8_t *)&hdr, sizeof(hdr));
    send_data(out, packet->data, packet->size);
    pthread_mutex_unlock(&out->mutex);
}

static obs_properties_t *raw_output_properties(void *unused) {
    UNUSED_PARAMETER(unused);
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, "url", "Video Stream URL (tcp/udp://ip:port)", OBS_TEXT_DEFAULT);
    return props;
}

static void raw_output_defaults(obs_data_t *settings) {
    obs_data_set_default_string(settings, "url", "udp://127.0.0.1:9999");
}

/* ========== 模块注册 ========== */

struct obs_output_info raw_output_info = {
    .id = "raw_video_tcp_udp_output",
    // ⚠️ 关键修改：移除 OBS_OUTPUT_AV，仅保留 OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED
    .flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED,
    .get_name = raw_output_get_name,
    .create = raw_output_create,
    .destroy = raw_output_destroy,
    .start = raw_output_start,
    .stop = raw_output_stop,
    .encoded_packet = raw_output_video_data, // 仅处理视频编码包
    .get_properties = raw_output_properties,
    .get_defaults = raw_output_defaults,
};

bool obs_module_load(void) {
#ifdef _WIN32
    WSADATA wsaData; WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    obs_register_output(&raw_output_info);
    blog(LOG_INFO, LOG_TAG " Video-only module loaded");
    return true;
}

void obs_module_unload(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}