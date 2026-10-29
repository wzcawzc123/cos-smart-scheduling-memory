// UnifiedRootOptimizer M1 — Event/Queue 契约 (设计文档 v1.1 §4.1)
#pragma once
#include <cstdint>
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>

namespace uro {

enum class EventType : uint8_t {
    ForegroundChanged, ScreenChanged, TouchChanged,
    MemoryPressureChanged, ThermalChanged, ChargerChanged,
    ConfigChanged, ModeChanged, ProcessDeath, ControllerFault
};

inline const char* etype_name(EventType t) {
    switch (t) {
        case EventType::ForegroundChanged:    return "ForegroundChanged";
        case EventType::ScreenChanged:        return "ScreenChanged";
        case EventType::TouchChanged:         return "TouchChanged";
        case EventType::MemoryPressureChanged:return "MemoryPressureChanged";
        case EventType::ThermalChanged:       return "ThermalChanged";
        case EventType::ChargerChanged:       return "ChargerChanged";
        case EventType::ConfigChanged:        return "ConfigChanged";
        case EventType::ModeChanged:          return "ModeChanged";
        case EventType::ProcessDeath:         return "ProcessDeath";
        case EventType::ControllerFault:      return "ControllerFault";
    }
    return "Unknown";
}

struct Event {
    EventType type;
    uint64_t  ts_ms;        // event time (epoch ms)
    uint64_t  generation;   // assigned by StateManager on commit
    std::string source;     // driver tag, e.g. "fg-inotify"
    std::string payload;
};

// 单消费者事件队列（多生产者 driver 线程 push，Runtime 独占 pop）
class EventQueue {
public:
    // v0.41（审查 F-11）：容量上限。消费者卡顿或传感器抖动时不无限堆积，
    // 超限丢最旧事件并计数（状态类事件本来就是"最新值有效"语义）。
    static constexpr size_t kMaxQueued = 2048;
    void push(Event e) {
        {
            std::lock_guard<std::mutex> lk(m_);
            while (q_.size() >= kMaxQueued) { q_.pop_front(); ++dropped_; }
            q_.push_back(std::move(e));
        }
        cv_.notify_one();
    }
    size_t dropped() { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    // 阻塞取，超时返回 false；stop 后返回 false 且不再阻塞
    bool pop(Event& out, int timeout_ms = 500) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                          [&]{ return stop_ || !q_.empty(); }))
            return false;
        if (q_.empty()) return false;
        out = std::move(q_.front()); q_.pop_front();
        return true;
    }
    void stop() { { std::lock_guard<std::mutex> lk(m_); stop_ = true; } cv_.notify_all(); }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return q_.size(); }

private:
    std::deque<Event> q_;
    size_t dropped_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace uro
