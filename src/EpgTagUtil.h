#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace dispatcharr
{

// Best-effort mapping from freeform XMLTV <category> text (Dispatcharr's own
// categories, or whatever its XMLTV EPG sources use --
// there's no fixed vocabulary) to Kodi's ETSI EN 300 468 content-mask genre
// types (kodi/c-api/addon-instance/pvr/pvr_epg.h's EPG_EVENT_CONTENTMASK_*
// constants -- duplicated in EpgTagUtil.cpp as raw hex rather than included
// directly, so this stays Kodi-SDK-independent and unit-testable standalone;
// see ../tests/test_epg_tag_util.cpp). Kodi's default skin colour-codes the
// EPG grid by genre type when it's one of these known masks rather than
// EPG_GENRE_USE_STRING, so this is what gets this addon's EPG grid looking
// like TVHeadend's rather than a flat single colour. Scans categories in
// order and returns the first keyword hit found across any of them (not
// just the first category), since Dispatcharr's XMLTV sources commonly
// list a non-genre category like "Series" before the actually-descriptive
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

// Year and FirstAired both derive from the same XMLTV <date> element, and
// both are only trustworthy when the programme also carries a real
// season/episode number -- confirmed live against a real instance: a
// long-running daily show with no season/episode identity at all carried
// the *identical* <date> value on every single airing across a week of
// distinct calendar dates, not a real "this specific episode first aired
// on X" fact -- almost certainly a series-level placeholder the guide
// source stamps on every instance rather than tracking real per-airing
// dates. GetEPGForChannel() uses this to decide whether to pass either
// through to Kodi at all, rather than surfacing that misleading placeholder
// as if it were real per-episode data.
//
// Explicit decision (2026-09-29, alongside ParseOnscreenEpisodeNum,
// XmlTvParser.h): an episode-only programme with no season (season -1,
// episode > 0, via that onscreen fallback) is treated the same as any
// other known-episode-number case here, via the existing episodeNumber >
// 0 half of this OR -- not special-cased to withhold Year/FirstAired.
// The placeholder-date problem this function exists for is specifically
// a programme with NO episode identity of its own repeating the same
// <date> across genuinely distinct airings; an onscreen-only value is a
// real per-episode number a provider chose to express without a season
// (per the XMLTV DTD, this is a legitimate, deliberate form, not a
// degraded/uncertain one), so there's no reason to believe its own
// <date> is any less trustworthy than an xmltv_ns-sourced one.
bool ShouldIncludeEpisodeDates(int seasonNumber, int episodeNumber);

// Dispatcharr's XMLTV sources commonly list a non-genre category like
// "Series" ahead of the actually-descriptive ones (also handled by
// MapCategoriesToGenreType() above) -- GetEPGForChannel() uses this
// specifically to backstop EPG_TAG_FLAG_IS_SERIES for a programme with a
// "Series" category but no season/episode number of its own.
bool CategoriesIndicateSeries(const std::vector<std::string>& categories);

// Joins non-empty category strings with the given separator, skipping the
// separator before the first entry -- the plain field-formatting half of
// GetEPGForChannel()'s GenreDescription assembly, kept separate from the
// EPG_STRING_TOKEN_SEPARATOR constant itself (a Kodi API define) so this
// stays parametrized and Kodi-SDK-independent.
std::string JoinCategories(const std::vector<std::string>& categories, const std::string& separator);

// Mirrors kodi/c-api/addon-instance/pvr/pvr_epg.h's EPG_TAG_FLAG_* values
// (kept as raw bit shifts rather than including Kodi's header directly, so
// this stays Kodi-SDK-independent -- see MapCategoriesToGenreType()'s own
// comment on the same convention). categorySaysSeries is
// CategoriesIndicateSeries()'s own result, passed in rather than
// recomputed here so this function stays pure bitmask arithmetic.
uint32_t ComputeEpgTagFlags(bool isNew, bool isPremiere, bool isLive, int seasonNumber, int episodeNumber,
                            bool categorySaysSeries);

// The pure window check behind PVRDispatcharr::IsEPGTagPlayable() --
// whether a given EPG tag's own start time still falls inside a
// catch-up-enabled channel's playable archive window. Not playable at
// all when catch-up isn't enabled; not playable when the tag hasn't
// aired yet (its start time is still in the future); not playable once
// it's aged out of the channel's own retention window. A non-positive
// catchupDays is Dispatcharr's own "archive depth unknown", not "no
// catch-up" (added 2026-09-26, a 29th-pass audit, fixing a real,
// confirmed gap found via a project-wide review, confirmed against
// Dispatcharr's own real current upstream source, not itself
// independently reproduced -- see this function's own .cpp comment) --
// falls back to a default retention window in that case rather than
// refusing outright. now is a parameter (rather than a direct
// time(nullptr) call) specifically so this is unit-testable standalone.
bool IsWithinCatchupWindow(bool catchupEnabled, int catchupDays, time_t tagStartTime, time_t now);

// Whether catch-up should actually be offered for a channel, folding in the
// two account/instance-level gates GetChannels()'s own SetHasArchive(),
// IsEPGTagPlayable()/IsWithinCatchupWindow(), and GetEPGTagStreamProperties()
// used to ignore entirely (found live 2026-09-28 -- see docs/OPEN_ITEMS.md):
// Dispatcharr's own catch-up endpoint (apps/timeshift/api_views.py) 403s
// with "Catch-up is disabled" when either system_settings.catchup_enabled
// or the current user's own custom_properties.catchup_enabled is false,
// independent of any per-channel catchupEnabled flag -- confirmed live that
// without this, Kodi kept offering "Play" for an already-aired programme
// on a perfectly catch-up-capable channel, with the only indication
// anything was wrong being a debug log line once the doomed request
// actually failed. All three call sites now compute this once per channel
// via PVRDispatcharr's own cached m_catchupEnabledGlobally/
// m_catchupEnabledForCurrentUser (fetched at startup, fail-open on a fetch
// error -- same reasoning as m_isAdmin's own comment: Dispatcharr's own
// 403 is still the authoritative enforcement either way, this is purely
// about not advertising something that's going to fail) rather than each
// re-deriving it inline.
bool ShouldOfferCatchup(bool channelCatchupEnabled, bool catchupEnabledGlobally, bool catchupEnabledForCurrentUser);

// How many days of already-aired programmes the guide fetch should ask for
// (`/output/epg?prev_days=N`), given the catch-up window of every channel that
// actually offers catch-up (ShouldOfferCatchup()). 0 -- no parameter at all -- when
// none does, so an install that never uses catch-up pays nothing.
//
// Without it the export only ever lists current and upcoming programmes, so catch-up
// worked only for whatever Kodi happened to have cached before a programme aired: a
// fresh install, a guide reset, or catch-up newly enabled on a channel had nothing
// to play (docs/CLOSED_ITEMS.md, "Guide fetch never includes already-aired
// programmes"). Measured against a real instance: prev_days=1 costs about a quarter
// more guide traffic (about 25% larger, 30 s against 24 s) and 3, 7 and 30 are
// byte-identical -- the guide source holds only about two days of history -- so the
// largest window is simply asked for and the server's own 30-day cap applied here as well.
int ComputeGuidePrevDays(const std::vector<int>& offeredCatchupDays);

} // namespace dispatcharr
