/*
 * server.c
 * Fork-per-client TCP Server：parent process 專心 accept()/fork()，
 * 每個 child process 獨立處理一個 client request，完成後立即結束。
 *
 * 正式版包含 timeout、partial-read、SIGPIPE 與 zombie 回收機制；
 * 定義 BASELINE_MODE 時則關閉指定保護，供 robustness 對照實驗使用。
 */

#include "netutil.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* macOS 與 Linux 取得記憶體、uptime 的 API 不同，於編譯期選擇。 */
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/time.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

/* listen queue 可容納超過作業要求的 10 個 concurrent clients。 */
#define SERVER_BACKLOG 64
/* 正式版 child 最多等待 client request 五秒。 */
#define SERVER_DEFAULT_TIMEOUT 5U

/* signal handler 與 accept loop 之間使用 sig_atomic_t 安全傳遞停止狀態。 */
static volatile sig_atomic_t stop_requested = 0;
/* shutdown signal handler 會關閉 listening socket，使 accept() 立即返回。 */
static int listening_fd = -1;

/* 顯示 server 支援的 command-line options。 */
static void print_usage(const char *program)
{
    (void)fprintf(stderr,
                  "Usage: %s [--bind ADDRESS] [--port PORT] "
                  "[--timeout SECONDS] [--log-level error|info|debug]\n",
                  program);
}

/* SIGINT/SIGTERM handler：只做 async-signal-safe 的狀態修改與 close()。 */
static void handle_shutdown_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
    if (listening_fd >= 0) {
        (void)close(listening_fd);
        listening_fd = -1;
    }
}

#ifndef BASELINE_MODE
/*
 * SIGCHLD handler：非阻塞回收所有已結束 child，避免留下 zombie process。
 * handler 先保存 errno，避免干擾 accept loop 原本正在處理的錯誤狀態。
 */
static void reap_children(int signal_number)
{
    int saved_errno = errno;

    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}
#endif

/* 安裝 parent process 需要的 graceful shutdown 與 robustness signals。 */
static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_shutdown_signal;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }

#ifndef BASELINE_MODE
    /* SA_RESTART 讓可重新開始的系統呼叫在 SIGCHLD 後自動繼續。 */
    memset(&action, 0, sizeof(action));
    action.sa_handler = reap_children;
    action.sa_flags = SA_RESTART;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGCHLD, &action, NULL) < 0) {
        return -1;
    }

    /* 對方提前關閉連線時，send() 回傳 EPIPE 而不是終止 process。 */
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGPIPE, &action, NULL) < 0) {
        return -1;
    }
#endif

    return 0;
}

/*
 * Child 不應沿用 parent 的 shutdown handler，否則 signal handler 可能操作
 * 已在 child 關閉的 listening fd；因此恢復 SIGINT/SIGTERM 預設行為。
 */
static void reset_child_shutdown_signals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
}

/* 驗證完整的 unsigned 整數字串及指定上下限。 */
static int parse_unsigned(const char *text, unsigned minimum, unsigned maximum,
                          unsigned *value)
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

/* 依平台 API 取得實體記憶體總 bytes；無法取得時回傳 0。 */
static unsigned long long get_total_memory_bytes(void)
{
#if defined(__APPLE__)
    /* macOS 透過 sysctl 的 hw.memsize 取得 uint64_t bytes。 */
    uint64_t memory = 0;
    size_t length = sizeof(memory);

    if (sysctlbyname("hw.memsize", &memory, &length, NULL, 0) == 0) {
        return (unsigned long long)memory;
    }
#elif defined(__linux__)
    /* Linux sysinfo.totalram 需乘上 mem_unit 才是真正的 bytes。 */
    struct sysinfo information;

    if (sysinfo(&information) == 0) {
        return (unsigned long long)information.totalram *
               (unsigned long long)information.mem_unit;
    }
#endif
    return 0;
}

/* 依平台 API 計算開機至今的秒數；無法取得時回傳 0。 */
static unsigned long long get_uptime_seconds(void)
{
#if defined(__APPLE__)
    /* macOS 取得 boot time，再以目前時間相減。 */
    int management_information_base[2] = {CTL_KERN, KERN_BOOTTIME};
    struct timeval boot_time;
    size_t length = sizeof(boot_time);
    time_t now = time(NULL);

    if (sysctl(management_information_base, 2, &boot_time, &length, NULL, 0) ==
            0 &&
        now >= boot_time.tv_sec) {
        return (unsigned long long)(now - boot_time.tv_sec);
    }
#elif defined(__linux__)
    /* Linux sysinfo 結構已直接提供 uptime 秒數。 */
    struct sysinfo information;

    if (sysinfo(&information) == 0 && information.uptime >= 0) {
        return (unsigned long long)information.uptime;
    }
#endif
    return 0;
}

/*
 * 收集 SYSINFO command 的 response payload。
 * 所有欄位都寫入 caller 提供的 bounded buffer，避免動態記憶體管理。
 */
static int collect_system_information(char *buffer, size_t capacity)
{
    char hostname[256] = "unknown";
    char timestamp[64] = "unknown";
    struct utsname system_name;
    struct tm local_time;
    time_t now = time(NULL);
    long online_processors;
    unsigned long long memory_mib;
    int written;

    /* hostname、uname 與 local time 任一失敗時，以 unknown 保持格式完整。 */
    memset(&system_name, 0, sizeof(system_name));
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        (void)snprintf(hostname, sizeof(hostname), "unknown");
    }
    hostname[sizeof(hostname) - 1] = '\0';
    if (uname(&system_name) < 0) {
        (void)snprintf(system_name.sysname, sizeof(system_name.sysname),
                       "unknown");
        (void)snprintf(system_name.release, sizeof(system_name.release),
                       "unknown");
        (void)snprintf(system_name.machine, sizeof(system_name.machine),
                       "unknown");
    }
    if (localtime_r(&now, &local_time) != NULL) {
        (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %z",
                       &local_time);
    }

    /* _SC_NPROCESSORS_ONLN 回報目前作業系統可用的 logical CPU 數。 */
    online_processors = sysconf(_SC_NPROCESSORS_ONLN);
    if (online_processors < 0) {
        online_processors = 0;
    }
    memory_mib = get_total_memory_bytes() / (1024ULL * 1024ULL);

    /* child_pid 可直接證明每次 request 是由不同 fork child 處理。 */
    written = snprintf(
        buffer, capacity,
        "hostname: %s\n"
        "os: %s\n"
        "kernel: %s\n"
        "architecture: %s\n"
        "cpu_online: %ld\n"
        "memory_total_mib: %llu\n"
        "uptime_seconds: %llu\n"
        "server_time: %s\n"
        "child_pid: %ld\n",
        hostname, system_name.sysname, system_name.release,
        system_name.machine, online_processors, memory_mib,
        get_uptime_seconds(), timestamp, (long)getpid());

    if (written < 0 || (size_t)written >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    return 0;
}

/*
 * 正式版使用 net_recv_line() 重組 TCP 分段；baseline 故意只 recv() 一次，
 * 用來展示「假設一次 recv 就是完整 request」會造成的錯誤。
 */
static ssize_t receive_request_line(int client_fd, char *line, size_t capacity)
{
#ifdef BASELINE_MODE
    ssize_t received = recv(client_fd, line, capacity - 1, 0);

    if (received > 0) {
        size_t length = (size_t)received;

        line[length] = '\0';
        while (length > 0 &&
               (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        return (ssize_t)length;
    }
    return received;
#else
    return net_recv_line(client_fd, line, capacity);
#endif
}

/* 處理單一 client 的一筆 request，成功回傳 0，失敗回傳 -1。 */
static int handle_client(int client_fd, unsigned timeout_seconds)
{
    char request_line[NET_MAX_LINE];
    char response[PROTOCOL_MAX_PAYLOAD + 1];
    struct protocol_request request;
    ssize_t received;

#ifndef BASELINE_MODE
    /* Read/write timeout 防止 slow client 永久占用 child process。 */
    if (net_set_socket_timeouts(client_fd, timeout_seconds) < 0) {
        NET_LOG_ERROR("cannot set client timeout: %s", strerror(errno));
        return -1;
    }
#else
    (void)timeout_seconds;
#endif

    /* 讀取一行 request；各種異常會轉成清楚的 protocol error。 */
    received = receive_request_line(client_fd, request_line,
                                    sizeof(request_line));
    if (received < 0) {
#ifndef BASELINE_MODE
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            NET_LOG_INFO("client request timed out");
            (void)protocol_send_error(client_fd, 408, "request timeout\n");
        } else if (errno == EMSGSIZE) {
            NET_LOG_INFO("client request exceeded %d bytes", NET_MAX_LINE - 1);
            (void)protocol_send_error(client_fd, 413, "request too large\n");
        } else {
            NET_LOG_ERROR("receive failed: %s", strerror(errno));
        }
#else
        NET_LOG_ERROR("baseline receive failed: %s", strerror(errno));
#endif
        return -1;
    }
    if (received == 0) {
        NET_LOG_INFO("client closed without a request");
        return -1;
    }

    NET_LOG_DEBUG("received request: %s", request_line);
    /* Server 永遠重新驗證輸入，不信任 client 已經做過的檢查。 */
    if (protocol_parse_request(request_line, &request) < 0) {
        NET_LOG_INFO("rejected malformed request: %s", request_line);
        return protocol_send_error(client_fd, 400, "invalid request\n");
    }

    /* 依 command 分派功能，每個分支都透過 protocol API 傳送 framed response。 */
    switch (request.command) {
    case PROTOCOL_COMMAND_PING:
        return protocol_send_ok(client_fd, "PONG\n");
    case PROTOCOL_COMMAND_SYSINFO:
        if (collect_system_information(response, sizeof(response)) < 0) {
            NET_LOG_ERROR("cannot collect system information: %s",
                          strerror(errno));
            return protocol_send_error(client_fd, 500,
                                       "system information unavailable\n");
        }
        return protocol_send_ok(client_fd, response);
    case PROTOCOL_COMMAND_DELAY:
        /* DELAY 用於 concurrency proof：多個 child 應同時等待而非依序等待。 */
        NET_LOG_INFO("delaying response for %u second(s)",
                     request.delay_seconds);
        if (net_sleep_ms(request.delay_seconds * 1000U) < 0) {
            return protocol_send_error(client_fd, 500, "delay interrupted\n");
        }
        (void)snprintf(response, sizeof(response),
                       "delay_seconds: %u\nchild_pid: %ld\n",
                       request.delay_seconds, (long)getpid());
        return protocol_send_ok(client_fd, response);
    default:
        return protocol_send_error(client_fd, 400, "unknown command\n");
    }
}

/* 建立 IPv4 TCP listening socket，完成 SO_REUSEADDR、bind 與 listen。 */
static int create_listening_socket(const char *bind_address, uint16_t port)
{
    int fd;
    int reuse_address = 1;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    /* 讓 server 關閉後較快重新綁定相同 port。 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   (socklen_t)sizeof(reuse_address)) < 0) {
        (void)close(fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    /* htons() 將 host byte order 的 port 轉成 network byte order。 */
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, SERVER_BACKLOG) < 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* Server 進入點：解析參數、建立 socket，然後執行 accept/fork loop。 */
int main(int argc, char **argv)
{
    const char *bind_address = "127.0.0.1";
    uint16_t port = NET_DEFAULT_PORT;
    unsigned timeout_seconds = SERVER_DEFAULT_TIMEOUT;
    enum net_log_level log_level = NET_LOG_LEVEL_INFO;
    int index;

    /* 逐一解析 options；缺值、未知 option 或非法範圍都立即拒絕。 */
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            bind_address = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (net_parse_port(argv[++index], &port) < 0) {
                (void)fprintf(stderr, "Invalid port: %s\n", argv[index]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--timeout") == 0 &&
                   index + 1 < argc) {
            if (parse_unsigned(argv[++index], 1, 3600, &timeout_seconds) < 0) {
                (void)fprintf(stderr, "Invalid timeout: %s\n", argv[index]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--log-level") == 0 &&
                   index + 1 < argc) {
            if (net_parse_log_level(argv[++index], &log_level) < 0) {
                (void)fprintf(stderr, "Invalid log level: %s\n", argv[index]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* 先完成 signal 與 listening socket 初始化，再進入無限服務迴圈。 */
    net_set_log_level(log_level);
    if (install_signal_handlers() < 0) {
        NET_LOG_ERROR("cannot install signal handlers: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    listening_fd = create_listening_socket(bind_address, port);
    if (listening_fd < 0) {
        NET_LOG_ERROR("cannot listen on %s:%u: %s", bind_address,
                      (unsigned)port, strerror(errno));
        return EXIT_FAILURE;
    }

#ifdef BASELINE_MODE
    NET_LOG_INFO("BASELINE server: partial-read, timeout, and zombie "
                 "protections are disabled for demonstration");
#else
    NET_LOG_INFO("robust server enabled (client timeout=%u seconds)",
                 timeout_seconds);
#endif
    NET_LOG_INFO("listening on %s:%u; compile-time debug=%s", bind_address,
                 (unsigned)port, net_debug_compiled() ? "enabled" : "disabled");

    /* Parent 永遠回到 accept()；每個連線的實際工作交給 fork child。 */
    while (!stop_requested) {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        char client_ip[INET_ADDRSTRLEN] = "unknown";
        int client_fd;
        pid_t child_pid;

        /* accept() 回傳的是這一個 client 專用的 connected socket。 */
        client_fd = accept(listening_fd, (struct sockaddr *)&client_address,
                           &client_length);
        if (client_fd < 0) {
            if (stop_requested) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            NET_LOG_ERROR("accept failed: %s", strerror(errno));
            continue;
        }

        (void)inet_ntop(AF_INET, &client_address.sin_addr, client_ip,
                        sizeof(client_ip));
        /* fork() 後 parent/child 各自關閉不需要的 descriptor。 */
        child_pid = fork();
        if (child_pid < 0) {
            NET_LOG_ERROR("fork failed for %s:%u: %s", client_ip,
                          (unsigned)ntohs(client_address.sin_port),
                          strerror(errno));
            (void)close(client_fd);
            continue;
        }
        if (child_pid == 0) {
            int result;

            /* Child 只使用 client_fd，不接受其他 client。 */
            reset_child_shutdown_signals();
            (void)close(listening_fd);
            listening_fd = -1;
            NET_LOG_INFO("child handling %s:%u", client_ip,
                         (unsigned)ntohs(client_address.sin_port));
            result = handle_client(client_fd, timeout_seconds);
            if (result < 0) {
                NET_LOG_DEBUG("child request ended with an error: %s",
                              strerror(errno));
            }
            (void)close(client_fd);
            _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
        }

        /* Parent 不處理本次資料，關閉自己的 client_fd copy 後繼續 accept。 */
        NET_LOG_DEBUG("forked child pid=%ld for %s:%u", (long)child_pid,
                      client_ip, (unsigned)ntohs(client_address.sin_port));
        (void)close(client_fd);
    }

    /* 收到停止 signal 後清理 parent listening socket。 */
    if (listening_fd >= 0) {
        (void)close(listening_fd);
    }
    NET_LOG_INFO("server stopped");
    return EXIT_SUCCESS;
}
