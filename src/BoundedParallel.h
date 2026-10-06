#pragma once

#include <cstddef>
#include <functional>
#include <thread>

namespace dispatcharr
{

// Runs task(i) for every i in [0, count), at most `batchSize` at a time, and returns once all have
// finished -- the fan-out RefreshInProgressRecordingManifest() uses to size a long playlist's segments.
//
// Found by the 2026-10-04 second hardening sweep: the inline version built each batch with
// std::vector<std::thread>::emplace_back(), and a thread that cannot be created (EAGAIN under a thread or
// memory limit, plausible on a low-RAM CoreELEC or Android box) throws std::system_error out of it. The
// unwinding vector then destroyed the threads already started while they were still joinable, which is
// std::terminate for the whole Kodi process. Here a failed creation joins what did start and runs that
// index and the rest of the batch on the calling thread instead, so the work always gets done, only slower.
// An exception out of a task is swallowed (that index simply keeps whatever the caller pre-filled it with,
// "unknown" for a probe) because an exception leaving a std::thread body is std::terminate too.
//
// `spawn` is how a thread is made, a parameter only so a test can make creation fail on demand.
using ThreadSpawner = std::function<std::thread(std::function<void()>)>;

void RunInBoundedBatches(size_t count, size_t batchSize, const std::function<void(size_t)>& task,
                         const ThreadSpawner& spawn = ThreadSpawner());

} // namespace dispatcharr
