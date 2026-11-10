#include "protocol.h"

/*
 * protocol.c
 * 負責 REQUEST 請求的組裝、解析，以及回覆的格式與長度資訊。
 * 這些函式同時被 client/server 使用，因此編譯進 libnetutil.so。
 */

#include "netutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 將 command 結構轉成以 newline 結尾的 request 文字。 */
int protocol_build_request(const struct protocol_request *request,
                           char *buffer, size_t capacity)
{
    int written;

    if (request == NULL || buffer == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    switch (request->command) {
    case PROTOCOL_COMMAND_PING:
        written = snprintf(buffer, capacity, PROTOCOL_NAME " PING\n");
        break;
    case PROTOCOL_COMMAND_SYSINFO:
        written = snprintf(buffer, capacity, PROTOCOL_NAME " SYSINFO\n");
        break;
    case PROTOCOL_COMMAND_DELAY:
        /* Client 端先限制 DELAY，Server 端解析時仍會再次驗證。 */
        if (request->delay_seconds > PROTOCOL_MAX_DELAY_SECONDS) {
            errno = EINVAL;
            return -1;
        }
        written = snprintf(buffer, capacity, PROTOCOL_NAME " DELAY %u\n",
                           request->delay_seconds);
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    /* snprintf() 回傳值大於等於 capacity 代表輸出被截斷。 */
    if (written < 0 || (size_t)written >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    return written;
}

/* 驗證 command 名稱、參數數量與 DELAY 範圍。 */
int protocol_parse_request(const char *line, struct protocol_request *request)
{
    const char *delay_prefix = PROTOCOL_NAME " DELAY ";
    size_t delay_prefix_length = strlen(delay_prefix);

    if (line == NULL || request == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(line, PROTOCOL_NAME " PING") == 0) {
        request->command = PROTOCOL_COMMAND_PING;
        request->delay_seconds = 0;
        return 0;
    }
    if (strcmp(line, PROTOCOL_NAME " SYSINFO") == 0) {
        request->command = PROTOCOL_COMMAND_SYSINFO;
        request->delay_seconds = 0;
        return 0;
    }
    if (strncmp(line, delay_prefix, delay_prefix_length) == 0) {
        const char *number = line + delay_prefix_length;
        char *end = NULL;
        unsigned long value;

        errno = 0;
        /* end 必須指向字串結尾，避免接受 "3abc" 這類模糊輸入。 */
        value = strtoul(number, &end, 10);
        if (errno != 0 || end == number || *end != '\0' ||
            value > PROTOCOL_MAX_DELAY_SECONDS) {
            errno = EINVAL;
            return -1;
        }
        request->command = PROTOCOL_COMMAND_DELAY;
        request->delay_seconds = (unsigned)value;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

/* 共用的 response 傳送流程：先 header，再依長度送 payload。 */
static int send_response(int fd, const char *header, const char *payload,
                         size_t payload_length)
{
    if (net_send_all(fd, header, strlen(header)) < 0) {
        return -1;
    }
    if (payload_length > 0 && net_send_all(fd, payload, payload_length) < 0) {
        return -1;
    }
    return 0;
}

/* 成功格式：OK <payload_bytes>\n<payload>。 */
int protocol_send_ok(int fd, const char *payload)
{
    char header[64];
    size_t length;
    int written;

    if (payload == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(payload);
    if (length > PROTOCOL_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    written = snprintf(header, sizeof(header), "OK %zu\n", length);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        errno = EMSGSIZE;
        return -1;
    }
    return send_response(fd, header, payload, length);
}

/* 錯誤格式：ERR <status_code> <payload_bytes>\n<message>。 */
int protocol_send_error(int fd, int status_code, const char *message)
{
    char header[64];
    size_t length;
    int written;

    if (message == NULL || status_code < 100 || status_code > 599) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(message);
    if (length > PROTOCOL_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    written = snprintf(header, sizeof(header), "ERR %d %zu\n", status_code,
                       length);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        errno = EMSGSIZE;
        return -1;
    }
    return send_response(fd, header, message, length);
}

/*
 * Client 端 response parser：先讀一行 header，再讀 header 宣告的 payload 長度。
 * 多加一個 byte 給 '\0'，讓收到的 payload 同時可以當作 C 字串使用。
 */
int protocol_recv_response(int fd, char *payload, size_t capacity,
                           int *is_error, int *status_code,
                           size_t *payload_length)
{
    char header[NET_MAX_LINE];
    char extra;
    size_t length;
    int code = 0;
    ssize_t header_length;

    if (payload == NULL || capacity == 0 || is_error == NULL ||
        status_code == NULL || payload_length == NULL) {
        errno = EINVAL;
        return -1;
    }

    header_length = net_recv_line(fd, header, sizeof(header));
    if (header_length <= 0) {
        if (header_length == 0) {
            errno = ECONNRESET;
        }
        return -1;
    }

    /* 最後的 %c 用來偵測不該存在的額外欄位。 */
    if (sscanf(header, "OK %zu %c", &length, &extra) == 1) {
        *is_error = 0;
    } else if (sscanf(header, "ERR %d %zu %c", &code, &length, &extra) == 2 &&
               code >= 100 && code <= 599) {
        *is_error = 1;
    } else {
        errno = EPROTO;
        return -1;
    }

    /* 同時檢查 protocol 上限與呼叫端實際提供的 buffer 大小。 */
    if (length > PROTOCOL_MAX_PAYLOAD || length + 1 > capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    if (length > 0 && net_recv_exact(fd, payload, length) < 0) {
        return -1;
    }
    payload[length] = '\0';
    *status_code = code;
    *payload_length = length;
    return 0;
}
