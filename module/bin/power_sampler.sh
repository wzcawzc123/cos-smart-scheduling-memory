#!/system/bin/sh
# M5 电量补救采样（只读 sysfs，零干预；A 段 4f05 无电量字段期间的独立分母）
# service.sh 幂等拉起（pgrep -f）；60s 粒度，充电/放电段由分析脚本区分
L=/sdcard/Android/UnifiedRootOptimizer/log
P=/sys/class/power_supply/battery
while true; do
    pct=$(cat $P/capacity 2>/dev/null)
    st=$(cat $P/status 2>/dev/null)
    ma=$(cat $P/current_now 2>/dev/null)
    ts=$(date +%s%3N 2>/dev/null || date +%s000)
    [ -n "$pct" ] && echo "{\"ts\":$ts,\"pct\":$pct,\"chg\":\"$st\",\"mA\":$ma}" >> $L/power.jsonl
    sleep 60
done
