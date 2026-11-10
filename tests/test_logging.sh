#!/bin/sh

# test_logging.sh
# 由各測試腳本載入；把測試項目、進度與結果同步寫到終端機及測試 log。
# 呼叫前須先設定 TEST_LOG，且不影響 server/client 各自的原始 log。

init_test_log()
{
    mkdir -p "$(dirname -- "$TEST_LOG")"
    # 每次執行重新記錄，避免把不同測試的 PID 和結果混在一起。
    : >"$TEST_LOG"
}

test_log()
{
    printf '%s\n' "$*" | tee -a "$TEST_LOG"
}
