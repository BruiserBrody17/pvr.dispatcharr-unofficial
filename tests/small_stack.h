#pragma once

// Runs a callable on a thread with a small, fixed stack, so a test can pin "this code does not recurse per JSON
// nesting level": nlohmann's copy constructor recurses (parsing and destruction do not), and a recording whose
// custom_properties nested about 8000 deep overflowed bionic's default 1 MiB thread stack on Android
// (docs/CLOSED_ITEMS.md, "Deeply nested custom_properties could overflow the stack on Android").

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <functional>
#include <string>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace dispatcharr_test
{

// Android's default for a non-main thread; the test fails by crashing the test binary (SIGSEGV on the guard page),
// which ctest reports as a failure.
constexpr std::size_t kAndroidThreadStackBytes = 1024 * 1024;

// Deep enough to overflow any realistic stack through a per-level recursion (a copy needs about 8000 levels at
// 1 MiB, fewer under ASan, whose frames are larger), and still cheap to build: a few MB of single-element arrays.
constexpr int kDeepNestingLevels = 100000;

// `[[[...0...]]]`, `levels` arrays deep, built by the (non-recursive) parser.
inline nlohmann::json DeeplyNestedArray(int levels)
{
  std::string text(static_cast<std::size_t>(levels), '[');
  text += '0';
  text.append(static_cast<std::size_t>(levels), ']');
  return nlohmann::json::parse(text);
}

// Returns false only if the thread could not be started at that stack size; a stack overflow inside `fn` crashes
// the process instead, which is the failure the callers are there to catch. An exception from `fn` is rethrown on
// the calling thread. Keep Catch2 assertions out of `fn`: record results and check them after this returns.
inline bool RunOnSmallStack(const std::function<void()>& fn, std::size_t stackBytes = kAndroidThreadStackBytes)
{
#ifdef _WIN32
  (void)stackBytes;
  fn();
  return true;
#else
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0)
    return false;
  if (pthread_attr_setstacksize(&attr, stackBytes) != 0)
  {
    pthread_attr_destroy(&attr);
    return false;
  }
  struct Job
  {
    const std::function<void()>* fn;
    std::exception_ptr error;
  } job{&fn, nullptr};
  auto trampoline = [](void* arg) -> void*
  {
    Job* j = static_cast<Job*>(arg);
    try
    {
      (*j->fn)();
    }
    catch (...)
    {
      j->error = std::current_exception();
    }
    return nullptr;
  };
  pthread_t thread;
  const int rc = pthread_create(&thread, &attr, trampoline, &job);
  pthread_attr_destroy(&attr);
  if (rc != 0)
    return false;
  pthread_join(thread, nullptr);
  if (job.error)
    std::rethrow_exception(job.error);
  return true;
#endif
}

} // namespace dispatcharr_test
