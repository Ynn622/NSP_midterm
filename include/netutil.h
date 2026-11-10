#ifndef NETUTIL_H
#define NETUTIL_H

/*
 * netutil.h
 * Client 與 Server 共用的網路工具介面。
 * 內容包含：log 控制、參數驗證、完整收送資料、socket timeout 與毫秒等待。
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 預設 TCP port，以及一行 protocol 訊息可使用的最大 buffer 大小。 */
#define NET_DEFAULT_PORT 9090
#define NET_MAX_LINE 1024

/*
 * Runtime log 等級：數值越大，輸出的資訊越詳細。
 * ERROR 一定保留；DEBUG 是否存在還會受到編譯期 ENABLE_DEBUG 控制。
 */
enum net_log_level {
    NET_LOG_LEVEL_ERROR = 0,
    NET_LOG_LEVEL_INFO = 1,
    NET_LOG_LEVEL_DEBUG = 2
};

/* 將 error/info/debug 或 0/1/2 轉換成 log 等級。 */
int net_parse_log_level(const char *text, enum net_log_level *level);
/* 設定與讀取目前 process 的 runtime log 等級。 */
void net_set_log_level(enum net_log_level level);
enum net_log_level net_get_log_level(void);
/* 回傳程式編譯時是否有定義 ENABLE_DEBUG。 */
int net_debug_compiled(void);
/* 實際的 log 輸出函式；macro 會自動帶入來源檔名與行號。 */
void net_log_message(enum net_log_level level, const char *file, int line,
                     const char *format, ...);

/* ERROR 與 INFO 永遠編譯；是否顯示則由 runtime log level 決定。 */
#define NET_LOG_ERROR(...) \
    net_log_message(NET_LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define NET_LOG_INFO(...) \
    net_log_message(NET_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)

/*
 * 編譯期 debug 控制：release build 會把 NET_LOG_DEBUG 直接變成空操作，
 * 因此即使 runtime 指定 debug，也不會執行 debug log。
 */
#ifdef ENABLE_DEBUG
#define NET_LOG_DEBUG(...) \
    net_log_message(NET_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define NET_LOG_DEBUG(...) ((void)0)
#endif

/* 驗證 port 文字必須落在 1～65535。 */
int net_parse_port(const char *text, uint16_t *port);
/* 持續呼叫 send()，直到 length bytes 全部送完。 */
int net_send_all(int fd, const void *buffer, size_t length);
/* 持續接收直到換行、EOF 或 buffer 滿；成功時回傳不含換行的長度。 */
ssize_t net_recv_line(int fd, char *buffer, size_t capacity);
/* 依 protocol 指定長度，接收剛好 length bytes。 */
int net_recv_exact(int fd, void *buffer, size_t length);
/* 同時設定 socket 的接收與傳送 timeout。 */
int net_set_socket_timeouts(int fd, unsigned seconds);
/* 以 nanosleep() 實作可處理 EINTR 的毫秒等待。 */
int net_sleep_ms(unsigned milliseconds);

#endif
