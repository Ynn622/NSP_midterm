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

# 在真正執行外部程式前顯示完整命令，讓終端機截圖能直接證明測試步驟。
test_command()
{
    DISPLAY_TEXT=$*
    # 腳本仍使用絕對路徑執行；終端機只顯示以專案根目錄為基準的短路徑。
    if [ -n "${ROOT:-}" ]; then
        DISPLAY_TEXT=$(printf '%s\n' "$DISPLAY_TEXT" | sed "s|$ROOT|.|g")
    fi
    test_log "▶ COMMAND: $DISPLAY_TEXT"
}

# 統一測試成功訊息的圖示與格式。
test_pass()
{
    test_log "🟢 PASS: $*"
}

# 統一標示測試證據、輸出目錄與 log 檔案。
test_file()
{
    DISPLAY_TEXT=$*
    if [ -n "${ROOT:-}" ]; then
        DISPLAY_TEXT=$(printf '%s\n' "$DISPLAY_TEXT" | sed "s|$ROOT|.|g")
    fi
    test_log "📃 $DISPLAY_TEXT"
}
