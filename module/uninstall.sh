#!/system/bin/sh
# uro/uninstall.sh — 卸载兜底（M4：改过别人的配置必须归还）
# 1) 若 COSMemory 配置处于被 URO 改偏离的状态 → 恢复基线
# 2) 删除 enforce 开关文件（模块没了，开关无意义）
# 注意：卸载时 /data/adb 可用；不依赖 /sdcard 上的模块数据
STATE=/sdcard/Android/UnifiedRootOptimizer/log/bridge.state
CMOS=/data/adb/modules/COSMemory/config/memory.json

if [ -f "$STATE" ] && [ -f "$CMOS" ]; then
  BASE=$(grep '^BASELINE=' "$STATE" 2>/dev/null | head -1 | cut -d= -f2)
  DIRTY=$(grep '^DIRTY=' "$STATE" 2>/dev/null | head -1 | cut -d= -f2)
  if [ "$DIRTY" = "true" ] && [ -n "$BASE" ]; then
    sed -i "/\"reclaim\"/,/}/s/\"aggressive\": \(true\|false\)/\"aggressive\": $BASE/" "$CMOS"
    sed -i "s/^DIRTY=.*/DIRTY=false/" "$STATE" 2>/dev/null
  fi
fi
rm -f /sdcard/Android/UnifiedRootOptimizer/uro.conf 2>/dev/null
