#!/system/bin/sh
# uro/uninstall.sh — 卸载兜底（M4：改过别人的配置必须归还）
# 1) 若 COSMemory 配置处于被 URO 改偏离的状态 → 恢复基线
# 2) 删除 enforce 开关文件（模块没了，开关无意义）
# 注意：卸载时 /data/adb 可用；不依赖 /sdcard 上的模块数据
# v0.41（审查 F-05）：优先用模块目录内的 state 快照（service.sh 每 30s 同步），
# /sdcard 不可用时仍能恢复；两处都读不到则显式告警，不再静默跳过。
MODDIR=$(dirname "$0")
STATE_SD=/sdcard/Android/UnifiedRootOptimizer/log/bridge.state
STATE_MOD="$MODDIR/state/bridge.state"
STATE=""
for c in "$STATE_MOD" "$STATE_SD"; do [ -f "$c" ] && { STATE=$c; break; }; done
CMOS=/data/adb/modules/COSMemory/config/memory.json

if [ -z "$STATE" ]; then
  echo "[uro] WARN: bridge.state 未找到（已查 $STATE_MOD 与 $STATE_SD）" >&2
  echo "[uro] WARN: COSMemory 配置可能未被恢复；URO 自身 CPU/线程改动由运行时 shutdown 负责" >&2
fi

if [ -n "$STATE" ] && [ -f "$STATE" ] && [ -f "$CMOS" ]; then
  BASE=$(grep '^BASELINE=' "$STATE" 2>/dev/null | head -1 | cut -d= -f2)
  DIRTY=$(grep '^DIRTY=' "$STATE" 2>/dev/null | head -1 | cut -d= -f2)
  if [ "$DIRTY" = "true" ] && [ -n "$BASE" ]; then
    sed -i "/\"reclaim\"/,/}/s/\"aggressive\": \(true\|false\)/\"aggressive\": $BASE/" "$CMOS"
    sed -i "s/^DIRTY=.*/DIRTY=false/" "$STATE" 2>/dev/null
    # 回读确认（审查 F-05）：恢复失败要留下可见的记录
    if [ -n "$BASE" ] && ! grep -q "\"aggressive\": $BASE" "$CMOS" 2>/dev/null; then
      echo "[uro] WARN: COSMemory aggressive 未确认恢复到 $BASE" >&2
    fi
  fi
fi
rm -f /sdcard/Android/UnifiedRootOptimizer/uro.conf 2>/dev/null
