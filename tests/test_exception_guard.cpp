#include "ExceptionGuard.h"

#include <new>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
template <typename F> std::string DescribeWhatThrows(F&& f)
{
  try
  {
    f();
  }
  catch (...)
  {
    return DescribeCurrentException();
  }
  return "nothing thrown";
}
} // namespace

TEST_CASE("A std::exception is described by its own message", "[ExceptionGuard]")
{
  CHECK(DescribeWhatThrows([] { throw std::runtime_error("boom"); }) == "boom");
  CHECK(DescribeWhatThrows([] { throw std::out_of_range("index 3"); }) == "index 3");
}

TEST_CASE("A std::bad_alloc is described without allocating", "[ExceptionGuard]")
{
  // The case the guard exists for: copying the recordings or rules on a low-memory box. Only checked non-empty, since
  // what() is the implementation's own text.
  const std::string text = DescribeWhatThrows([] { throw std::bad_alloc(); });
  CHECK_FALSE(text.empty());
  CHECK(text != "nothing thrown");
}

TEST_CASE("Something not derived from std::exception gets a fixed description", "[ExceptionGuard]")
{
  CHECK(DescribeWhatThrows([] { throw 42; }) == "an exception not derived from std::exception");
  CHECK(DescribeWhatThrows([] { throw "text"; }) == "an exception not derived from std::exception");
}

TEST_CASE("The description stays valid while the handler runs", "[ExceptionGuard]")
{
  // e.what() points into the exception object, which the rethrow inside DescribeCurrentException() does not copy.
  try
  {
    throw std::runtime_error(std::string(200, 'x'));
  }
  catch (...)
  {
    const char* text = DescribeCurrentException();
    CHECK(std::string(text) == std::string(200, 'x'));
  }
}
