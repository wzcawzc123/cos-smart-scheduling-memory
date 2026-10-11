#!/usr/bin/env python3
"""把内联 Vue + 画像编辑器拼进 module/webroot/index.html（单文件交付）。

用法: python3 tmp_vue/patch_index.py
  首次运行：把当前 module/webroot/index.html 存为 tmp_vue/index.html.orig_v029（原件），
            再基于原件生成新的 index.html；重复运行 = 从原件重新生成（幂等）。
输入: tmp_vue/index.html.orig_v029（原件）
      tmp_vue/{vue.global.prod.js,pf_style.css,pf_section.html,pf_app.js}
输出: module/webroot/index.html（就地覆盖）
      tmp_vue/syntax/seg*.js（供 node --check 的三段脚本）
"""
import pathlib, re

ROOT = pathlib.Path(__file__).resolve().parents[1]
TMP = pathlib.Path(__file__).resolve().parent
IDX = ROOT / "module" / "webroot" / "index.html"
ORIG = TMP / "index.html.orig_v029"

if not ORIG.exists():
    ORIG.write_text(IDX.read_text(encoding="utf-8"), encoding="utf-8")
html = ORIG.read_text(encoding="utf-8")

vue = (TMP / "vue.global.prod.js").read_text(encoding="utf-8")
css = (TMP / "pf_style.css").read_text(encoding="utf-8")
sec = (TMP / "pf_section.html").read_text(encoding="utf-8").rstrip("\n")
app = (TMP / "pf_app.js").read_text(encoding="utf-8").rstrip("\n")

assert "</script" not in vue, "vue 包内含 </script，需转义"
assert "</script" not in app, "app 脚本内含 </script，需转义"

MARK = "<!-- ===== 画像编辑器（v0.29.0 内联 Vue）===== -->"


def once(hay, needle, repl, what):
    n = hay.count(needle)
    assert n == 1, f"锚点不唯一({n}): {what}"
    return hay.replace(needle, repl, 1)


# 1) 样式：追加到既有 </style> 前
html = once(html, "</style>", "\n" + css + "</style>", "style 结束标签")

# 2) 标记段：插到页脚前
html = once(html, '<p class="foot">', MARK + "\n" + sec + "\n\n" + '<p class="foot">', "页脚")

# 3) Vue 运行时：插到看板主脚本前
html = once(html, "<script>\n// ===== 桥",
            "<script>\n/* vue.global.prod.js v3.5.43 (MIT) — 内联，KSU WebUI 本地环境无外网依赖 */\n"
            + vue + "\n</script>\n<script>\n// ===== 桥", "主脚本开始")

# 4) 应用脚本：插到 </body> 前
html = once(html, "</body>", "<script>\n" + app + "\n</script>\n</body>", "body 结束")

# 5) 页脚数据源补一行
html = html.replace("· 每 5 秒刷新", "· app_profiles.txt（画像卡按需刷新）· 每 5 秒刷新", 1)

assert html.count(MARK) == 1 and html.count("id=\"pf-app\"") == 1, "插入结果自检失败"
IDX.write_text(html, encoding="utf-8")
print(f"written {IDX} ({len(html.encode('utf-8'))} bytes)  src={ORIG.name}")

scripts = re.findall(r"<script>(.*?)</script>", html, re.S)
outdir = TMP / "syntax"
outdir.mkdir(exist_ok=True)
for i, s in enumerate(scripts):
    (outdir / f"seg{i}.js").write_text(s, encoding="utf-8")
print(f"scripts={len(scripts)} -> {outdir}")
