#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h
 * 定義 client 與 server 約定的訊息格式，以 REQUEST 表示一筆請求。
 * Request 是一行文字；Response 使用狀態與 payload 長度做 framing。
 */

#include <stddef.h>

/* Request 開頭文字、最大 response payload，以及 DELAY 指令上限。 */
#define PROTOCOL_NAME "REQUEST"
#define PROTOCOL_MAX_PAYLOAD 8192
#define PROTOCOL_MAX_DELAY_SECONDS 10U

/* Server 支援的三種 request command。 */
enum protocol_command {
    PROTOCOL_COMMAND_PING,
    PROTOCOL_COMMAND_SYSINFO,
    PROTOCOL_COMMAND_DELAY
};

/* 解析後的 request；只有 DELAY command 會使用 delay_seconds。 */
struct protocol_request {
    enum protocol_command command;
    unsigned delay_seconds;
};

/* 將結構化 request 組成可傳送的文字，例如 REQUEST SYSINFO。 */
int protocol_build_request(const struct protocol_request *request,
                           char *buffer, size_t capacity);
/* 驗證並解析去除換行後的 request 文字。 */
int protocol_parse_request(const char *line, struct protocol_request *request);
/* 傳送 OK 或 ERR response header，再傳送對應 payload。 */
int protocol_send_ok(int fd, const char *payload);
int protocol_send_error(int fd, int status_code, const char *message);
/* 接收 response header，依宣告長度讀完 payload，並回傳狀態資訊。 */
int protocol_recv_response(int fd, char *payload, size_t capacity,
                           int *is_error, int *status_code,
                           size_t *payload_length);

#endif
