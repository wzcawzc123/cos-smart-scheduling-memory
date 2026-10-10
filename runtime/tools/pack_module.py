#!/usr/bin/env python3
"""打包 URO KSU 模块 zip（KernelSU 原生格式：module.prop 顶层，无需 META-INF）

用法: python3 runtime/tools/pack_module.py [输出路径]
默认: dist/uro-<version>.zip
"""
import sys, zipfile, pathlib, re

ROOT = pathlib.Path(__file__).resolve().parents[2]   # tools -> runtime -> 仓库根
MOD = ROOT / "module"
BIN = ROOT / "runtime" / "output" / "URORuntime"

prop = (MOD / "module.prop").read_text(encoding="utf-8")
version = re.search(r"^version=(.+)$", prop, re.M).group(1).strip()

out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist" / f"uro-{version}.zip"
out.parent.mkdir(parents=True, exist_ok=True)

if not BIN.is_file():
    sys.exit(f"missing binary: {BIN} (run build.sh first)")

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(MOD / "module.prop", "module.prop")
    z.write(MOD / "service.sh", "service.sh")
    z.write(MOD / "uninstall.sh", "uninstall.sh")
    z.write(MOD / "customize.sh", "customize.sh")   # COSMemory 联动（架构定案 1+2 的"1"）
    z.write(MOD / "webroot" / "index.html", "webroot/index.html")   # 调度看板（KSU WebUI）
    # COSMemory 内嵌包（联动装；缺失仅降级提示不阻断）
    cm = pathlib.Path("/workspace/COSMemory_v1.2.1.zip")
    if cm.exists():
        z.write(cm, "cosmemory/COSMemory_v1.2.1.zip")
    else:
        print("WARN: 内嵌 COSMemory 包缺失 -> 联动降级")
    z.write(BIN, "bin/URORuntime")
    z.write(MOD / "bin" / "power_sampler.sh", "bin/power_sampler.sh")   # M5 电量补救采样
    # 可执行权限位（KSU 解压后按此还原）
    for n in ("service.sh", "uninstall.sh", "bin/URORuntime"):
        i = z.getinfo(n)
        i.external_attr = 0o755 << 16

print(f"built {out} ({out.stat().st_size} bytes) v{version}")
