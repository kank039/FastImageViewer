#pragma once
// Fixed worker pool + disk gate.
//
// Workers are created once and live for the whole session (no thread per
// image). Thread 0 is an express lane that only runs work for the image on
// screen, so the current image never waits behind prefetching.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "Win.h"

struct IWICImagingFactory;

namespace fiv {

struct WorkerContext {
    IWICImagingFactory* wic = nullptr;  // owned by the worker thread
};

using CancelFlag = std::atomic<bool>;

// Limits concurrent disk reads (1 on HDD/network so the head never thrashes).
// Waiters are served by priority (lower first); a long read can YieldTo() a
// more urgent waiter between chunks.
class IoGate {
public:
    void SetSlots(int n);
    bool Acquire(int priority, const CancelFlag& cancel);  // false if cancelled
    void Release();
    bool HasMoreUrgentWaiter(int priority);

private:
    std::mutex m_;
    std::condition_variable cv_;
    int slots_ = 2;
    int busy_ = 0;
    std::multiset<int> waiters_;
};

class IoGateLock {
public:
    IoGateLock(IoGate& g, int priority, const CancelFlag& c) : g_(g), held_(g.Acquire(priority, c)) {}
    ~IoGateLock() {
        if (held_) g_.Release();
    }
    bool held() const { return held_; }
    // Lets a more urgent reader go first (called between read chunks).
    void YieldTo(int priority, const CancelFlag& c) {
        if (held_ && g_.HasMoreUrgentWaiter(priority)) {
            g_.Release();
            held_ = g_.Acquire(priority, c);
        }
    }

private:
    IoGate& g_;
    bool held_;
};

class JobSystem {
public:
    using Fn = std::function<void(WorkerContext&, const CancelFlag&)>;

    // threads >= 2: one express thread + (threads - 1) general threads.
    void Start(int threads);
    void Stop();  // cancels everything and joins

    // Lower priority value runs first. Priority <= kExpressPriority may use the express thread.
    void Submit(uint64_t id, int priority, Fn fn);
    void Cancel(uint64_t id);
    void CancelAll();
    void SetPriority(uint64_t id, int priority);
    void WaitIdle();
    int ThreadCount() const { return int(threads_.size()); }

    static constexpr int kExpressPriority = 1;

private:
    struct Job {
        uint64_t id;
        int priority;
        Fn fn;
        std::shared_ptr<CancelFlag> cancel;
    };
    void Worker(bool express);
    bool PopFor(bool express, Job& out);

    std::mutex m_;
    std::condition_variable cv_, idleCv_;
    std::vector<Job> queue_;
    std::map<uint64_t, std::shared_ptr<CancelFlag>> running_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

// Lowers I/O, memory and CPU priority (and requests efficiency cores via
// EcoQoS) for prefetch work; restores on destruction.
class BackgroundMode {
public:
    explicit BackgroundMode(bool enable) { Set(enable); }
    ~BackgroundMode() { Set(false); }
    void Set(bool enable);  // switchable mid-job: a prefetch may become the image on screen

private:
    bool active_ = false;
};

}  // namespace fiv
