#!/bin/sh
# UnifiedRootOptimizer 全量测试套件（host 构建，无需设备/NDK）
# 用法: sh tests/run_test.sh [all|bridge|m2|m3]
#   all      三套全跑（默认）
#   bridge   桥接/崩溃恢复/多字段/uag/画像外的控制器行为
#   m2       Controller/Adapter 集成
#   m3       PolicyManager（场景/四档/滞回/画像/生成）
# 退出码 0 = 全过。断言数汇总输出，任一 FAIL 非零退出。
set -e
cd "$(dirname "$0")/.."
TARGET="${1:-all}"
OUT="${TMPDIR:-/tmp}/uro_ut"
FAILED=0
TOTAL_P=0
TOTAL_F=0

run_one() {
    name="$1"
    if [ "$TARGET" != "all" ] && [ "$TARGET" != "$name" ]; then return 0; fi
    echo "== test_$name =="
    if ! g++ -std=c++20 -Wall -Iinclude src/adapter.cpp src/controllers.cpp src/appopt.cpp \
            "tests/test_$name.cpp" -o "$OUT.$name" 2>"$OUT.$name.cc.err"; then
        echo "   COMPILE FAIL:"
        grep -m5 error "$OUT.$name.cc.err" | sed 's/^/   /'
        FAILED=1
        return 0
    fi
    set +e
    result="$("$OUT.$name" 2>&1)"
    rc=$?
    set -e
    echo "$result" | sed 's/^/   /'
    # 解析 "结果: N passed, M failed"
    p=$(echo "$result" | sed -n 's/.*结果: \([0-9]*\) passed.*/\1/p' | tail -1)
    f=$(echo "$result" | sed -n 's/.*结果: [0-9]* passed, \([0-9]*\) failed.*/\1/p' | tail -1)
    [ -n "$p" ] && TOTAL_P=$((TOTAL_P + p))
    [ -n "$f" ] && TOTAL_F=$((TOTAL_F + f))
    if [ "$rc" -ne 0 ] || [ "${f:-0}" -ne 0 ]; then FAILED=1; fi
}

run_one bridge
run_one m2
run_one m3

echo "=================================="
if [ "$FAILED" -eq 0 ]; then
    echo "ALL PASS — $TOTAL_P assertions, 0 failed"
    exit 0
else
    echo "FAILED — $TOTAL_P passed, $TOTAL_F failed"
    exit 1
fi
