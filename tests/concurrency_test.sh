#!/bin/sh

# concurrency_test.sh
# 自動證明 server 能以 fork child 同時服務至少 10 個 clients。
# 每個 client 都要求 DELAY 3；並行時總時間應接近 3 秒，而非串行的 30 秒。

# 任一未處理錯誤或未定義變數都立即讓測試失敗。
set -eu

# 可用第一個參數選擇 release/debug，也可用環境變數調整 client 數與 port。
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-release}
CLIENTS=${CLIENTS:-10}
PORT=${PORT:-19090}
BIN_DIR="$ROOT/build/$MODE/bin"
OUTPUT_DIR="$ROOT/build/$MODE/test-output/concurrency"
SERVER="$BIN_DIR/server"
CLIENT="$BIN_DIR/client"
SERVER_PID=""
TEST_LOG="$OUTPUT_DIR/test.log"

# 載入共用輸出工具，讓終端機進度也保存在測試 log。
. "$ROOT/tests/test_logging.sh"
init_test_log
test_log "[TEST] Concurrency: at least 10 simultaneous clients (mode=$MODE)"
test_log "[EXPECT] Each client requests DELAY 3; at least 10 server children must overlap."

# 無論正常結束、失敗或收到 signal，都關閉背景 server，避免占用 port。
cleanup()
{
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}

# 只計算指定 parent 之下仍在運作、且不是 zombie 的 child 數量。
count_live_children()
{
    ps -axo ppid=,stat= | awk -v parent="$1" \
        '$1 == parent && $2 !~ /^Z/ { count++ } END { print count + 0 }'
}

trap cleanup EXIT INT TERM

# 測試前先確認 binary 存在，並禁止低於作業要求的 10 clients。
if [ ! -x "$SERVER" ] || [ ! -x "$CLIENT" ]; then
    test_log "FAIL: missing binaries. Run: make" >&2
    exit 1
fi
if [ "$CLIENTS" -lt 10 ]; then
    test_log "FAIL: CLIENTS must be at least 10 for the assignment proof." >&2
    exit 1
fi

# 啟動正式 server，log 寫入 build 目錄供報告與除錯使用。
test_log "[1/5] Start the robust server on 127.0.0.1:$PORT."
"$SERVER" --port "$PORT" --log-level info >"$OUTPUT_DIR/server.log" 2>&1 &
SERVER_PID=$!
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    test_log "FAIL: server did not start; inspect $OUTPUT_DIR/server.log" >&2
    exit 1
fi
test_log "      Parent server PID: $SERVER_PID"

# 記錄起始秒數，然後在背景一次啟動所有 clients。
test_log "[2/5] Start $CLIENTS clients concurrently, each requesting DELAY 3."
START_TIME=$(date +%s)
CLIENT_PIDS=""
INDEX=1
while [ "$INDEX" -le "$CLIENTS" ]; do
    "$CLIENT" --port "$PORT" delay 3 \
        >"$OUTPUT_DIR/client-$INDEX.log" 2>&1 &
    CLIENT_PIDS="$CLIENT_PIDS $!"
    INDEX=$((INDEX + 1))
done

# Client 尚在 DELAY 時擷取 process snapshot，證明 child 同時存在。
test_log "[3/5] Observe the server children while all clients are waiting for replies."
sleep 1
LIVE_CHILDREN=$(count_live_children "$SERVER_PID")
{
    printf '%s\n' "This list shows the SERVER's child processes, not the client processes."
    printf '%s\n' "以下清單顯示 server 的 child processes，不是 client processes。"
    printf '%s\n' "PPID = parent server PID; PID = child PID; STATE S = sleeping/waiting."
    printf '%s\n' "PPID  STATE  PID  COMMAND"
    ps -axo ppid=,stat=,pid=,command= | \
        awk -v parent="$SERVER_PID" '$1 == parent && $2 !~ /^Z/'
} | tee "$OUTPUT_DIR/child-processes.txt" | tee -a "$TEST_LOG"

# 等待所有 client，保留任一 client 的失敗狀態。
test_log "[4/5] Wait for all $CLIENTS clients to finish and check their exit statuses."
CLIENT_FAILURE=0
for CLIENT_PID in $CLIENT_PIDS; do
    if ! wait "$CLIENT_PID"; then
        CLIENT_FAILURE=1
    fi
done
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

# 顯示報告最需要的 child 數量與總耗時。
test_log "[5/5] Verify client results, simultaneous server-child count, and elapsed time."
test_log "Observed live server child processes: $LIVE_CHILDREN"
test_log "Elapsed time: ${ELAPSED}s (serial execution would take about $((CLIENTS * 3))s)"

# 三個通過條件：client 全成功、至少 10 children、總時間不超過 8 秒。
if [ "$CLIENT_FAILURE" -ne 0 ]; then
    test_log "FAIL: at least one client failed; inspect $OUTPUT_DIR" >&2
    exit 1
fi
if [ "$LIVE_CHILDREN" -lt 10 ]; then
    test_log "FAIL: fewer than 10 live server child processes were observed." >&2
    exit 1
fi
if [ "$ELAPSED" -gt 8 ]; then
    test_log "FAIL: execution took too long to demonstrate concurrency." >&2
    exit 1
fi

test_log "PASS: at least 10 clients were handled concurrently by separate server children."
test_log "Evidence saved in: $OUTPUT_DIR"
test_log "Test progress and results: $TEST_LOG"
