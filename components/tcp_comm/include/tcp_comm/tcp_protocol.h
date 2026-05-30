#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// TCP frame: [2B: frame_len (big-endian)][MsgHeader + payload]
// frame_len = total bytes after the 2-byte prefix

#define TCP_FRAME_PREFIX_SIZE 2
#define TCP_MAX_FRAME_SIZE    2048

// Frame a message with 2-byte length prefix
// Returns total frame size (msg_len + 2), or -1 on buffer overflow.
// out_frame points to start of frame in buf.
int  tcp_protocol_frame(uint8_t* buf, size_t buf_size,
                         const uint8_t* msg, int msg_len,
                         uint8_t** out_frame);

// Read one complete frame from a TCP socket.
// Returns total frame size (including 2-byte prefix) on success,
// 0 on timeout, -1 on error/disconnect.
int  tcp_protocol_read_frame(int fd, uint8_t* buf, size_t buf_size,
                              int timeout_ms);

#ifdef __cplusplus
}
#endif
