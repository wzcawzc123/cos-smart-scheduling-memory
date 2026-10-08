#!/system/bin/sh
# UnifiedRootOptimizer 安装脚本 — COSMemory 联动（架构定案 1+2 的"1"：联动不合并）
# 规范同配套 LSP 模块：失败只提示绝不 abort（主模块安装优先）。
ui_print "- UnifiedRootOptimizer v0.16+ / COSMemory 联动检查"

if [ -d /data/adb/modules/COSMemory ]; then
    ui_print "- COSMemory 已安装，跳过联装"
else
    CMZIP="$MODPATH/cosmemory/COSMemory_v1.1.5.zip"
    if [ -f "$CMZIP" ]; then
        KSU_BIN="$(command -v ksud 2>/dev/null)"
        [ -z "$KSU_BIN" ] && KSU_BIN=/data/adb/ksu/bin/ksud
        if [ -x "$KSU_BIN" ] && "$KSU_BIN" module install "$CMZIP" >/dev/null 2>&1; then
            ui_print "- COSMemory v1.1.5 已随装（重启后生效）"
        else
            ui_print "! COSMemory 自动联装失败：可手动刷入 $MODPATH/cosmemory/ 内的包"
        fi
    else
        ui_print "! 未找到内嵌 COSMemory 包（打包缺 cosmemory/），URO 独立运行（桥将降级）"
    fi
fi
