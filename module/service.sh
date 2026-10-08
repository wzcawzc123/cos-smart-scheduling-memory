#!/system/bin/sh
# uro/service.sh — 开机自启 + watchdog（M4 阶段A）
# 职责：boot 完成后拉起 URORuntime；进程死亡 30s 内补拉。
# 崩溃残留恢复不在这里做 —— URORuntime 自身 probe 时读 bridge.state 的 DIRTY=1 并恢复基线。
MODDIR=$(dirname "$0")
BB=""
for c in /data/adb/ksu/bin/busybox /data/adb/magisk/busybox /data/adb/ap/bin/busybox; do
  [ -x "$c" ] && { BB=$c; break; }
done
[ -n "$BB" ] && export ASH_STANDALONE=1

LOGDIR=/sdcard/Android/UnifiedRootOptimizer/log
BIN="$MODDIR/bin/URORuntime"
[ -x "$BIN" ] || BIN="$MODDIR/URORuntime"
LOG="$LOGDIR/watchdog.log"
mkdir -p "$LOGDIR" 2>/dev/null

# /sdcard(FUSE) 就绪可能晚于 boot_completed（COSMemory v1.1.5 教训），等日志目录可写
i=0
while [ ! -d "$(dirname "$LOGDIR")" ] && [ "$i" -lt 30 ]; do sleep 2; i=$((i+1)); done
while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 5; done
sleep 5

# 日志滚动 512KB
if [ -f "$LOG" ]; then
  sz=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
  [ "$sz" -gt 524288 ] && mv "$LOG" "$LOG.1"
fi

[ -x "$BIN" ] || { echo "[$(date '+%F %T')] WATCHDOG halt: bin missing $BIN" >> "$LOG"; exit 1; }
echo "[$(date '+%F %T')] WATCHDOG start bin=$BIN" >> "$LOG"

while :; do
  if ! pgrep -f "$BIN" >/dev/null 2>&1; then
    echo "[$(date '+%F %T')] WATCHDOG restart (dead)" >> "$LOG"
    "$BIN" >> "$LOGDIR/uro_daemon.log" 2>&1 &
    # 首轮启动后 URORuntime 会做崩溃残留检查（bridge.state DIRTY=1 → 恢复用户基线）
  fi
  sleep 30
done
