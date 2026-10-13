// SPDX-License-Identifier: GPL-3.0-or-later
// A LOCK AND A THREAD ID THAT COST NOTHING WITHOUT THREADS: `std::mutex`,
// `std::lock_guard` and `std::thread::id` where `OMK_THREADS` is on, and empty
// stand-ins where it is off (classic Mac OS, the 3DS - no thread model at all).
// So code that guards shared state writes the guard once, without an
// `#if OMK_THREADS` around every use.
#pragma once

#include "platform/threads.h"   // OMK_THREADS

#if OMK_THREADS
#include <mutex>
#include <thread>
namespace omk {
using Mutex = std::mutex;
using ScopedLock = std::lock_guard<std::mutex>;
using ThreadId = std::thread::id;
inline ThreadId thisThread() { return std::this_thread::get_id(); }
}  // namespace omk
#else
namespace omk {
struct Mutex {};
struct ScopedLock {
    explicit ScopedLock(Mutex&) {}
};
// one thread: every id is the same id
struct ThreadId {
    bool operator==(const ThreadId&) const { return true; }
    bool operator!=(const ThreadId&) const { return false; }
};
inline ThreadId thisThread() { return {}; }
}  // namespace omk
#endif
