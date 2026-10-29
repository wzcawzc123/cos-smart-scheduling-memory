#!/usr/bin/env python3
"""打包 URO KSU 模块 zip（KernelSU 原生格式：module.prop 顶层，无需 META-INF）

用法: python3 runtime/tools/pack_module.py [输出路径]
默认: dist/uro-<version>.zip
"""
import sys, zipfile, pathlib, re, hashlib, subprocess, datetime

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
    z.write(MOD / "uro_tactics.conf", "uro_tactics.conf")   # v0.33 场景配方外置（首次安装落地，不覆盖用户配置）
    # 调度看板（KSU WebUI）：v0.30.0 起拆成多文件 + assets
    WR = MOD / "webroot"
    for rel in ("index.html", "style.css", "app.js"):
        z.write(WR / rel, "webroot/" + rel)
    for f in sorted((WR / "assets").iterdir()):
        if f.is_file():
            z.write(f, "webroot/assets/" + f.name)
    # COSMemory 内嵌包（联动装；缺失仅降级提示不阻断）
    # v0.39：不再写死版本号 —— 取 /workspace 下版本号最高的 COSMemory_v*.zip，
    # 并排除 _fixed/_final 之类临时产物（与正式版同号，容易混）。
    def newest(pattern):
        best = None; bestkey = ()
        for p in pathlib.Path("/workspace").glob(pattern):
            if not p.is_file() or "_fixed" in p.name or "_final" in p.name:
                continue
            m = re.search(r"v(\d+(?:\.\d+)*)", p.name)
            if not m:
                continue
            key = tuple(int(x) for x in m.group(1).split("."))
            if key >= bestkey:
                best, bestkey = p, key
        return best

    cm = newest("COSMemory_v*.zip")
    if cm:
        z.write(cm, "cosmemory/" + cm.name)
        print(f"embed COSMemory: {cm.name}")
    else:
        print("WARN: 内嵌 COSMemory 包缺失 -> 联动降级")
    # AppOpt 内嵌包（GPL3.0 · 原创 SutoLiu · 仅再分发官方原版，不含修改）
    ap = pathlib.Path("/workspace/AppOpt_v2.2.6.zip")
    if ap.exists():
        z.write(ap, "appopt/AppOpt_v2.2.6.zip")
    else:
        print("WARN: 内嵌 AppOpt 包缺失 -> 联动降级")
    z.write(BIN, "bin/URORuntime")
    z.write(MOD / "bin" / "power_sampler.sh", "bin/power_sampler.sh")   # M5 电量补救采样
    # 可执行权限位（KSU 解压后按此还原）
    for n in ("service.sh", "uninstall.sh", "bin/URORuntime"):
        i = z.getinfo(n)
        i.external_attr = 0o755 << 16

    # v0.41（审查 F-12）：发布包内记录可复现信息（commit / 工具链 / 二进制哈希）
    def _sha256(fp):
        h = hashlib.sha256()
        with open(fp, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    try:
        commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                                         text=True).strip()
    except Exception:
        commit = "unknown"
    info = [
        f"version={version}",
        f"git_commit={commit}",
        f"build_utc={datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}",
        "toolchain=NDK 26.3.11579264 (clang, aarch64, android-29)",
        f"binary_sha256={_sha256(BIN)}",
        f"cosmemory={cm.name if cm else 'none'}",
        f"appopt={ap.name if ap else 'none'}",
    ]
    z.writestr("build.info", "\n".join(info) + "\n")

print(f"built {out} ({out.stat().st_size} bytes) {version}")
