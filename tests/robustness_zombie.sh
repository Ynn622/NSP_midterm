#!/bin/sh

# robustness_zombie.sh
# 獨立比較 Zombie process 回收 的 baseline 與 robust 行為。
# 使用方式：sh tests/robustness_zombie.sh [release|debug]

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
TEST_LOG="$OUTPUT_DIR/zombie-test.log"

. "$ROOT/tests/test_logging.sh"
init_test_log
test_log "[TEST] Robustness: reap exited server children (mode=$MODE)"
test_log "[EXPECT] Eight short connections leave zombies in baseline but none in robust."

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

# 計算 process state 以 Z 開頭的 zombie children。
count_zombie_children()
{
    ps -axo ppid=,stat= | awk -v parent="$1" \
        '$1 == parent && $2 ~ /^Z/ { count++ } END { print count + 0 }'
}

trap cleanup EXIT INT TERM

# 三個 binary 缺少任何一個，都無法進行公平比較。
if [ ! -x "$SERVER" ] || [ ! -x "$BASELINE" ] || [ ! -x "$CLIENT" ]; then
    test_log "FAIL: missing binaries. Run: make demos" >&2
    exit 1
fi
mkdir -p "$OUTPUT_DIR"
# PING client 的輸出另存，且每次測試重新開始記錄。
: >"$OUTPUT_DIR/zombie-baseline-client.log"
: >"$OUTPUT_DIR/zombie-robust-client.log"

#
# 比較三：連續建立八個短連線後觀察 process state。
# baseline 不 wait()，因此留下 zombies；robust 由 SIGCHLD handler 回收。
#
test_log "[1/5] Start the baseline server on port 19105 (no child reaper)."
start_server "$BASELINE" 19105 "$OUTPUT_DIR/zombie-baseline-server.log"
test_log "[2/5] Run 8 short PING connections, then count baseline zombie children."
INDEX=1
while [ "$INDEX" -le 8 ]; do
    if ! "$CLIENT" --port 19105 ping >>"$OUTPUT_DIR/zombie-baseline-client.log" 2>&1; then
        test_log "FAIL: baseline PING client $INDEX/8 failed; inspect $OUTPUT_DIR/zombie-baseline-client.log" >&2
        exit 1
    fi
    test_log "      Baseline PING clients completed: $INDEX/8"
    INDEX=$((INDEX + 1))
done
sleep 1
BASELINE_ZOMBIES=$(count_zombie_children "$SERVER_PID")
test_log "      Baseline zombie server children: $BASELINE_ZOMBIES"
ps -axo ppid=,stat=,pid=,command= | \
    awk -v parent="$SERVER_PID" '$1 == parent' \
    >"$OUTPUT_DIR/zombie-baseline-processes.txt"
cleanup

test_log "[3/5] Start the robust server on port 19106 (SIGCHLD child reaper enabled)."
start_server "$SERVER" 19106 "$OUTPUT_DIR/zombie-robust-server.log"
test_log "[4/5] Run the same 8 PING connections, then count robust zombie children."
INDEX=1
while [ "$INDEX" -le 8 ]; do
    if ! "$CLIENT" --port 19106 ping >>"$OUTPUT_DIR/zombie-robust-client.log" 2>&1; then
        test_log "FAIL: robust PING client $INDEX/8 failed; inspect $OUTPUT_DIR/zombie-robust-client.log" >&2
        exit 1
    fi
    test_log "      Robust PING clients completed: $INDEX/8"
    INDEX=$((INDEX + 1))
done
sleep 1
ROBUST_ZOMBIES=$(count_zombie_children "$SERVER_PID")
test_log "      Robust zombie server children: $ROBUST_ZOMBIES"
ps -axo ppid=,stat=,pid=,command= | \
    awk -v parent="$SERVER_PID" '$1 == parent' \
    >"$OUTPUT_DIR/zombie-robust-processes.txt"
cleanup

# 預期 baseline 至少一個 zombie，robust 則完全沒有。
test_log "[5/5] Compare zombie counts: baseline must have zombies; robust must have zero."
test_log "  baseline zombie children: $BASELINE_ZOMBIES"
test_log "  robust zombie children:   $ROBUST_ZOMBIES"
if [ "$BASELINE_ZOMBIES" -lt 1 ] || [ "$ROBUST_ZOMBIES" -ne 0 ]; then
    test_log "FAIL: zombie comparison did not show the expected difference." >&2
    exit 1
fi
test_log "PASS: SIGCHLD + waitpid(WNOHANG) prevented zombies."

test_log "Evidence saved in: $OUTPUT_DIR"
test_log "Test progress and results: $TEST_LOG"
