/*
 * client.c
 * 命令列 Client：連線至 server、送出一筆 REQUEST 請求、接收一筆回覆。
 * 除正常的 ping/sysinfo/delay 外，也提供逐字傳送與不送請求的測試模式，
 * 用來重現 robustness 機制開啟前後的差異。
 */

#include "netutil.h"
#include "protocol.h"

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Client 等待 response 的最長時間，需大於 DELAY command 的 10 秒上限。 */
#define CLIENT_DEFAULT_TIMEOUT 15U

/* 顯示一般 command 與 robustness 測試 options。 */
static void print_usage(const char *program)
{
    (void)fprintf(
        stderr,
        "Usage:\n"
        "  %s [options] ping\n"
        "  %s [options] sysinfo\n"
        "  %s [options] delay SECONDS\n"
        "  %s [options] --sleep-no-request SECONDS\n\n"
        "Options:\n"
        "  --host HOST                  default: 127.0.0.1\n"
        "  --port PORT                  default: 9090\n"
        "  --log-level LEVEL            error, info, or debug\n"
        "  --fragment-delay-ms MS       send request one byte at a time\n"
        "  --sleep-no-request SECONDS   connect, sleep SECONDS without sending a request\n",
        program, program, program, program);
}

/* 驗證完整的 unsigned 整數字串及指定上下限。 */
static int parse_unsigned(const char *text, unsigned minimum, unsigned maximum, unsigned *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL || *text == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

/* 對方關閉連線時讓 send() 回傳 EPIPE，避免 client 被 SIGPIPE 直接終止。 */
static int ignore_sigpipe(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));     // 結構的記憶體全部填成 0
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGPIPE, &action, NULL) < 0) {
        return -1;
    }
    return 0;
}

/* 使用 getaddrinfo() 解析 hostname，逐一嘗試可用的 IPv4 TCP address。 */
static int connect_to_server(const char *host, uint16_t port)
{
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *current;
    int fd = -1;
    int result;

    /* getaddrinfo() 的 service/port 參數使用字串格式。 */
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    result = getaddrinfo(host, port_text, &hints, &addresses);
    if (result != 0) {
        NET_LOG_ERROR("cannot resolve %s: %s", host, gai_strerror(result));
        return -1;
    }

    /* 某一筆 address 失敗時關閉 socket，繼續嘗試下一筆結果。 */
    for (current = addresses; current != NULL; current = current->ai_next) {
        fd = socket(current->ai_family, current->ai_socktype,
                    current->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

/*
 * 將 request 一次只送一個 byte，並在 byte 間等待指定毫秒。
 * 此模式刻意製造 TCP fragmented request，驗證 server 是否會重組完整一行。
 */
static int send_fragmented(int fd, const char *request, unsigned delay_milliseconds)
{
    size_t index;
    size_t length = strlen(request);

    for (index = 0; index < length; ++index) {
        if (net_send_all(fd, request + index, 1) < 0) {
            return -1;
        }
        if (index + 1 < length && net_sleep_ms(delay_milliseconds) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Client 進入點：解析參數、建立 request、連線、收送 response。 */
int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    uint16_t port = NET_DEFAULT_PORT;
    enum net_log_level log_level = NET_LOG_LEVEL_INFO;
    unsigned fragment_delay_ms = 0;
    unsigned no_request_seconds = 0;
    struct protocol_request request;
    char request_text[NET_MAX_LINE];
    char response[PROTOCOL_MAX_PAYLOAD + 1];
    int is_error;
    int status_code;
    size_t response_length;
    int fd;
    int index = 1;
    int request_length;

    /* 所有 `--option` 必須放在 command 之前。 */
    while (index < argc && strncmp(argv[index], "--", 2) == 0) {
        if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
            host = argv[index + 1];
            index += 2;
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (net_parse_port(argv[index + 1], &port) < 0) {
                (void)fprintf(stderr, "Invalid port: %s\n", argv[index + 1]);
                return EXIT_FAILURE;
            }
            index += 2;
        } else if (strcmp(argv[index], "--log-level") == 0 && index + 1 < argc) {
            if (net_parse_log_level(argv[index + 1], &log_level) < 0) {
                (void)fprintf(stderr, "Invalid log level: %s\n",
                              argv[index + 1]);
                return EXIT_FAILURE;
            }
            index += 2;
        } else if (strcmp(argv[index], "--fragment-delay-ms") == 0 && index + 1 < argc) {
            if (parse_unsigned(argv[index + 1], 1, 2000,
                               &fragment_delay_ms) < 0) {
                (void)fprintf(stderr, "Invalid fragment delay: %s\n",
                              argv[index + 1]);
                return EXIT_FAILURE;
            }
            index += 2;
        } else if (strcmp(argv[index], "--sleep-no-request") == 0 && index + 1 < argc) {
            if (parse_unsigned(argv[index + 1], 1, 3600,
                               &no_request_seconds) < 0) {
                (void)fprintf(stderr, "Invalid sleep-no-request duration: %s\n",
                              argv[index + 1]);
                return EXIT_FAILURE;
            }
            index += 2;
        } else if (strcmp(argv[index], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* 設定 runtime log，並先保護後續所有 send() 不受 SIGPIPE 終止。 */
    net_set_log_level(log_level);
    if (ignore_sigpipe() < 0) {
        NET_LOG_ERROR("cannot ignore SIGPIPE: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    /* --sleep-no-request 是獨立診斷模式；一般模式則必須指定合法 command。 */
    if (no_request_seconds == 0) {
        if (index >= argc) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (strcasecmp(argv[index], "ping") == 0) {
            request.command = PROTOCOL_COMMAND_PING;
            request.delay_seconds = 0;
            ++index;
        } else if (strcasecmp(argv[index], "sysinfo") == 0) {
            request.command = PROTOCOL_COMMAND_SYSINFO;
            request.delay_seconds = 0;
            ++index;
        } else if (strcasecmp(argv[index], "delay") == 0 && index + 1 < argc) {
            /* Client 先驗證範圍；Server 收到後仍會做第二次驗證。 */
            request.command = PROTOCOL_COMMAND_DELAY;
            if (parse_unsigned(argv[index + 1], 0,
                               PROTOCOL_MAX_DELAY_SECONDS,
                               &request.delay_seconds) < 0) {
                (void)fprintf(stderr, "Delay must be between 0 and %u\n",
                              PROTOCOL_MAX_DELAY_SECONDS);
                return EXIT_FAILURE;
            }
            index += 2;
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (index != argc) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    } else if (index != argc) {
        (void)fprintf(stderr, "--sleep-no-request does not take a command\n");
        return EXIT_FAILURE;
    }

    /* connect 成功後設定 timeout，避免 server 無回覆時永久等待。 */
    fd = connect_to_server(host, port);
    if (fd < 0) {
        NET_LOG_ERROR("cannot connect to %s:%u: %s", host, (unsigned)port,
                      strerror(errno));
        return EXIT_FAILURE;
    }
    if (net_set_socket_timeouts(fd, CLIENT_DEFAULT_TIMEOUT) < 0) {
        NET_LOG_ERROR("cannot set socket timeout: %s", strerror(errno));
        (void)close(fd);
        return EXIT_FAILURE;
    }
    NET_LOG_DEBUG("connected to %s:%u; compile-time debug=%s", host,
                  (unsigned)port,
                  net_debug_compiled() ? "enabled" : "disabled");

    /* 不送請求模式只連線並等待，用來觀察 server 是否釋放 idle child。 */
    if (no_request_seconds > 0) {
        NET_LOG_INFO("connected without sending a request; waiting %u second(s)",
                     no_request_seconds);
        (void)net_sleep_ms(no_request_seconds * 1000U);
        (void)close(fd);
        return EXIT_SUCCESS;
    }

    /* 透過 shared library 組成請求文字，例如 REQUEST SYSINFO。 */
    request_length = protocol_build_request(&request, request_text, sizeof(request_text));
    if (request_length < 0) {
        NET_LOG_ERROR("cannot build request: %s", strerror(errno));
        (void)close(fd);
        return EXIT_FAILURE;
    }

    /* 一般模式完整送出；fragment 模式則刻意拆成單一 bytes。 */
    if (fragment_delay_ms > 0) {
        NET_LOG_INFO("sending %d-byte request one byte at a time", request_length);
        if (send_fragmented(fd, request_text, fragment_delay_ms) < 0) {
            NET_LOG_ERROR("fragmented send failed: %s", strerror(errno));
            (void)close(fd);
            return EXIT_FAILURE;
        }
    } else if (net_send_all(fd, request_text, (size_t)request_length) < 0) {
        NET_LOG_ERROR("send failed: %s", strerror(errno));
        (void)close(fd);
        return EXIT_FAILURE;
    }

    /* Header 會告知 payload 長度，shared library 會保證完整接收。 */
    if (protocol_recv_response(fd, response, sizeof(response), &is_error,
                               &status_code, &response_length) < 0) {
        NET_LOG_ERROR("cannot receive response: %s", strerror(errno));
        (void)close(fd);
        return EXIT_FAILURE;
    }
    (void)close(fd);

    /* 正常 payload 寫到 stdout；server 的 ERR status 另以非零狀態結束。 */
    if (response_length > 0) {
        (void)fwrite(response, 1, response_length, stdout);
    }
    if (is_error) {
        (void)fprintf(stderr, "Server returned error %d\n", status_code);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
