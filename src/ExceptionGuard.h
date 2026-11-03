#pragma once

#include <exception>

namespace dispatcharr
{

// What the exception being handled says about itself, for the log line written when one is caught at the boundary
// with Kodi. Call only inside a catch handler. Returns e.what() for a std::exception (valid while that handler runs,
// since a rethrow does not copy the exception) and a fixed text for anything else. Allocates nothing, so it is safe in
// a handler for std::bad_alloc.
//
// Why there is a boundary to catch at: Kodi calls the PVR overrides through the dev-kit's C thunks, and neither those
// nor CPVRClient::DoAddonCall() catch anything, so an exception leaving an override (a std::bad_alloc copying the
// recordings or rules on a 32-bit or low-memory box) ends the Kodi process, and on Windows crosses a CRT boundary
// first (docs/CLOSED_ITEMS.md, "Nothing stops an exception crossing from a PVR callback into Kodi").
inline const char* DescribeCurrentException() noexcept
{
  try
  {
    throw;
  }
  catch (const std::exception& e)
  {
    return e.what();
  }
  catch (...)
  {
    return "an exception not derived from std::exception";
  }
}

} // namespace dispatcharr
