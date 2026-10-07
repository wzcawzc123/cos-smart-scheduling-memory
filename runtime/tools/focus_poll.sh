#!/system/bin/sh
# M1 影子对账真值 B：mCurrentFocus 1s 轮询（独立于 inotify/ResumedActivity 的交叉真值）
OUT=/sdcard/Android/UnifiedRootOptimizer/log/shadow_focus_run4.log
LAST=""
echo "$(date '+%F %T') focus-poll START" >> "$OUT"
while true; do
  F=$(dumpsys window 2>/dev/null | grep -m1 'mCurrentFocus=' | sed 's/.*u0 //; s/}.*//')
  if [ -n "$F" ] && [ "$F" != "$LAST" ]; then
    echo "$(date '+%F %T') focus=$F" >> "$OUT"
    LAST="$F"
  fi
  sleep 1
done
