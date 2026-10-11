#!/system/bin/sh
# URO 原子部署脚本（2026-10-11 立，防 cp 截断窗口）
#
# 背景：旧流程 kill → cp 覆盖 bin → chmod 存在"文件被截断"窗口，
#       watchdog 恰在窗口内循环会看到 -x/缺失（旧 service.sh 会 exit 1 自杀，已改自愈）。
# 本脚本用 同目录 tmp + mv（rename 原子）消除窗口：
#   kill → cp 到 $BIN.new → chmod 755 → mv -f（原子替换）→ 等 watchdog 拉起 → 验证
#
# 用法：deploy.sh <源二进制路径>      例：sh deploy.sh /sdcard/uro_new_bin
set -u
BIN=/data/adb/modules/uro/bin/URORuntime
SRC="${1:-}"
if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
  echo "[deploy] usage: deploy.sh <src_bin>（当前: '$SRC' 不存在）"
  exit 1
fi

echo "[deploy] kill old runtime..."
pkill -9 -f "$BIN" 2>/dev/null
sleep 2

echo "[deploy] atomic replace..."
cp "$SRC" "$BIN.new" || { echo "[deploy] FAIL: cp 到 tmp 失败"; exit 1; }
chmod 755 "$BIN.new" || { echo "[deploy] FAIL: chmod 失败"; exit 1; }
mv -f "$BIN.new" "$BIN" || { echo "[deploy] FAIL: mv 失败"; exit 1; }
rm -f "$SRC"
MD5=$(md5sum "$BIN" | awk '{print $1}')
echo "[deploy] deployed md5=$MD5"

echo "[deploy] waiting watchdog (max 150s)..."
i=0
while [ "$i" -lt 15 ]; do
  sleep 10
  i=$((i+1))
  if pgrep -f "$BIN" >/dev/null 2>&1; then
    echo "[deploy] OK: runtime running (after ${i}0s)"
    exit 0
  fi
done
echo "[deploy] WARN: runtime not up in 150s — 检查 watchdog.log / uro_daemon.log"
exit 2
