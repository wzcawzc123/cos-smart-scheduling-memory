#!/system/bin/sh
pgrep -f power_sampler.sh >/dev/null 2>&1 || setsid sh /data/adb/modules/uro/bin/power_sampler.sh >/dev/null 2>&1 &
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
[ -f "$BIN" ] || BIN="$MODDIR/URORuntime"
# KSU 开机处理模块时会重置文件权限位（实测 bin/URORuntime 变 644 → -x 失败 → halt）。
# 不信任解压结果，启动时自行补执行位。
[ -f "$BIN" ] && chmod 755 "$BIN" 2>/dev/null
LOG="$LOGDIR/watchdog.log"
mkdir -p "$LOGDIR" 2>/dev/null

# /sdcard(FUSE) 就绪可能晚于 boot_completed（COSMemory v1.1.5 教训），等日志目录可写
i=0
while [ ! -d "$(dirname "$LOGDIR")" ] && [ "$i" -lt 30 ]; do sleep 2; i=$((i+1)); done
while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 5; done
sleep 5

# v0.33 场景配方：首次开机兜底落地（install 阶段 /sdcard 未就绪时补做；不覆盖用户配置）
TACTICS="/sdcard/Android/UnifiedRootOptimizer/uro_tactics.conf"
if [ ! -f "$TACTICS" ] && [ -f "$MODDIR/uro_tactics.conf" ]; then
  cp "$MODDIR/uro_tactics.conf" "$TACTICS" 2>/dev/null && \
    echo "[$(date '+%F %T')] tactics conf seeded -> $TACTICS" >> "$LOG"
fi

# 日志滚动 512KB
if [ -f "$LOG" ]; then
  sz=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
  [ "$sz" -gt 524288 ] && mv "$LOG" "$LOG.1"
fi

# 注意：不使用 exit 1 —— cp 覆盖二进制存在"被截断窗口"（实测致 watchdog 自杀两轮）；
# 缺失/无执行位时记日志并等待下一轮重试（自愈）。
if [ ! -f "$BIN" ]; then
  echo "[$(date '+%F %T')] WATCHDOG warn: bin missing (will retry) $BIN" >> "$LOG"
fi
[ -f "$BIN" ] && chmod 755 "$BIN" 2>/dev/null
echo "[$(date '+%F %T')] WATCHDOG start bin=$BIN" >> "$LOG"

CRASH_N=0        # 时间窗内连续崩溃次数（v0.39 熔断）
CRASH_T0=0       # 窗口起点
lastStart=0      # 上次拉起时间（用于判定"稳定存活"）
DL="$LOGDIR/uro_daemon.log"
while :; do
  now=$(date +%s)
  if ! pgrep -f "$BIN" >/dev/null 2>&1; then
    # 熔断：10 分钟内连续崩 5 次 → 退避 5 分钟，避免崩溃循环持续烧电/刷日志
    if [ "$CRASH_N" -ge 5 ] && [ $((now - CRASH_T0)) -lt 600 ]; then
      echo "[$(date '+%F %T')] WATCHDOG circuit-open: $CRASH_N crashes in $((now - CRASH_T0))s -> backoff 300s" >> "$LOG"
      sleep 300
      CRASH_N=0; CRASH_T0=0
      continue
    fi
    if [ "$CRASH_T0" -eq 0 ] || [ $((now - CRASH_T0)) -ge 600 ]; then
      CRASH_N=0; CRASH_T0=$now
    fi
    CRASH_N=$((CRASH_N + 1))
    # 守护日志滚动（512KB）：常驻循环里也要滚，否则长期运行无限增长
    if [ -f "$LOG" ]; then
      sz=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
      [ "$sz" -gt 524288 ] && mv "$LOG" "$LOG.1"
    fi
    if [ -f "$DL" ]; then
      dsz=$(stat -c %s "$DL" 2>/dev/null || echo 0)
      [ "$dsz" -gt 2097152 ] && mv "$DL" "$DL.1"
    fi
    echo "[$(date '+%F %T')] WATCHDOG restart (dead, #$CRASH_N in window)" >> "$LOG"
    "$BIN" >> "$DL" 2>&1 &
    lastStart=$now
    # 首轮启动后 URORuntime 会做崩溃残留检查（bridge.state DIRTY=1 → 恢复用户基线）
  elif [ "$lastStart" -gt 0 ] && [ $((now - lastStart)) -ge 180 ]; then
    # 稳定存活 3 分钟 → 清零熔断计数
    CRASH_N=0; CRASH_T0=0; lastStart=0
    echo "[$(date '+%F %T')] WATCHDOG stable (uptime>=180s), crash counter reset" >> "$LOG"
  fi
  # v0.41（审查 F-05）：把恢复快照同步到模块目录，卸载时即便 /sdcard 不可用也能归还
  mkdir -p "$MODDIR/state" 2>/dev/null
  for f in bridge.state cpu.state; do
    [ -f "$LOGDIR/$f" ] && cp -f "$LOGDIR/$f" "$MODDIR/state/$f" 2>/dev/null
  done
  sleep 30
done
