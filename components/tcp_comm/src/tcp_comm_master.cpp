#include "sdkconfig.h"
#include "tcp_comm/tcp_comm.h"
#include "tcp_comm/tcp_protocol.h"
#include "espnow_comm/protocol.h"
#include "espnow_comm/peer_mgr.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char* TAG = "tcp_comm";

// ── Master globals ──
static int              s_server_fd = -1;
static SemaphoreHandle_t s_comm_mutex = NULL;
static SemaphoreHandle_t s_resp_sem   = NULL;
static TaskHandle_t      s_server_task_handle = NULL;

static tcp_recv_callback_t s_recv_callback = NULL;

// Sync read state
static bool     s_resp_pending = false;
static uint8_t  s_resp_expected_module_id = 0;
static double   s_resp_values[DATA_RESP_MAX_VALUES];
static int      s_resp_value_count = 0;

// Client slot tracking
#define MAX_CLIENTS CONFIG_TCP_MAX_SENSORS
static int s_client_fds[MAX_CLIENTS];
static int s_client_count = 0;

#define COMM_LOCK()   xSemaphoreTake(s_comm_mutex, portMAX_DELAY)
#define COMM_UNLOCK() xSemaphoreGive(s_comm_mutex)

// ── Forward declarations ──
static void tcp_server_task(void* arg);

// ── Init ──
esp_err_t tcp_comm_init(void)
{
    s_comm_mutex = xSemaphoreCreateMutex();
    if (s_comm_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create comm mutex");
        return ESP_ERR_NO_MEM;
    }

    s_resp_sem = xSemaphoreCreateBinary();
    if (s_resp_sem == NULL) {
        ESP_LOGE(TAG, "failed to create response semaphore");
        vSemaphoreDelete(s_comm_mutex);
        s_comm_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Initialize client slots
    for (int i = 0; i < MAX_CLIENTS; i++) {
        s_client_fds[i] = -1;
    }
    s_client_count = 0;

    // Create socket
    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(CONFIG_TCP_PORT);

    if (bind(s_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(s_server_fd);
        return ESP_FAIL;
    }

    if (listen(s_server_fd, CONFIG_TCP_BACKLOG) < 0) {
        ESP_LOGE(TAG, "listen failed: %d", errno);
        close(s_server_fd);
        return ESP_FAIL;
    }

    BaseType_t task_ok = xTaskCreate(tcp_server_task, "tcp_srv",
                                      CONFIG_TCP_COMM_STACK_SIZE,
                                      NULL, 5, &s_server_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create server task");
        close(s_server_fd);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", CONFIG_TCP_PORT);
    return ESP_OK;
}

// ── Deinit ──
void tcp_comm_deinit(void)
{
    if (s_server_task_handle != NULL) {
        vTaskDelete(s_server_task_handle);
        s_server_task_handle = NULL;
    }
    if (s_server_fd >= 0) {
        close(s_server_fd);
        s_server_fd = -1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_client_fds[i] >= 0) close(s_client_fds[i]);
    }
    if (s_resp_sem != NULL) { vSemaphoreDelete(s_resp_sem); s_resp_sem = NULL; }
    if (s_comm_mutex != NULL) { vSemaphoreDelete(s_comm_mutex); s_comm_mutex = NULL; }
    s_recv_callback = NULL;
}

// ── Register callback ──
void tcp_comm_register_recv_callback(tcp_recv_callback_t cb) {
    s_recv_callback = cb;
}

// ── Main server task ──
static void tcp_server_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TCP server task started");

    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(s_server_fd, &read_fds);
        int max_fd = s_server_fd;

        COMM_LOCK();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_client_fds[i] >= 0) {
                FD_SET(s_client_fds[i], &read_fds);
                if (s_client_fds[i] > max_fd) max_fd = s_client_fds[i];
            }
        }
        COMM_UNLOCK();

        struct timeval tv;
        tv.tv_sec  = CONFIG_TCP_SELECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (CONFIG_TCP_SELECT_TIMEOUT_MS % 1000) * 1000;

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select error: %d", errno);
            break;
        }

        // Accept new connections
        if (FD_ISSET(s_server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(s_server_fd,
                                   (struct sockaddr*)&client_addr,
                                   &addr_len);
            if (client_fd >= 0) {
                // Set keepalive
                int ka = 1;
                setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
                int idle  = CONFIG_TCP_KEEPALIVE_IDLE_S;
                int intvl = CONFIG_TCP_KEEPALIVE_INTERVAL_S;
                int cnt   = CONFIG_TCP_KEEPALIVE_COUNT;
                setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
                setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

                // Disable Nagle
                int nodelay = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                // Non-blocking
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                // Store in slot
                bool stored = false;
                COMM_LOCK();
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (s_client_fds[i] < 0) {
                        s_client_fds[i] = client_fd;
                        s_client_count++;
                        stored = true;
                        break;
                    }
                }
                COMM_UNLOCK();

                if (!stored) {
                    ESP_LOGW(TAG, "max clients reached, rejecting");
                    close(client_fd);
                } else {
                    ESP_LOGI(TAG, "client connected fd=%d (%s:%d)",
                             client_fd,
                             inet_ntoa(client_addr.sin_addr),
                             ntohs(client_addr.sin_port));
                    // Send IDENTIFY request
                    uint8_t buf[250];
                    size_t len = 0;
                    protocol_build_identify(buf, &len, 0, "master",
                                             "ESP-LEGO Master");
                    uint8_t frame_buf[TCP_MAX_FRAME_SIZE];
                    uint8_t* frame = NULL;
                    int frame_len = tcp_protocol_frame(frame_buf,
                                                       sizeof(frame_buf),
                                                       buf, (int)len,
                                                       &frame);
                    if (frame_len > 0) {
                        write(client_fd, frame, (size_t)frame_len);
                    }
                }
            }
        }

        // Process client data
        COMM_LOCK();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = s_client_fds[i];
            if (fd < 0) continue;
            COMM_UNLOCK(); // release lock while processing (fd stays valid)

            if (FD_ISSET(fd, &read_fds)) {
                uint8_t frame_buf[TCP_MAX_FRAME_SIZE];
                int ret = tcp_protocol_read_frame(fd, frame_buf,
                                                   sizeof(frame_buf), 0);
                if (ret <= 0) {
                    // Disconnect
                    ESP_LOGI(TAG, "client fd=%d disconnected", fd);
                    close(fd);
                    COMM_LOCK();
                    // Find peer by fd and mark offline
                    uint8_t mid;
                    if (peer_mgr_find_by_fd(fd, &mid)) {
                        peer_mgr_set_fd(mid, -1);
                    }
                    s_client_fds[i] = -1;
                    s_client_count--;
                    COMM_UNLOCK();
                    continue;
                }

                // Parse frame: skip 2-byte prefix
                const uint8_t* msg = frame_buf + TCP_FRAME_PREFIX_SIZE;
                int msg_len = ret - TCP_FRAME_PREFIX_SIZE;

                MsgHeader hdr;
                if (!protocol_parse_header(msg, msg_len, &hdr)) {
                    ESP_LOGW(TAG, "invalid header from fd=%d", fd);
                    continue;
                }

                switch (hdr.msg_type) {
                case MSG_IDENTIFY_ACK: {
                    // Sensor identified itself — register peer
                    char ann_name[32];
                    char ann_cap[CONFIG_MAX_CAPABILITY_LEN];
                    protocol_parse_identify(msg, msg_len,
                                            ann_name, sizeof(ann_name),
                                            ann_cap, sizeof(ann_cap));
                    uint8_t module_id = peer_mgr_handle_announce(
                        NULL, ann_name, ann_cap);
                    // NULL MAC is fine — we use socket_fd for addressing
                    if (module_id > 0) {
                        peer_mgr_set_fd(module_id, fd);
                        ESP_LOGI(TAG, "sensor identified: id=%u name=%s fd=%d",
                                 module_id, ann_name, fd);
                    }
                    break;
                }
                case MSG_DATA_RESP: {
                    // Handle sync read response
                    COMM_LOCK();
                    if (s_resp_pending) {
                        // Find which module_id this fd belongs to
                        uint8_t mid;
                        if (peer_mgr_find_by_fd(fd, &mid) &&
                            mid == s_resp_expected_module_id) {
                            int n = protocol_extract_values(msg, msg_len,
                                s_resp_values, DATA_RESP_MAX_VALUES);
                            s_resp_value_count = (n > 0) ? n : 0;
                            xSemaphoreGive(s_resp_sem);
                        }
                    }
                    COMM_UNLOCK();
                    break;
                }
                case MSG_CMD:
                case MSG_DATA_REQ:
                    // Forward to app callback
                    if (s_recv_callback != NULL) {
                        s_recv_callback(NULL, hdr.msg_type, msg, msg_len);
                    }
                    break;
                default:
                    break;
                }
            }

            COMM_LOCK(); // re-lock for next iteration
        }
        COMM_UNLOCK();
    }

    // Task exit — close all
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_client_fds[i] >= 0) close(s_client_fds[i]);
    }
    close(s_server_fd);
    vTaskDelete(NULL);
}

// ── Sync request read ──
int tcp_comm_request_read(uint8_t module_id, double* out_values,
                           int max_values)
{
    COMM_LOCK();

    if (s_resp_pending) {
        COMM_UNLOCK();
        ESP_LOGW(TAG, "request_read(%u): concurrent request rejected", module_id);
        return 0;
    }

    int fd = peer_mgr_get_fd(module_id);
    if (fd < 0) {
        COMM_UNLOCK();
        ESP_LOGW(TAG, "request_read(%u): peer not connected", module_id);
        return 0;
    }

    s_resp_pending = true;
    s_resp_expected_module_id = module_id;
    COMM_UNLOCK();

    int result_count = 0;

    for (int retry = 0; retry < 3; retry++) {
        uint8_t buf[250];
        size_t len = 0;
        protocol_build_data_req(buf, &len, module_id, (uint8_t)(retry + 1));

        uint8_t frame_buf[TCP_MAX_FRAME_SIZE];
        uint8_t* frame = NULL;
        int frame_len = tcp_protocol_frame(frame_buf, sizeof(frame_buf),
                                           buf, (int)len, &frame);
        if (frame_len <= 0) {
            ESP_LOGE(TAG, "frame build failed");
            break;
        }

        // Reset semaphore before send
        COMM_LOCK();
        xSemaphoreTake(s_resp_sem, 0);
        COMM_UNLOCK();

        ssize_t sent = write(fd, frame, (size_t)frame_len);
        if (sent < 0) {
            ESP_LOGW(TAG, "request_read(%u): send failed on attempt %d",
                     module_id, retry + 1);
            if (retry < 2) continue;
            break;
        }

        if (xSemaphoreTake(s_resp_sem,
                           pdMS_TO_TICKS(CONFIG_READ_TIMEOUT_MS)) == pdTRUE) {
            COMM_LOCK();
            int n = s_resp_value_count;
            if (n > max_values) n = max_values;
            for (int i = 0; i < n; i++) {
                out_values[i] = s_resp_values[i];
            }
            s_resp_pending = false;
            COMM_UNLOCK();
            result_count = n;
            break;
        }

        ESP_LOGW(TAG, "request_read(%u): timeout attempt %d/3",
                 module_id, retry + 1);
    }

    if (result_count == 0) {
        COMM_LOCK();
        s_resp_pending = false;
        COMM_UNLOCK();
        ESP_LOGE(TAG, "request_read(%u): failed after 3 attempts", module_id);
    }

    return result_count;
}

// ── Send command ──
esp_err_t tcp_comm_send_cmd(uint8_t module_id, uint16_t cmd_id,
                             const uint8_t* payload, uint16_t payload_len)
{
    int fd = peer_mgr_get_fd(module_id);
    if (fd < 0) {
        ESP_LOGW(TAG, "send_cmd: peer module_id=%u not connected", module_id);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t buf[250];
    size_t len = 0;
    protocol_build_cmd(buf, &len, module_id, 0, cmd_id, payload, payload_len);

    uint8_t frame_buf[TCP_MAX_FRAME_SIZE];
    uint8_t* frame = NULL;
    int frame_len = tcp_protocol_frame(frame_buf, sizeof(frame_buf),
                                       buf, (int)len, &frame);
    if (frame_len <= 0) return ESP_FAIL;

    ssize_t sent = write(fd, frame, (size_t)frame_len);
    if (sent < 0) return ESP_FAIL;
    return ESP_OK;
}

// ── Send raw (sensor mode — not used on master) ──
int tcp_comm_send_raw(const uint8_t* data, int len) {
    (void)data; (void)len;
    return -1;  // master doesn't support raw send
}

// ── Module globals (set by app before init) ──
char g_tcp_module_name[17] = "master";
char g_tcp_module_capability[CONFIG_MAX_CAPABILITY_LEN] = "ESP-LEGO Master";
