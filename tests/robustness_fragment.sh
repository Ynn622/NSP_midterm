#!/bin/sh

# robustness_fragment.sh
# 獨立比較 TCP request 分段 的 baseline 與 robust 行為。
# 使用方式：sh tests/robustness_fragment.sh [release|debug]

# 任一未處理錯誤或未定義變數都立即讓測試失敗。
set -eu

# 定位指定 build mode 的 binaries 與測試證據輸出目錄。
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-release}
BIN_DIR="$ROOT/build/$MODE/bin"
OUTPUT_DIR="$ROOT/build/$MODE/test-output/robustness"
SERVER="$BIN_DIR/server"
BASELINE="$BIN_DIR/server_baseline"
CLIENT="$BIN_DIR/client"
SERVER_PID=""
TEST_LOG="$OUTPUT_DIR/fragment-test.log"

# 每一組測試都有自己的進度 log，避免與其他 robustness 測試混淆。
. "$ROOT/tests/test_logging.sh"
init_test_log
test_log "[TEST] Robustness: fragmented TCP request (mode=$MODE)"
test_log "[EXPECT] Baseline rejects an incomplete request; robust reconstructs it and succeeds."

# 關閉目前背景 server；重複呼叫也安全。
cleanup()
{
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
}

# 以固定兩秒 timeout 啟動指定版本，並等待一秒讓 socket 完成 listen。
start_server()
{
    BINARY=$1
    PORT_NUMBER=$2
    LOG_FILE=$3
    "$BINARY" --port "$PORT_NUMBER" --timeout 2 --log-level info \
        >"$LOG_FILE" 2>&1 &
    SERVER_PID=$!
    sleep 1
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        test_log "FAIL: server did not start; inspect $LOG_FILE" >&2
        exit 1
    fi
}

trap cleanup EXIT INT TERM

# 三個 binary 缺少任何一個，都無法進行公平比較。
if [ ! -x "$SERVER" ] || [ ! -x "$BASELINE" ] || [ ! -x "$CLIENT" ]; then
    test_log "FAIL: missing binaries. Run: make demos" >&2
    exit 1
fi
mkdir -p "$OUTPUT_DIR"

#
# 比較一：client 每 50 ms 只送一個 byte。
# baseline 的單次 recv() 會誤判 partial request；robust 會讀到 newline 再解析。
#
test_log "[1/5] Start the baseline server on port 19101."
start_server "$BASELINE" 19101 "$OUTPUT_DIR/fragment-baseline-server.log"
test_log "[2/5] Send SYSINFO one byte every 50 ms to baseline (expected failure)."
if "$CLIENT" --port 19101 --fragment-delay-ms 50 sysinfo \
    >"$OUTPUT_DIR/fragment-baseline-client.log" 2>&1; then
    BASELINE_FRAGMENT_STATUS=0
else
    BASELINE_FRAGMENT_STATUS=$?
fi
cleanup

test_log "[3/5] Start the robust server on port 19102."
start_server "$SERVER" 19102 "$OUTPUT_DIR/fragment-robust-server.log"
test_log "[4/5] Send the same fragmented SYSINFO request to robust (expected success)."
if "$CLIENT" --port 19102 --fragment-delay-ms 50 sysinfo \
    >"$OUTPUT_DIR/fragment-robust-client.log" 2>&1; then
    ROBUST_FRAGMENT_STATUS=0
else
    ROBUST_FRAGMENT_STATUS=$?
fi
cleanup

# 記錄兩邊 exit status；預期 baseline 非 0、robust 為 0。
test_log "[5/5] Compare the baseline and robust client exit statuses."
test_log "  baseline exit status: $BASELINE_FRAGMENT_STATUS"
test_log "  robust exit status:   $ROBUST_FRAGMENT_STATUS"
if [ "$BASELINE_FRAGMENT_STATUS" -eq 0 ] || [ "$ROBUST_FRAGMENT_STATUS" -ne 0 ]; then
    test_log "FAIL: fragmented-request comparison did not show the expected difference." >&2
    exit 1
fi
test_log "PASS: robust recv_line() reconstructed the split request."

test_log "Evidence saved in: $OUTPUT_DIR"
test_log "Test progress and results: $TEST_LOG"
