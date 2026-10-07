#!/bin/sh
# M2′ Controller/Adapter 集成测试（host 构建，无需设备 / NDK）
# 用法: sh tests/run_test.sh  ；退出码 0 = 全过
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/uro_test"
g++ -std=c++20 -Wall -Iinclude src/adapter.cpp src/controllers.cpp tests/test_m2.cpp -o "$OUT"
"$OUT"
