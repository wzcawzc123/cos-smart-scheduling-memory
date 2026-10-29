#!/system/bin/sh
# UnifiedRootOptimizer 安装脚本 — COSMemory 联动（架构定案 1+2 的"1"：联动不合并）
# 规范同配套 LSP 模块：失败只提示绝不 abort（主模块安装优先）。
ui_print "- UnifiedRootOptimizer v0.16+ / COSMemory 联动检查"

if [ -d /data/adb/modules/COSMemory ]; then
    ui_print "- COSMemory 已安装，跳过联装"
else
    CMZIP=$(ls "$MODPATH"/cosmemory/COSMemory_v*.zip 2>/dev/null | head -1)
    if [ -n "$CMZIP" ] && [ -f "$CMZIP" ]; then
        KSU_BIN="$(command -v ksud 2>/dev/null)"
        [ -z "$KSU_BIN" ] && KSU_BIN=/data/adb/ksu/bin/ksud
        if [ -x "$KSU_BIN" ] && "$KSU_BIN" module install "$CMZIP" >/dev/null 2>&1; then
            ui_print "- COSMemory $(basename "$CMZIP" .zip) 已随装（重启后生效）"
        else
            ui_print "! COSMemory 自动联装失败：可手动刷入 $MODPATH/cosmemory/ 内的包"
        fi
    else
        ui_print "! 未找到内嵌 COSMemory 包（打包缺 cosmemory/），URO 独立运行（桥将降级）"
    fi
fi

# ── AppOpt（线程优化）联动 ─────────────────────────────────────────────
# 版权与合规：AppOpt 原创 SutoLiu（酷安/GitHub），GPL-3.0 开源；
# 本模块仅再分发其官方原版 zip（未作任何修改），保留署名与许可，源码与原仓库：
# https://github.com/SutoLiu/AppOpt  ·  https://gitee.com/sutoliu/AppOptR
# 与 COSMemory 同规则：已装则跳过（尊重用户自装版本），缺失/失败仅提示不 abort。
if [ -d /data/adb/modules/AppOpt ]; then
    ui_print "- AppOpt 已安装，跳过联装"
else
    APZIP="$MODPATH/appopt/AppOpt_v2.2.6.zip"
    if [ -f "$APZIP" ]; then
        KSU_BIN="$(command -v ksud 2>/dev/null)"
        [ -z "$KSU_BIN" ] && KSU_BIN=/data/adb/ksu/bin/ksud
        if [ -x "$KSU_BIN" ] && "$KSU_BIN" module install "$APZIP" >/dev/null 2>&1; then
            ui_print "- AppOpt v2.2.6 已随装（原作者 SutoLiu · GPL3.0 · 重启后生效）"
        else
            ui_print "! AppOpt 自动联装失败：可手动刷入 $MODPATH/appopt/ 内的官方包"
        fi
    else
        ui_print "! 未找到内嵌 AppOpt 包（打包缺 appopt/），URO 的线程约束源将降级"
    fi
fi

# ── v0.33 场景配方外置：默认配置落地 ─────────────────────────────────
# 已存在则保留（尊重用户已改的配置）；失败只提示不 abort（service.sh 开机后还有一次兜底）
TACTICS="/sdcard/Android/UnifiedRootOptimizer/uro_tactics.conf"
if [ -f "$TACTICS" ]; then
    ui_print "- uro_tactics.conf 已存在，保留现有配置"
else
    mkdir -p /sdcard/Android/UnifiedRootOptimizer 2>/dev/null
    if cp "$MODPATH/uro_tactics.conf" "$TACTICS" 2>/dev/null; then
        ui_print "- uro_tactics.conf 默认配置已落地（改完保存即热生效）"
    else
        ui_print "! uro_tactics.conf 落地失败（/sdcard 未就绪？）——开机后自动重试，运行时暂用内置默认"
    fi
fi
