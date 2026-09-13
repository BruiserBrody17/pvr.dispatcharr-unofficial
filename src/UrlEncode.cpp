#include "UrlEncode.h"

#include <curl/curl.h>

namespace dispatcharr
{

std::string UrlEncode(const std::string& value)
{
  CURL* curl = curl_easy_init();
  if (!curl)
    return value;
  char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
  std::string result = escaped ? escaped : value;
  if (escaped)
    curl_free(escaped);
  curl_easy_cleanup(curl);
  return result;
}

} // namespace dispatcharr
