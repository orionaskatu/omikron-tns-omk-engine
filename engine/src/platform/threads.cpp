// SPDX-License-Identifier: GPL-3.0-or-later
// `omk::Threads`' shared half - the pool every platform has - and the two
// helpers built on a single detached thread, `FileFetch` and `BackgroundJob`.
// The three implementations of the pool are in their own files, compiled only
// where they apply (`threads.h` says why they are here and not at the call
// sites); the shape is the same in all three: `parallelFor` cuts
// `[begin, end)` into at most `runners()` contiguous chunks of at least
// `grain` items, hands all but the last to the workers, runs the last itself,
// and waits. What differs is how a worker is made and how it is woken:
//
//   * `threads_std.cpp`: `std::thread` workers on a condition variable;
//   * `threads_vita.cpp`: kernel threads, semaphores, an explicit stack and
//     priority;
//   * `threads_none.cpp`: no threads - the body runs once on the caller's.
#include "platform/threads.h"
#include "platform/datafs.h"
#include "platform/hostthread.h"

#include <atomic>
#include <memory>

namespace omk {

Threads& Threads::shared() {
    static Threads one;
    return one;
}

// ---------------------------------------------------------------- FileFetch
struct FileFetch::State {
    std::string path;
    std::vector<std::byte> bytes;
    std::atomic<bool> done{false};
    void run() {
        bytes = DataFs::readPath(path);
        done.store(true, std::memory_order_release);
    }
};

FileFetch::FileFetch(std::string path)
    : path_(std::move(path)), state_(std::make_shared<State>()) {
    state_->path = path_;
    // the thread holds a reference to the state until it is done
    if (!hostthread::spawnDetached("omk_fetch", 64 * 1024, [s = state_] { s->run(); }))
        state_->run();                       // no thread: read it now
}

FileFetch::~FileFetch() = default;

bool FileFetch::ready() const {
    return state_ && state_->done.load(std::memory_order_acquire);
}

std::vector<std::byte> FileFetch::take() {
    if (!state_) return {};
    while (!state_->done.load(std::memory_order_acquire)) hostthread::sleepMs(1);
    std::vector<std::byte> out = std::move(state_->bytes);
    state_.reset();
    return out;
}

// ------------------------------------------------------------ BackgroundJob
struct BackgroundJob::State {
    std::function<void()> work;
    std::atomic<bool> done{false};
    bool threaded = false;
    void run() {
        work();
        work = nullptr;                      // what it captured goes with it
        done.store(true, std::memory_order_release);
    }
};

BackgroundJob::BackgroundJob(std::function<void()> work)
    : state_(std::make_shared<State>()) {
    state_->work = std::move(work);
    // a megabyte of stack where the host takes a size: the work is a loader,
    // not a read. `threaded` is set before the thread starts - only this
    // side reads it.
    state_->threaded = true;
    if (!hostthread::spawnDetached("omk_job", 1024 * 1024, [s = state_] { s->run(); })) {
        state_->threaded = false;
        state_->run();                       // no thread: do it now
    }
}

BackgroundJob::~BackgroundJob() = default;

bool BackgroundJob::ready() const {
    return state_->done.load(std::memory_order_acquire);
}

bool BackgroundJob::threaded() const { return state_->threaded; }

void BackgroundJob::wait() {
    while (!state_->done.load(std::memory_order_acquire)) hostthread::sleepMs(1);
}

}  // namespace omk
