# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone test project -- deliberately NOT a subdirectory of the main
# ../CMakeLists.txt, which unconditionally does find_package(Kodi REQUIRED)
# and can only be configured through Kodi's own addon build harness (see
# ../docs/BUILDING.md). This project only covers source files with no Kodi
# SDK dependency (confirmed via each file's own #include list), so it builds
# with plain CMake -- no Kodi checkout, no addon harness, fast enough for
# ordinary CI. Run from the repo root:
#   cmake -S tests -B build-tests
#   cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure

cmake_minimum_required(VERSION 3.15)
project(pvr.dispatcharr-unofficial-tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same module-mode discovery as the main addon's own CMakeLists.txt (see its
# own comment) -- kept consistent even though this project never links a
# prebuilt Windows curl package, so the two CURL setups don't silently drift.
set(CURL_NO_CURL_CMAKE ON)
find_package(CURL REQUIRED)

include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.16.0
)
FetchContent_MakeAvailable(Catch2)

# Same pins as the main addon's own CMakeLists.txt, to avoid needing to track
# two versions of the same dependency.
FetchContent_Declare(
  pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG        v1.14
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

set(DISPATCHARR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(pvr_dispatcharr_tests
  test_time_util.cpp
  test_xmltv_parser.cpp
  test_timezone_util.cpp
  test_epg_tag_util.cpp
  test_string_util.cpp
  test_date_time_format.cpp
  test_url_encode.cpp
  test_json_field_util.cpp
  test_curl_callbacks.cpp
  test_catch_up_util.cpp
  test_recurring_rule_util.cpp
  test_recording_parser.cpp
  test_plugin_run_result.cpp
  test_realtime_update_parser.cpp
  test_m3u8_segment_parser.cpp
  test_segment_lookup.cpp
  test_channel_parser.cpp
  test_timer_rule_parser.cpp
  test_timer_identity.cpp
  ${DISPATCHARR_SRC_DIR}/XmlTvParser.cpp
  ${DISPATCHARR_SRC_DIR}/TimeZoneUtil.cpp
  ${DISPATCHARR_SRC_DIR}/EpgTagUtil.cpp
  ${DISPATCHARR_SRC_DIR}/StringUtil.cpp
  ${DISPATCHARR_SRC_DIR}/DateTimeFormat.cpp
  ${DISPATCHARR_SRC_DIR}/UrlEncode.cpp
  ${DISPATCHARR_SRC_DIR}/CurlCallbacks.cpp
  ${DISPATCHARR_SRC_DIR}/RecurringRuleUtil.cpp
  ${DISPATCHARR_SRC_DIR}/RecordingParser.cpp
  ${DISPATCHARR_SRC_DIR}/PluginRunResult.cpp
  ${DISPATCHARR_SRC_DIR}/RealtimeUpdateParser.cpp
  ${DISPATCHARR_SRC_DIR}/M3u8SegmentParser.cpp
  ${DISPATCHARR_SRC_DIR}/ChannelParser.cpp
  ${DISPATCHARR_SRC_DIR}/TimerRuleParser.cpp
)
target_include_directories(pvr_dispatcharr_tests PRIVATE ${DISPATCHARR_SRC_DIR} ${CURL_INCLUDE_DIRS})
if(TARGET CURL::libcurl)
  set(CURL_DEPLIB CURL::libcurl)
else()
  set(CURL_DEPLIB ${CURL_LIBRARIES})
endif()
target_link_libraries(pvr_dispatcharr_tests PRIVATE Catch2::Catch2WithMain pugixml::pugixml nlohmann_json::nlohmann_json ${CURL_DEPLIB})

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(CTest)
include(Catch)
catch_discover_tests(pvr_dispatcharr_tests)
