// UnifiedRootOptimizer — 事件驱动源 (收敛图 §3.1) + M2 焦点补盲扳机
// 全部只读采集，SHADOW 模式：不写任何系统节点
#pragma once
#include "event.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <memory>

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

uint64_t now_ms();

} // namespace uro
