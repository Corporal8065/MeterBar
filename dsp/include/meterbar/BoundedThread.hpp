#pragma once
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
namespace meterbar {
// BoundedThread (v0.20.2) — a std::thread wrapper whose stop is BOUNDED.
//
// Why this exists: WindowsLoopback::stop() used to join() the WASAPI
// capture thread with no timeout, on the message thread (via
// AudioEngine::applySource). The capture thread only observes its stop
// flag between WASAPI calls; if a call blocks inside a driver (device
// invalidation races, virtual endpoints, USB headsets) the join never
// returns and the whole UI freezes — buttons dead while the process
// lives. That was the top suspect of the v0.20.1 freeze audit.
//
// stopBounded() flips nothing itself (the worker's own stop flag is the
// caller's business); it WAITS for the worker to actually finish, up to
// the timeout:
//   - finished in time  -> join (now non-blocking) and return true:
//     the owner may free its state;
//   - still running     -> detach and return false: the owner must NOT
//     free anything the worker can touch — leak it deliberately, the
//     same leak-don't-free-under-a-live-thread pattern AudioEngine's
//     stopRecording() uses for its recorder thread (v0.12 audit).
// The finished flag is set by the wrapper AFTER the worker body returns,
// so "finished" means the body is fully done, not merely exiting.
class BoundedThread {
public:
    BoundedThread() = default;
    BoundedThread(const BoundedThread&) = delete;
    BoundedThread& operator=(const BoundedThread&) = delete;
    ~BoundedThread() { // last-resort hygiene; owners should stopBounded()
        if (th_.joinable()) th_.detach();
    }
    template <class F>
    void start(F&& fn) {
        finished_.store(false);
        th_ = std::thread([this, f = std::forward<F>(fn)]() mutable {
            f();
            finished_.store(true);
        });
    }
    bool finished() const { return finished_.load(); }
    bool joinable() const { return th_.joinable(); }
    // Wait up to timeoutMs for the worker body to finish. See the class
    // comment for the true/false contract. A never-started or already-
    // stopped thread counts as finished (true).
    bool stopBounded(int timeoutMs) {
        if (!th_.joinable()) return true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!finished_.load()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                th_.detach();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        th_.join();
        return true;
    }
private:
    std::thread th_;
    std::atomic<bool> finished_{true};
};
} // namespace meterbar
