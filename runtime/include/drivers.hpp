// UnifiedRootOptimizer M1 — 四大事件驱动源 (收敛图 §3.1)
// 全部只读采集，SHADOW 模式：不写任何系统节点
#pragma once
#include "event.hpp"
#include <atomic>
#include <thread>
#include <string>

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
};

// A. 前台 App：inotify top-app + 500ms 防抖 + 包名解析
void fg_driver(EventQueue& q, DriverPaths p, std::atomic<bool>& run);
// B. 模式/配置：inotify 目录（mode.txt 写入 → ModeChanged；其他配置文件 → ConfigChanged）
void mode_config_driver(EventQueue& q, DriverPaths p, std::atomic<bool>& run);
// C. 阈值采样：PSI/MemAvailable 越阈值才发 + 充电状态翻转
void sampler_driver(EventQueue& q, DriverPaths p, std::atomic<bool>& run);
// D. 屏幕状态：property 零 fork 读取（debug.tracing.screen_state，CT v4.2 同源机制）
void screen_driver(EventQueue& q, EventQueue* /*reserved*/, std::atomic<bool>& run);

uint64_t now_ms();

} // namespace uro
