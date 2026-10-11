#!/bin/sh
# 用生产解析器自检 uro_tactics.conf（host 运行，不依赖设备）
# 用法: sh runtime/tools/check_tactics.sh [配置文件路径]
#   默认检查 module/uro_tactics.conf；退出码 0 = 配置合法
# 用途：改完配方先自检再上机，避免"上机才发现整份被拒"（spec §4 整份拒绝语义）
set -e
cd "$(dirname "$0")/../.."
CONF="${1:-module/uro_tactics.conf}"
TMP="${TMPDIR:-/tmp}/uro_chk_$$"

cat > "$TMP.cpp" <<'EOF'
#include "tactics_conf.hpp"
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    std::ifstream f(argv[1]);
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto c = uro::parse_tactics_conf(t);
    std::cout << (c.ok ? "OK" : "REJECT")
              << " applied=" << c.applied
              << " scenarios=" << c.byScenario.size();
    if (!c.ok) std::cout << " line=" << c.errorLine << " err=" << c.error;
    std::cout << "\n";
    return c.ok ? 0 : 1;
}
EOF

g++ -std=c++20 -I runtime/include "$TMP.cpp" -o "$TMP.bin"
set +e
"$TMP.bin" "$CONF"
rc=$?
set -e
rm -f "$TMP.cpp" "$TMP.bin"
exit $rc
