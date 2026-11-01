#pragma once

#include "UnicodeText.h"

#include <string>

namespace dispatcharr
{

// Mirrors Dispatcharr's own _safe_name() (apps/channels/tasks.py) filename
// sanitization -- confirmed against its real current upstream source (a
// 21st-pass audit, cloned into a scratchpad, never committed to this
// repo -- stronger than the API shape alone, not the same standard as a
// live test): strips exactly the same forbidden-filename-character set
// (backslash, forward slash, colon, asterisk, question mark, double quote,
// angle brackets, pipe) and trims leading/trailing Unicode whitespace, so
// PVRDispatcharr::GetRecordings()'s own SetDirectory(rec.title) call
// matches the single flat per-show folder Dispatcharr itself actually
// wrote the recording under.
//
// Fix for a real, confirmed bug found via a project-wide review, not
// itself independently reproduced: a title containing a forward slash
// (e.g. "Alpha/Beta", "10/12") passed straight through to SetDirectory()
// turned into a *nested* Kodi folder instead of Dispatcharr's own single
// flat one -- confirmed against Kodi's real source, CPVRRecordingsPath
// treats a raw '/' in Directory as a path separator, while only the
// title itself gets CURL::Encode()'d. The comment this replaces claimed
// rec.title was "provably identical" to Dispatcharr's own on-disk show
// folder -- true only for a title with none of these characters; this
// closes that gap instead of relying on it never coming up.
//
// Falls back to "Recording" (matching this addon's own already-existing
// "never an empty Directory" convention for rec.title itself, which
// Dispatcharr's own server-side default already guarantees is never
// literally empty) on the rare edge case where sanitizing consumes the
// whole title (e.g. a title of nothing but forbidden characters).
inline std::string SanitizeRecordingDirectory(const std::string& title)
{
  std::string result;
  result.reserve(title.size());
  for (char c : title)
  {
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      continue;
    result.push_back(c);
  }
  // _safe_name() trims with Python's str.strip(), which removes Unicode whitespace. std::isspace() trimmed only ASCII,
  // and under a C locale Kodi set from a Windows code page that counts 0xA0 as a space it cut the second byte off a
  // title ending in "à" (UTF-8 C3 A0), leaving a folder name that was not valid UTF-8 (docs/CLOSED_ITEMS.md,
  // "SanitizeRecordingDirectory trims with std::isspace").
  result = StripUnicodeWhitespace(result);
  return result.empty() ? "Recording" : result;
}

} // namespace dispatcharr
