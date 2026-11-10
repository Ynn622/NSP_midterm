#include "netutil.h"

/*
 * netutil.c
 * 實作 client/server 共用的底層工具，最後會編譯進 libnetutil.so。
 * 這裡特別處理 TCP partial I/O 與 EINTR，避免假設一次系統呼叫就能完成工作。
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* fork() 後每個 process 都有自己的一份 runtime log level。 */
static enum net_log_level current_log_level = NET_LOG_LEVEL_INFO;

/* 將 enum 轉成 log 中顯示的名稱。 */
static const char *log_level_name(enum net_log_level level)
{
    switch (level) {
    case NET_LOG_LEVEL_ERROR:
        return "ERROR";
    case NET_LOG_LEVEL_INFO:
        return "INFO";
    case NET_LOG_LEVEL_DEBUG:
        return "DEBUG";
    default:
        return "UNKNOWN";
    }
}

/* 接受名稱或數字兩種格式，讓 command line 使用更方便。 */
int net_parse_log_level(const char *text, enum net_log_level *level)
{
    if (text == NULL || level == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (strcasecmp(text, "error") == 0 || strcmp(text, "0") == 0) {
        *level = NET_LOG_LEVEL_ERROR;
    } else if (strcasecmp(text, "info") == 0 || strcmp(text, "1") == 0) {
        *level = NET_LOG_LEVEL_INFO;
    } else if (strcasecmp(text, "debug") == 0 || strcmp(text, "2") == 0) {
        *level = NET_LOG_LEVEL_DEBUG;
    } else {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/* 修改目前 process 的 log 過濾等級。 */
void net_set_log_level(enum net_log_level level)
{
    current_log_level = level;
}

/* 取得目前 process 的 log 過濾等級。 */
enum net_log_level net_get_log_level(void)
{
    return current_log_level;
}

/* 讓程式可在執行時顯示這個 binary 是否包含 debug 程式碼。 */
int net_debug_compiled(void)
{
#ifdef ENABLE_DEBUG
    return 1;
#else
    return 0;
#endif
}

/*
 * 統一 log 格式：時間、等級、PID、來源檔案與行號。
 * level 大於 runtime 設定時直接返回，避免產生不需要的輸出。
 */
void net_log_message(enum net_log_level level, const char *file, int line,
                     const char *format, ...)
{
    char timestamp[32];
    time_t now;
    struct tm local_time;
    va_list arguments;

    /* 數值較大的等級代表較詳細；超過門檻的訊息不輸出。 */
    if (level > current_log_level) {
        return;
    }

    /* localtime_r() 是 thread/process-safe 版本，不使用共用 static buffer。 */
    now = time(NULL);
    if (localtime_r(&now, &local_time) != NULL) {
        (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                       &local_time);
    } else {
        (void)snprintf(timestamp, sizeof(timestamp), "unknown-time");
    }

    /* stderr 適合 log，stdout 則保留給 client 的正常 response。 */
    (void)fprintf(stderr, "[%s] [%s] [pid=%ld] %s:%d: ", timestamp,
                  log_level_name(level), (long)getpid(), file, line);
    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);
    (void)fputc('\n', stderr);
}

/* 使用 strtol() 驗證整段字串，拒絕 0、負數、溢位與尾端雜字。 */
int net_parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    long value;

    if (text == NULL || port == NULL || *text == '\0') {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 ||
        value > 65535) {
        errno = EINVAL;
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}

/*
 * TCP 的 send() 可能只送出部分資料，因此以迴圈累計 sent。
 * 若被 signal 中斷（EINTR），不視為失敗而是重新嘗試。
 */
int net_send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(fd, cursor + sent, length - sent, 0);

        if (result > 0) {
            sent += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = EPIPE;
        }
        return -1;
    }

    return 0;
}

/*
 * 一次讀一個 byte，直到遇到 newline，確保 request 即使被 TCP 分段也能重組。
 * CR 會被忽略，因此同時接受 Unix LF 與網路常見的 CRLF。
 */
ssize_t net_recv_line(int fd, char *buffer, size_t capacity)
{
    size_t used = 0;

    if (buffer == NULL || capacity < 2) {
        errno = EINVAL;
        return -1;
    }

    while (used + 1 < capacity) {
        char byte;
        ssize_t result = recv(fd, &byte, 1, 0);

        if (result > 0) {
            /* newline 是 framing 邊界，不放入回傳字串。 */
            if (byte == '\n') {
                buffer[used] = '\0';
                return (ssize_t)used;
            }
            if (byte != '\r') {
                buffer[used++] = byte;
            }
            continue;
        }
        if (result == 0) {
            /* 對方正常關閉；保留關閉前已收到的 partial line。 */
            buffer[used] = '\0';
            return (ssize_t)used;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    /* 預留最後一個 byte 給 '\0'，避免字串越界。 */
    buffer[used] = '\0';
    errno = EMSGSIZE;
    return -1;
}

/* 根據 response header 的 payload length，讀取精確數量的 bytes。 */
int net_recv_exact(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(fd, cursor + received, length - received, 0);

        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result == 0) {
            /* 尚未讀滿就遇到 EOF，代表 protocol response 不完整。 */
            errno = ECONNRESET;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    return 0;
}

/* 避免惡意或故障 client 永久占用一個 fork child。 */
int net_set_socket_timeouts(int fd, unsigned seconds)
{
    struct timeval timeout;

    timeout.tv_sec = (time_t)seconds;
    timeout.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   (socklen_t)sizeof(timeout)) < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   (socklen_t)sizeof(timeout)) < 0) {
        return -1;
    }
    return 0;
}

/* nanosleep() 被 signal 中斷時會回傳剩餘時間，繼續睡到完整時間為止。 */
int net_sleep_ms(unsigned milliseconds)
{
    struct timespec remaining;

    remaining.tv_sec = (time_t)(milliseconds / 1000U);
    remaining.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}
