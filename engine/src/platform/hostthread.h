// SPDX-License-Identifier: GPL-3.0-or-later
// THE HOST'S THREADS for `FileFetch` and `BackgroundJob` (`threads.cpp`): a
// single thread nobody joins, and a sleep. One implementation per platform,
// beside that platform's `Threads` pool (`threads_std.cpp`, `threads_vita.cpp`,
// `threads_none.cpp`). Internal: not part of `threads.h`.
#pragma once

#include <cstddef>
#include <functional>

namespace omk::hostthread {

// `fn` run on a new thread of its own, `stackBytes` deep where the host takes
// a size -> false when no thread could be made (the caller then runs it).
bool spawnDetached(const char* name, std::size_t stackBytes, std::function<void()> fn);
void sleepMs(int ms);

}  // namespace omk::hostthread
