#!/bin/sh

# robustness_timeout.sh
# 獨立比較 Idle client timeout 的 baseline 與 robust 行為。
# 使用方式：sh tests/robustness_timeout.sh [release|debug]

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
TEST_LOG="$OUTPUT_DIR/timeout-test.log"

. "$ROOT/tests/test_logging.sh"
init_test_log
test_log "[TEST] Robustness: connected client sends no request (mode=$MODE)"
test_log "[EXPECT] After 3s, baseline still has a waiting child; robust has released it (timeout=2s)."

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
    test_command "$BINARY --port $PORT_NUMBER --timeout 2 --log-level info > $LOG_FILE 2>&1 &"
    "$BINARY" --port "$PORT_NUMBER" --timeout 2 --log-level info \
        >"$LOG_FILE" 2>&1 &
    SERVER_PID=$!
    sleep 1
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        test_log "FAIL: server did not start; inspect $LOG_FILE" >&2
        exit 1
    fi
}

# 計算指定 parent 下仍在運作的非 zombie children。
count_live_children()
{
    ps -axo ppid=,stat= | awk -v parent="$1" \
        '$1 == parent && $2 !~ /^Z/ { count++ } END { print count + 0 }'
}

trap cleanup EXIT INT TERM

# 三個 binary 缺少任何一個，都無法進行公平比較。
if [ ! -x "$SERVER" ] || [ ! -x "$BASELINE" ] || [ ! -x "$CLIENT" ]; then
    test_log "FAIL: missing binaries. Run: make demos" >&2
    exit 1
fi
mkdir -p "$OUTPUT_DIR"

#
# 比較二：client 連線四秒但不送資料。
# 三秒時 baseline child 應仍卡在 recv()；robust child 應已在兩秒 timeout 後結束。
#
test_log "[1/5] Start the baseline server on port 19103 (no request timeout)."
start_server "$BASELINE" 19103 "$OUTPUT_DIR/timeout-baseline-server.log"
test_log "[2/5] Connect without sending a request for 4s; count baseline children after 3s."
test_command "$CLIENT --port 19103 --sleep-no-request 4 > $OUTPUT_DIR/timeout-baseline-client.log 2>&1 &"
"$CLIENT" --port 19103 --sleep-no-request 4 \
    >"$OUTPUT_DIR/timeout-baseline-client.log" 2>&1 &
NO_REQUEST_PID=$!
sleep 3
test_command "ps -axo ppid=,stat= | awk -v parent=$SERVER_PID '\$1 == parent && \$2 !~ /^Z/'"
BASELINE_LIVE=$(count_live_children "$SERVER_PID")
test_log "      Baseline live server children after 3s: $BASELINE_LIVE"
wait "$NO_REQUEST_PID" || true
cleanup

test_log "[3/5] Start the robust server on port 19104 (request timeout=2s)."
start_server "$SERVER" 19104 "$OUTPUT_DIR/timeout-robust-server.log"
test_log "[4/5] Repeat the no-request client; count robust children after 3s."
test_command "$CLIENT --port 19104 --sleep-no-request 4 > $OUTPUT_DIR/timeout-robust-client.log 2>&1 &"
"$CLIENT" --port 19104 --sleep-no-request 4 \
    >"$OUTPUT_DIR/timeout-robust-client.log" 2>&1 &
NO_REQUEST_PID=$!
sleep 3
test_command "ps -axo ppid=,stat= | awk -v parent=$SERVER_PID '\$1 == parent && \$2 !~ /^Z/'"
ROBUST_LIVE=$(count_live_children "$SERVER_PID")
test_log "      Robust live server children after 3s: $ROBUST_LIVE"
wait "$NO_REQUEST_PID" || true
cleanup

# 預期 baseline 至少一個 live child，robust 則為零。
test_log "[5/5] Verify that the timeout releases the robust server child."
test_log "  baseline live children after 3s: $BASELINE_LIVE"
test_log "  robust live children after 3s:   $ROBUST_LIVE"
if [ "$BASELINE_LIVE" -lt 1 ] || [ "$ROBUST_LIVE" -ne 0 ]; then
    test_log "FAIL: timeout comparison did not show the expected difference." >&2
    exit 1
fi
test_pass "the robust server released the idle child after 2s."

test_file "Evidence directory: $OUTPUT_DIR"
test_file "Test progress and results: $TEST_LOG"
