// UnifiedRootOptimizer — 事件驱动源 (收敛图 §3.1) + M2 焦点补盲扳机
// 全部只读采集，SHADOW 模式：不写任何系统节点
#pragma once
#include "event.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <memory>
#include <vector>
#include <utility>

namespace uro {

struct DriverPaths {
    std::string topAppCpuset = "/dev/cpuset/top-app/cgroup.procs";
    std::string modeFile     = "/sdcard/Android/UnifiedRootOptimizer/mode.txt";
    std::string uroDir       = "/sdcard/Android/UnifiedRootOptimizer";
    std::string cosmDir      = "/sdcard/Android/COSMemory";   // 名单/配置热变更
    int psiThresholdTenx     = 50;   // psi avg10 >= 5.0 (十分位整数)
    long memFloorMb          = 1024;
    int  pressureCooldownSec = 60;
    int  pollIntervalMs      = 5000;
    int  focusPollMs         = 1000; // 焦点巡检间隔（dumpsys window 单次实测 14-20ms）
};

// 前台状态共享：inotify 与 focus 两扳机在此去重，谁先到谁发事件
struct FgShared {
    std::mutex m;
    std::string pkg;                       // last-known 前台包名
    bool publish(const std::string& p) {   // true = 新状态，调用方应发事件
        std::lock_guard<std::mutex> lk(m);
        if (p == pkg) return false;
        pkg = p;
        return true;
    }
    std::string get() {
        std::lock_guard<std::mutex> lk(m);
        return pkg;
    }
};
using FgSharedPtr = std::shared_ptr<FgShared>;

// A. 前台 App（主扳机）：inotify top-app + 300ms 防抖 + ResumedActivity 解析
void fg_driver(EventQueue& q, DriverPaths p, FgSharedPtr fg, std::atomic<bool>& run);
// A'. 前台 App（补盲扳机）：mCurrentFocus 低频巡检 —— 补 inotify 的 systemui 盲区
void focus_driver(EventQueue& q, DriverPaths p, FgSharedPtr fg, std::atomic<bool>& run);
// B. 模式/配置：inotify 目录（mode.txt 写入 → ModeChanged；其他配置文件 → ConfigChanged）
void mode_config_driver(EventQueue& q, DriverPaths p, std::atomic<bool>& run);
// C. 阈值采样：PSI/MemAvailable 越阈值才发 + 充电状态翻转
void sampler_driver(EventQueue& q, DriverPaths p, std::atomic<bool>& run);
// D. 屏幕状态：property 零 fork 读取（debug.tracing.screen_state，CT v4.2 同源机制）
void screen_driver(EventQueue& q, EventQueue* /*reserved*/, std::atomic<bool>& run);
// 第2步·智能感知：触摸屏 BTN_TOUCH 边沿（down/up → TouchChanged；边沿即节流）
void touch_driver(EventQueue& q, std::atomic<bool>& run);

// ThreadController Phase-1（AppOpt 路线 a：约束源+Evidence+豁免校验+URO-GEN框架）
struct AppOptSummary {
    bool present = false;                    // conf 与 cpuset 至少其一在
    int rules = 0;                           // 活跃规则行数
    std::vector<std::pair<std::string,int>> groups;  // cpuset 组 -> 成员数
    std::string gameViolation;               // 非空 = 游戏包被活跃规则覆盖（告警）
};

AppOptSummary read_appopt(const std::string& confPath, const std::string& gamePath,
                          const std::string& cpusetRoot);
void ensure_uro_gen_block(const std::string& confPath);
// 指挥链①：GAME 自动豁免（注释活跃游戏规则+[URO-EXEMPT]标记，幂等；出GAME按标记还原）
int exempt_game_rules(const std::string& confPath, const std::string& gameListPath);
int restore_exempted(const std::string& confPath);
// 指挥链②：画像第四字段(pattern|down|up|cpuset)对账生成 URO-GEN 规则（幂等；通配跳过）
int reconcile_uro_gen(const std::string& confPath, const std::string& profilesPath);
void thread_evidence_driver(const std::string& logdir, const std::string& appoptConf,
                            const std::string& gamePath, const std::string& cpusetRoot,
                            std::atomic<bool>& run);

// Evidence 扩展（v0.17：温度/FPS 只读采样，纯逻辑在 evidence.cpp，host 可测）
std::string thermal_json(const std::string& thermalRoot);
std::string battery_json(const std::string& psRoot);
bool parse_gfxinfo(const std::string& text, long& total, long& janky);
int read_max_temp(const std::string& thermalRoot);
std::string gpu_json(const std::string& kgslRoot);
void read_battery_state(const std::string& psRoot, int& pct, bool& charging);
bool fps_sentry_trigger(const int* jankPctHist, int n, int threshPct);
int next_thermal_level(int prev, int t, int t1, int t2, int hyst, int trendDeg = 0);
void evidence_driver(const std::string& logdir, FgSharedPtr fg, EventQueue& q, std::atomic<bool>& run);

uint64_t now_ms();

} // namespace uro
