# 可由 command line 覆寫編譯器，例如：make CC=gcc。
CC ?= cc
# DEBUG 只有 1 代表開啟；其他值一律正規化為 release 的 0。
DEBUG ?= 0
DEBUG := $(if $(filter 1,$(DEBUG)),1,0)

# 偵測專案絕對路徑與作業系統，供跨平台建置規則使用。
PROJECT_ROOT := $(abspath .)
UNAME_S := $(shell uname -s)

# Debug/release 分開輸出，避免切換編譯旗標時誤用舊 object file。
ifeq ($(DEBUG),1)
BUILD_MODE := debug
OPT_FLAGS := -O0 -g3
DEBUG_FLAGS := -DENABLE_DEBUG
else
BUILD_MODE := release
OPT_FLAGS := -O2
DEBUG_FLAGS :=
endif

# 所有產物集中於 build/<mode>/，原始碼目錄保持乾淨。
BUILD_DIR := build/$(BUILD_MODE)
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib
TEST_OUTPUT_DIR := $(BUILD_DIR)/test-output

# 使用 C11、POSIX API、常用警告；ENABLE_DEBUG 控制編譯期 debug log。
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L $(DEBUG_FLAGS)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic $(OPT_FLAGS)

#
# macOS 與 Linux 的 shared library 格式及 rpath 語法不同：
# - macOS：Mach-O dynamic library，執行時以 @loader_path 找到 ../lib。
# - Linux：ELF shared object，執行時以 $ORIGIN 找到 ../lib。
# 兩邊的輸出檔名都依作業要求命名為 libnetutil.so。
#
ifeq ($(UNAME_S),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
SHARED_FLAGS := -dynamiclib -Wl,-install_name,@rpath/libnetutil.so
APP_RPATH := -Wl,-rpath,@loader_path/../lib
else
CPPFLAGS += -D_DEFAULT_SOURCE
SHARED_FLAGS := -shared -Wl,-soname,libnetutil.so
APP_RPATH := -Wl,-rpath,'$$ORIGIN/../lib'
endif

# 列出 shared library 與三個 executable 所需的 object files。
LIB_OBJECTS := $(OBJ_DIR)/netutil.o $(OBJ_DIR)/protocol.o
SERVER_OBJECT := $(OBJ_DIR)/server.o
BASELINE_OBJECT := $(OBJ_DIR)/server_baseline.o
CLIENT_OBJECT := $(OBJ_DIR)/client.o

SHARED_LIBRARY := $(LIB_DIR)/libnetutil.so
SERVER := $(BIN_DIR)/server
BASELINE_SERVER := $(BIN_DIR)/server_baseline
CLIENT := $(BIN_DIR)/client

# 這些 target 是命令名稱，不代表同名實體檔案。
.PHONY: all release debug baseline demos test concurrency-test robustness-test \
	fragment-test timeout-test zombie-test print-config clean

# 預設只建置正式 server 與 client。
all: $(SERVER) $(CLIENT)

# release/debug 額外建置 baseline，方便執行 robustness 對照。
release:
	$(MAKE) DEBUG=0 all baseline

debug:
	$(MAKE) DEBUG=1 all baseline

baseline: $(BASELINE_SERVER)

demos: all baseline

# 完整測試包含 10-client concurrency 與三組 robustness 比較。
test: all baseline
	@echo "[TEST SUITE 1/2] Concurrent clients"
	$(MAKE) concurrency-test
	@echo "[TEST SUITE 2/2] Robustness comparisons"
	$(MAKE) robustness-test
	@echo "🟢 PASS: All test suites completed (2/2)."

concurrency-test: all
	sh tests/concurrency_test.sh $(BUILD_MODE)

robustness-test: all baseline
	sh tests/robustness_demo.sh $(BUILD_MODE)

# 三組 robustness 比較也能分別建置並執行，方便單獨展示。
fragment-test: all baseline
	sh tests/robustness_fragment.sh $(BUILD_MODE)

timeout-test: all baseline
	sh tests/robustness_timeout.sh $(BUILD_MODE)

zombie-test: all baseline
	sh tests/robustness_zombie.sh $(BUILD_MODE)

# 顯示目前實際採用的 OS、compiler、debug 與輸出目錄。
print-config:
	@echo "OS=$(UNAME_S)"
	@echo "CC=$(CC)"
	@echo "DEBUG=$(DEBUG)"
	@echo "BUILD_DIR=$(BUILD_DIR)"

# 將共用網路與 protocol objects 連結成動態函式庫。
$(SHARED_LIBRARY): $(LIB_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(SHARED_FLAGS) -o $@ $(LIB_OBJECTS)

# Server、baseline server、client 都連結同一個 libnetutil.so。
$(SERVER): $(SERVER_OBJECT) $(SHARED_LIBRARY)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(SERVER_OBJECT) $(SHARED_LIBRARY) $(APP_RPATH)

$(BASELINE_SERVER): $(BASELINE_OBJECT) $(SHARED_LIBRARY)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(BASELINE_OBJECT) $(SHARED_LIBRARY) $(APP_RPATH)

$(CLIENT): $(CLIENT_OBJECT) $(SHARED_LIBRARY)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(CLIENT_OBJECT) $(SHARED_LIBRARY) $(APP_RPATH)

# Shared library 的 objects 使用 -fPIC，讓程式碼可被動態載入。
$(OBJ_DIR)/netutil.o: src/netutil.c include/netutil.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(OBJ_DIR)/protocol.o: src/protocol.c include/protocol.h include/netutil.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(SERVER_OBJECT): src/server.c include/netutil.h include/protocol.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# 同一份 server.c 加上 BASELINE_MODE，產生缺少指定保護的對照版本。
$(BASELINE_OBJECT): src/server.c include/netutil.h include/protocol.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBASELINE_MODE=1 -c $< -o $@

$(CLIENT_OBJECT): src/client.c include/netutil.h include/protocol.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# 僅移除本專案產生的 build 目錄，不碰原始碼與文件。
clean:
	rm -rf build
