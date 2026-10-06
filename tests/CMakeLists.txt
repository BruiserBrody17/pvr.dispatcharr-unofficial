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
  GIT_TAG        317ac1ed4c0bb6e6b91eafc817e05c488feffcb3 # v3.16.0
)
FetchContent_MakeAvailable(Catch2)

# Same pins as the main addon's own CMakeLists.txt, to avoid needing to track
# two versions of the same dependency.
FetchContent_Declare(
  pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG        db78afc2b7d8f043b4bc6b185635d949ea2ed2a8 # v1.14
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 # v3.11.3
)
FetchContent_MakeAvailable(json)

set(DISPATCHARR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(pvr_dispatcharr_tests
  test_time_util.cpp
  test_xmltv_parser.cpp
  test_timezone_util.cpp
  test_epg_tag_util.cpp
  test_redirect_policy.cpp
  test_sha1.cpp
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
  test_json_response.cpp
  test_bounded_parallel.cpp
  test_m3u8_segment_parser.cpp
  test_segment_lookup.cpp
  test_channel_parser.cpp
  test_channel_number.cpp
  test_timer_rule_parser.cpp
  test_timer_identity.cpp
  test_live_manifest_parser.cpp
  test_live_edge_margin.cpp
  test_recurring_rule_edit.cpp
  test_socket_wait.cpp
  test_web_socket_client.cpp
  test_managed_recurring_rule.cpp
  test_dvr_access.cpp
  test_recording_epg_links.cpp
  test_unprobeable_segment.cpp
  test_recurring_rule_renewal.cpp
  test_web_socket_frame.cpp
  test_series_rule_matching.cpp
  test_series_rule_text_match.cpp
  test_unicode_text.cpp
  test_recurring_rule_weekdays.cpp
  test_channel_group_filter.cpp
  test_web_socket_handshake.cpp
  test_auth_backoff.cpp
  test_stream_seek.cpp
  test_segment_append_offsets.cpp
  test_pending_title_lookup.cpp
  test_recording_visibility.cpp
  test_staleness.cpp
  test_request_timeout.cpp
  test_server_offset_cross_check.cpp
  test_refresh_events.cpp
  test_plugin_url_util.cpp
  test_stream_property_util.cpp
  test_recording_http_util.cpp
  test_recording_directory.cpp
  test_channel_renumbering.cpp
  test_channel_lineup_change.cpp
  test_api_key_recovery.cpp
  test_in_progress_segment_cache.cpp
  test_epg_program_match.cpp
  test_timer_request_builder.cpp
  test_segment_fetch_failure.cpp
  test_hls_viewer_keep_alive.cpp
  test_catchup_session_cache.cpp
  test_catchup_session_request.cpp
  test_padding_push.cpp
  ${DISPATCHARR_SRC_DIR}/XmlTvParser.cpp
  ${DISPATCHARR_SRC_DIR}/TimeZoneUtil.cpp
  ${DISPATCHARR_SRC_DIR}/EpgTagUtil.cpp
  ${DISPATCHARR_SRC_DIR}/RedirectPolicy.cpp
  ${DISPATCHARR_SRC_DIR}/Sha1.cpp
  ${DISPATCHARR_SRC_DIR}/SocketWait.cpp
  ${DISPATCHARR_SRC_DIR}/WebSocketClient.cpp
  ${DISPATCHARR_SRC_DIR}/StringUtil.cpp
  ${DISPATCHARR_SRC_DIR}/DateTimeFormat.cpp
  ${DISPATCHARR_SRC_DIR}/UrlEncode.cpp
  ${DISPATCHARR_SRC_DIR}/CurlCallbacks.cpp
  ${DISPATCHARR_SRC_DIR}/RecurringRuleUtil.cpp
  ${DISPATCHARR_SRC_DIR}/RefreshEvents.cpp
  ${DISPATCHARR_SRC_DIR}/ServerOffsetCrossCheck.cpp
  ${DISPATCHARR_SRC_DIR}/RecordingParser.cpp
  ${DISPATCHARR_SRC_DIR}/PluginRunResult.cpp
  ${DISPATCHARR_SRC_DIR}/ApiKeyRecovery.cpp
  ${DISPATCHARR_SRC_DIR}/RealtimeUpdateParser.cpp
  ${DISPATCHARR_SRC_DIR}/JsonResponse.cpp
  ${DISPATCHARR_SRC_DIR}/BoundedParallel.cpp
  ${DISPATCHARR_SRC_DIR}/M3u8SegmentParser.cpp
  ${DISPATCHARR_SRC_DIR}/UnprobeableSegment.cpp
  ${DISPATCHARR_SRC_DIR}/ChannelParser.cpp
  ${DISPATCHARR_SRC_DIR}/ChannelNumber.cpp
  ${DISPATCHARR_SRC_DIR}/TimerRuleParser.cpp
  ${DISPATCHARR_SRC_DIR}/LiveManifestParser.cpp
  ${DISPATCHARR_SRC_DIR}/RecurringRuleRenewal.cpp
  ${DISPATCHARR_SRC_DIR}/RecurringRuleEdit.cpp
  ${DISPATCHARR_SRC_DIR}/ManagedRecurringRule.cpp
  ${DISPATCHARR_SRC_DIR}/DvrAccess.cpp
  ${DISPATCHARR_SRC_DIR}/RecordingEpgLinks.cpp
  ${DISPATCHARR_SRC_DIR}/WebSocketFrame.cpp
  ${DISPATCHARR_SRC_DIR}/SeriesRuleMatching.cpp
  ${DISPATCHARR_SRC_DIR}/SeriesRuleTextMatch.cpp
  ${DISPATCHARR_SRC_DIR}/UnicodeText.cpp
  ${DISPATCHARR_SRC_DIR}/AuthBackoff.cpp
  ${DISPATCHARR_SRC_DIR}/StreamSeek.cpp
  ${DISPATCHARR_SRC_DIR}/RecordingVisibility.cpp
  ${DISPATCHARR_SRC_DIR}/PluginUrlUtil.cpp
  ${DISPATCHARR_SRC_DIR}/StreamPropertyUtil.cpp
  ${DISPATCHARR_SRC_DIR}/RecordingHttpUtil.cpp
  ${DISPATCHARR_SRC_DIR}/EpgProgramMatch.cpp
  ${DISPATCHARR_SRC_DIR}/TimerRequestBuilder.cpp
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
# PRE_TEST: the tests are listed when ctest starts, not by running the freshly linked executable as a build step. A
# ThreadSanitizer build (the unit-tests-tsan job) cannot always start on a CI runner's address-space layout, and the
# listing run then failed the whole build before the job's own retrying run loop was reached.
catch_discover_tests(pvr_dispatcharr_tests DISCOVERY_MODE PRE_TEST)
