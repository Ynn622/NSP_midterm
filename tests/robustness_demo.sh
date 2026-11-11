#!/bin/sh

# robustness_demo.sh
# 一次執行三個獨立的 robustness 對照腳本；任一測試失敗便停止。
# 使用方式：sh tests/robustness_demo.sh [release|debug]
set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$TEST_DIR/.." && pwd)
MODE=${1:-release}
TEST_LOG="$ROOT/build/$MODE/test-output/robustness/suite.log"

. "$TEST_DIR/test_logging.sh"
init_test_log
test_log "[SUITE] Robustness: 3 comparisons (mode=$MODE)"

# 保留子腳本的失敗狀態，並在 suite log 記錄失敗的測試項目。
run_test()
{
    test_log "$1"
    test_command "sh $TEST_DIR/$2 $MODE"
    if sh "$TEST_DIR/$2" "$MODE"; then
        test_pass "$3"
    else
        TEST_STATUS=$?
        test_log "[FAIL] $3 (exit status=$TEST_STATUS)" >&2
        exit "$TEST_STATUS"
    fi
}

run_test "[1/3] Test TCP request fragmentation." robustness_fragment.sh "TCP request fragmentation"
run_test "[2/3] Test timeout when a client sends no request." robustness_timeout.sh "No-request timeout"
run_test "[3/3] Test zombie-process reaping." robustness_zombie.sh "Zombie-process reaping"

test_pass "All three robustness comparisons passed (3/3)."
test_file "Suite progress and results: $TEST_LOG"
