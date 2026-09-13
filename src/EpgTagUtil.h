#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace dispatcharr
{

// Best-effort mapping from freeform XMLTV <category> text (Dispatcharr's own
// categories, or whatever its Schedules Direct/XMLTV EPG sources use --
// there's no fixed vocabulary) to Kodi's ETSI EN 300 468 content-mask genre
// types (kodi/c-api/addon-instance/pvr/pvr_epg.h's EPG_EVENT_CONTENTMASK_*
// constants -- duplicated in EpgTagUtil.cpp as raw hex rather than included
// directly, so this stays Kodi-SDK-independent and unit-testable standalone;
// see ../tests/test_epg_tag_util.cpp). Kodi's default skin colour-codes the
// EPG grid by genre type when it's one of these known masks rather than
// EPG_GENRE_USE_STRING, so this is what gets this addon's EPG grid looking
// like TVHeadend's rather than a flat single colour. Scans categories in
// order and returns the first keyword hit found across any of them (not
// just the first category), since Dispatcharr/Schedules Direct commonly
// lists a non-genre category like "Series" before the actually-descriptive
// ones.
bool MapCategoriesToGenreType(const std::vector<std::string>& categories, int& genreType);

// Broadcast ids only need to be unique per channel/addon, not globally;
// combining channel id with the *full* start time (not just its low bits)
// is what actually keeps this stable across refreshes without colliding.
// Found via a project-wide review, not a live-observed mis-tagged
// recording: an earlier version XORed in only `startTime & 0xFFFF`, so any
// two entries on the *same* channel whose start times differed by an exact
// multiple of 65536 seconds (~18.2h) produced the identical id -- a real,
// plausible collision across a multi-day guide with a few hundred entries
// per channel (birthday-paradox math puts a meaningful chance of at least
// one such collision per channel well within a typical guide window,
// compounding across an entire lineup). Multiplying channelUid by a large
// odd constant (Knuth's multiplicative hash constant) rather than shifting
// it also avoids the same class of truncation once a channel id exceeds 16
// bits.
uint32_t ComputeBroadcastId(int channelUid, time_t startTime);

} // namespace dispatcharr
