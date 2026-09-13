#include "DateTimeFormat.h"

#include "TimeUtil.h"

#include <cstdio>
#include <stdexcept>

namespace dispatcharr
{

std::string IsoFromTime(time_t t)
{
  char buf[32];
  tm tmVal{};
  GmTimeUtc(t, &tmVal);
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmVal);
  return std::string(buf);
}

time_t TimeFromIso(const std::string& isoStr)
{
  if (isoStr.size() < 19)
    return 0;

  tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(isoStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(isoStr.substr(5, 2)) - 1;
    tmVal.tm_mday = std::stoi(isoStr.substr(8, 2));
    tmVal.tm_hour = std::stoi(isoStr.substr(11, 2));
    tmVal.tm_min = std::stoi(isoStr.substr(14, 2));
    tmVal.tm_sec = std::stoi(isoStr.substr(17, 2));
  }
  catch (const std::exception&)
  {
    return 0;
  }
  return PortableTimeGm(&tmVal);
}

std::string TimeOfDayString(int secondsSinceMidnight)
{
  int s = secondsSinceMidnight % 86400;
  if (s < 0)
    s += 86400;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  return std::string(buf);
}

int SecondsSinceMidnightFromString(const std::string& hms)
{
  int h = 0, m = 0, s = 0;
  if (std::sscanf(hms.c_str(), "%d:%d:%d", &h, &m, &s) < 2)
    return 0;
  return h * 3600 + m * 60 + s;
}

std::string DateStringFromTime(time_t t)
{
  char buf[16];
  tm tmVal{};
  GmTimeUtc(t, &tmVal);
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmVal);
  return std::string(buf);
}

time_t TimeFromDateString(const std::string& dateStr)
{
  if (dateStr.size() < 10)
    return 0;
  tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(dateStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(dateStr.substr(5, 2)) - 1;
    tmVal.tm_mday = std::stoi(dateStr.substr(8, 2));
  }
  catch (const std::exception&)
  {
    return 0;
  }
  return PortableTimeGm(&tmVal);
}

} // namespace dispatcharr
