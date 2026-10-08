#!/usr/bin/env python3
# Phase-1 重组：把 append 的段落拆成 appopt.cpp(纯逻辑,host可测) + drivers.cpp(线程)
import io
base = '/workspace/cos-sched-memory/runtime/'
p_drv = base + 'src/drivers.cpp'
s = open(p_drv, encoding='utf-8').read()

mark = '// ---------- ThreadController Phase-1'
idx = s.find(mark)
assert idx > 0, 'mark'
head, tail = s[:idx], s[idx:]
dmark = '// 驱动：60s 低频 Evidence 轮询'
didx = tail.find(dmark)
assert didx > 0, 'dmark'
pure, drv = tail[:didx], tail[didx:]

# struct 段剪给 hpp（从 struct AppOptSummary 到其结束的 };）
sstart = pure.find('struct AppOptSummary')
send = pure.find('};', sstart) + 3
struct_src = pure[sstart:send].rstrip() + '\n'
pure_rest = pure[:sstart] + pure[send:]
pure_rest = pure_rest.split('static std::string trim_local', 1)[1]
pure_rest = 'static std::string trim_local' + pure_rest.rstrip() + '\n'

# 1) drivers.cpp：补 dirent + driver 段进 namespace（} // namespace uro 之前）
head = head.replace('#include <sys/inotify.h>',
                     '#include <sys/inotify.h>\n#include <dirent.h>', 1)
close = head.rfind('} // namespace uro')
assert close > 0, 'ns close'
new_drv = head[:close] + drv.rstrip() + '\n\n' + head[close:]
open(p_drv, 'w', encoding='utf-8').write(new_drv)

# 2) appopt.cpp：纯逻辑
appopt = '''// ThreadController Phase-1 纯逻辑（AppOpt 路线 a）——不依赖 Android 头，host 可测。
// 编译入 host 套件（run_test.sh）与 NDK 主构建。
#include "drivers.hpp"
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <vector>

namespace uro {

''' + pure_rest + '''
} // namespace uro
'''
open(base + 'src/appopt.cpp', 'w', encoding='utf-8').write(appopt)

# 3) drivers.hpp：struct + 声明（插在 touch_driver 声明后）
p_hpp = base + 'include/drivers.hpp'
h = open(p_hpp, encoding='utf-8').read()
anchor = 'void touch_driver(EventQueue& q, std::atomic<bool>& run);'
assert anchor in h
decl = anchor + '''

// ThreadController Phase-1（AppOpt 路线 a：约束源+Evidence+豁免校验+URO-GEN框架）
''' + struct_src + '''
AppOptSummary read_appopt(const std::string& confPath, const std::string& gamePath,
                          const std::string& cpusetRoot);
void ensure_uro_gen_block(const std::string& confPath);
void thread_evidence_driver(const std::string& logdir, const std::string& appoptConf,
                            const std::string& gamePath, const std::string& cpusetRoot,
                            std::atomic<bool>& run);'''
h = h.replace(anchor, decl, 1)
open(p_hpp, 'w', encoding='utf-8').write(h)

# 4) main.cpp 接线 t7（t6 行后）
p_main = base + 'src/main.cpp'
m = open(p_main, encoding='utf-8').read()
a6 = 'std::thread t6(touch_driver, std::ref(q), std::ref(g_run));'
assert a6 in m
line6_end = m.find('\n', m.find(a6))
m = m[:line6_end + 1] + '''    std::thread t7(thread_evidence_driver, std::string(logdir),
                   "/data/adb/modules/AppOpt/applist.conf",
                   std::string(logdir) + "/game_apps.txt",
                   "/dev/cpuset/AppOpt", std::ref(g_run));   // Phase-1: Thread Evidence
'''
# join（t6.join() 后）
a6j = 't6.join();'
assert a6j in m
j_end = m.find('\n', m.find(a6j))
m = m[:j_end + 1] + '    t7.join();\n'
open(p_main, 'w', encoding='utf-8').write(m)

print('drivers', len(new_drv.encode()), 'appopt', len(appopt.encode()), 'hpp OK main OK')
