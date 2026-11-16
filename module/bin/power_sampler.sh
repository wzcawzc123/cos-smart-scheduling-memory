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
    # v0.41（审查 F-10）：1MB 轮转，避免长期运行无限增长
    if [ -f "$L/power.jsonl" ]; then
        sz=$(stat -c %s "$L/power.jsonl" 2>/dev/null || echo 0)
        [ "$sz" -gt 1048576 ] && mv -f "$L/power.jsonl" "$L/power.jsonl.1"
    fi
    # v0.41.2（复审 P3-01）：数字字段做校验，读不到用 null（原来会输出非法 JSON）；
    # 字段改名 current_now_raw —— 不同内核单位不同，不假定是 mA
    if [ -n "$pct" ]; then
        case "$ma" in ''|*[!0-9-]*) ma_json=null ;; *) ma_json=$ma ;; esac
        case "$pct" in ''|*[!0-9]*) pct_json=null ;; *) pct_json=$pct ;; esac
        echo "{\"ts\":$ts,\"pct\":$pct_json,\"chg\":\"$st\",\"current_now_raw\":$ma_json}" >> $L/power.jsonl
    fi
    sleep 60
done
