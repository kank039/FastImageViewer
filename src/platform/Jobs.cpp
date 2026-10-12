#include "Jobs.h"

#include <wincodec.h>

#include <chrono>

namespace fiv {

// ---- IoGate -----------------------------------------------------------------

void IoGate::SetSlots(int n) {
    std::lock_guard lk(m_);
    slots_ = n < 1 ? 1 : n;
    cv_.notify_all();
}

bool IoGate::Acquire(int priority, const CancelFlag& cancel) {
    std::unique_lock lk(m_);
    auto me = waiters_.insert(priority);
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) {
            waiters_.erase(me);
            cv_.notify_all();
            return false;
        }
        if (busy_ < slots_ && priority <= *waiters_.begin()) {
            waiters_.erase(me);
            ++busy_;
            return true;
        }
        // Timed so a cancel is noticed promptly without extra plumbing.
        cv_.wait_for(lk, std::chrono::milliseconds(20));
    }
}

void IoGate::Release() {
    std::lock_guard lk(m_);
    --busy_;
    cv_.notify_all();
}

bool IoGate::HasMoreUrgentWaiter(int priority) {
    std::lock_guard lk(m_);
    return !waiters_.empty() && *waiters_.begin() < priority;
}

// ---- JobSystem --------------------------------------------------------------

void JobSystem::Start(int threads) {
    if (threads < 2) threads = 2;
    stop_ = false;
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this, i] { Worker(i == 0); });
}

void JobSystem::Stop() {
    {
        std::lock_guard lk(m_);
        stop_ = true;
        for (auto& j : queue_) j.cancel->store(true);
        for (auto& [id, c] : running_) c->store(true);
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    running_.clear();
}

void JobSystem::Submit(uint64_t id, int priority, Fn fn) {
    {
        std::lock_guard lk(m_);
        queue_.push_back({id, priority, std::move(fn), std::make_shared<CancelFlag>(false)});
    }
    cv_.notify_all();
}

void JobSystem::Cancel(uint64_t id) {
    {
        std::lock_guard lk(m_);
        for (auto& j : queue_) {
            if (j.id == id) {
                j.cancel->store(true);
                j.priority = -1;  // run immediately: it returns at once and frees its accounting
            }
        }
        auto it = running_.find(id);
        if (it != running_.end()) it->second->store(true);
    }
    cv_.notify_all();
}

void JobSystem::CancelAll() {
    {
        std::lock_guard lk(m_);
        for (auto& j : queue_) {
            j.cancel->store(true);
            j.priority = -1;
        }
        for (auto& [id, c] : running_) c->store(true);
    }
    cv_.notify_all();
}

void JobSystem::SetPriority(uint64_t id, int priority) {
    std::lock_guard lk(m_);
    for (auto& j : queue_)
        if (j.id == id && !j.cancel->load()) j.priority = priority;
}

void JobSystem::WaitIdle() {
    std::unique_lock lk(m_);
    idleCv_.wait(lk, [&] { return stop_ || (queue_.empty() && running_.empty()); });
}

bool JobSystem::PopFor(bool express, Job& out) {
    std::unique_lock lk(m_);
    for (;;) {
        if (stop_) return false;
        int best = -1;
        for (int i = 0; i < int(queue_.size()); ++i) {
            if (express && queue_[size_t(i)].priority > kExpressPriority) continue;
            if (best < 0 || queue_[size_t(i)].priority < queue_[size_t(best)].priority) best = i;
        }
        if (best >= 0) {
            out = std::move(queue_[size_t(best)]);
            queue_.erase(queue_.begin() + best);
            running_[out.id] = out.cancel;
            return true;
        }
        cv_.wait(lk);
    }
}

void JobSystem::Worker(bool express) {
    ComApartment com(COINIT_MULTITHREADED);
    {  // ComPtr scope: released before CoUninitialize
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        WorkerContext ctx;
        ctx.wic = wic.Get();
        Job job;
        while (PopFor(express, job)) {
            job.fn(ctx, *job.cancel);
            job.fn = nullptr;
            std::lock_guard lk(m_);
            running_.erase(job.id);
            if (queue_.empty() && running_.empty()) idleCv_.notify_all();
        }
    }
}

// ---- BackgroundMode ---------------------------------------------------------

// THREAD_POWER_THROTTLING_STATE, spelled out so older SDK headers compile too.
struct PowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
constexpr ULONG kPowerThrottlingVersion = 1;
constexpr ULONG kPowerThrottlingExecutionSpeed = 0x1;

static void SetEcoQoS(bool on) {
    PowerThrottlingState s{kPowerThrottlingVersion, kPowerThrottlingExecutionSpeed,
                           on ? kPowerThrottlingExecutionSpeed : 0};
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &s, sizeof s);  // no-op before Win10 1709
}

BackgroundMode::BackgroundMode(bool enable) {
    if (!enable) return;
    active_ = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
    SetEcoQoS(true);
}

BackgroundMode::~BackgroundMode() {
    if (active_) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        SetEcoQoS(false);
    }
}

}  // namespace fiv
