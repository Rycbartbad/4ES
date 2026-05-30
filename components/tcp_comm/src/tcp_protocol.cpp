#include "sdkconfig.h"
#include "tcp_comm/tcp_protocol.h"

#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>

#include "esp_log.h"

static const char* TAG = "tcp_proto";

// ------------------------------------------------------------------
// Helper: read exactly 'count' bytes from fd, retrying on partial reads
// ------------------------------------------------------------------
static int read_exact(int fd, uint8_t* buf, int count, int timeout_ms) {
    int total = 0;
    while (total < count) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) {
            return -1;  // timeout while reading
        }

        ssize_t n = read(fd, buf + total, (size_t)(count - total));
        if (n <= 0) {
            return -1;
        }
        total += (int)n;
    }
    return total;
}

// ==================================================================
// Public API
// ==================================================================

int tcp_protocol_frame(uint8_t* buf, size_t buf_size,
                       const uint8_t* msg, int msg_len,
                       uint8_t** out_frame) {
    if (buf == NULL || msg == NULL || out_frame == NULL) return -1;
    if (msg_len <= 0 || msg_len > 65535) return -1;

    size_t total = (size_t)msg_len + TCP_FRAME_PREFIX_SIZE;
    if (total > buf_size) return -1;

    // Write 2-byte big-endian length prefix
    buf[0] = (uint8_t)((msg_len >> 8) & 0xFF);
    buf[1] = (uint8_t)(msg_len & 0xFF);

    // Copy payload
    if (msg_len > 0) {
        memcpy(buf + TCP_FRAME_PREFIX_SIZE, msg, (size_t)msg_len);
    }

    *out_frame = buf;
    return (int)total;
}

int tcp_protocol_read_frame(int fd, uint8_t* buf, size_t buf_size,
                            int timeout_ms) {
    if (buf == NULL || buf_size < (TCP_FRAME_PREFIX_SIZE + 1)) return -1;

    // --- Wait for data with select() ---
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) return 0;  // interrupted -> treat as timeout
        ESP_LOGW(TAG, "select error: %d", errno);
        return -1;
    }
    if (ret == 0) {
        return 0;  // timeout, no data
    }

    // --- Read 2-byte frame length prefix ---
    int n = read_exact(fd, buf, TCP_FRAME_PREFIX_SIZE, timeout_ms);
    if (n < 0) {
        ESP_LOGW(TAG, "failed to read frame length prefix");
        return -1;
    }

    // --- Parse big-endian frame length ---
    int frame_len = ((int)buf[0] << 8) | (int)buf[1];
    if (frame_len <= 0 ||
        (size_t)frame_len > (buf_size - TCP_FRAME_PREFIX_SIZE)) {
        ESP_LOGW(TAG, "invalid frame length: %d (max %d)",
                 frame_len, (int)(buf_size - TCP_FRAME_PREFIX_SIZE));
        return -1;
    }

    // --- Read the payload ---
    n = read_exact(fd, buf + TCP_FRAME_PREFIX_SIZE, frame_len, timeout_ms);
    if (n < 0) {
        ESP_LOGW(TAG, "failed to read frame payload (%d bytes)", frame_len);
        return -1;
    }

    return TCP_FRAME_PREFIX_SIZE + frame_len;
}
