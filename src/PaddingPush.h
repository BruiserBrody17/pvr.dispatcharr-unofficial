#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace dispatcharr
{

// One DVR padding value together with the sequence number of the user edit it
// came from (see BeginPaddingEdit()).
struct PaddingEdit
{
  int value = 0;
  uint64_t seq = 0;
};

// Bookkeeping for a DVR padding edit (recording_pre/post_offset_minutes). Dispatcharr's padding is
// one global value written through a GET-merge-PATCH, and the pushes are made from detached threads,
// so several edits of one side can be in flight at once and their requests can LAND in a different
// order than the user made them and than their results are reported. A push that fails may also have
// landed. The one thing that is known is which edit is the NEWEST made, so everything here works
// from that:
//
//   * the newest edit's own success is the only thing that clears the parked value;
//   * any other result -- an older edit's success or failure, or the newest's own failure -- means the
//     server may hold something other than the newest value, so the newest is parked for the
//     background thread (RetryDeferredServerSyncs()) to push again;
//   * a retry works the same way.
//
// This was found by three rounds of sweeps and checked at last by enumerating every landing order,
// result order and outcome of three edits (tests/test_padding_push.cpp): earlier versions compared
// values, then sequence numbers of parked values, and each left an interleaving that ended with the
// server on an older value. Guarded by the caller's own mutex; these functions only touch the struct
// they are given.
struct PendingPaddingPush
{
  std::optional<int> pre;
  std::optional<int> post;
  uint64_t preSeq = 0;  // sequence of the edit `pre` came from
  uint64_t postSeq = 0; // sequence of the edit `post` came from
  // The newest edit MADE per side, whether or not its push has finished.
  std::optional<PaddingEdit> preNewest;
  std::optional<PaddingEdit> postNewest;
  uint64_t nextSeq = 0; // last sequence handed out
  int retries = 0;
  // Direct pushes begun (BeginPaddingEdit()) whose result has not been applied yet. A server read
  // made while this is non-zero may predate a PATCH that is in flight or still queued behind the
  // client's settings mutex (see IsSafeToSyncPaddingFromServer()).
  int inFlightEdits = 0;

  bool HasPending() const
  {
    return pre.has_value() || post.has_value();
  }
};

// Called when the user makes an edit, before its push starts. A later edit always has a larger
// number than an earlier one, and becomes that side's newest.
inline uint64_t BeginPaddingEdit(PendingPaddingPush& state, bool isPre, int value)
{
  const uint64_t seq = ++state.nextSeq;
  (isPre ? state.preNewest : state.postNewest) = PaddingEdit{value, seq};
  ++state.inFlightEdits;
  return seq;
}

// Parks the newest edit of one side for the background retry (counting it as a fresh chance when it
// was not already parked).
inline void ParkNewestPaddingEdit(PendingPaddingPush& state, bool isPre)
{
  const std::optional<PaddingEdit>& newest = isPre ? state.preNewest : state.postNewest;
  if (!newest)
    return;
  std::optional<int>& side = isPre ? state.pre : state.post;
  uint64_t& sideSeq = isPre ? state.preSeq : state.postSeq;
  if (!side || sideSeq != newest->seq)
    state.retries = 0;
  side = newest->value;
  sideSeq = newest->seq;
}

// A push made directly from the settings callback (edit number `seq`) finished.
inline void ApplyDirectPaddingPushResult(PendingPaddingPush& state, bool isPre, int value, uint64_t seq, bool ok)
{
  if (state.inFlightEdits > 0)
    --state.inFlightEdits;
  std::optional<PaddingEdit>& newest = isPre ? state.preNewest : state.postNewest;
  if (!newest || newest->seq < seq)
    newest = PaddingEdit{value, seq}; // a caller that never called BeginPaddingEdit()
  std::optional<int>& side = isPre ? state.pre : state.post;
  const uint64_t sideSeq = isPre ? state.preSeq : state.postSeq;
  if (ok && seq == newest->seq)
  {
    if (side && sideSeq <= seq)
      side.reset();
    return;
  }
  ParkNewestPaddingEdit(state, isPre);
}

// What the background retry should send, decided at the moment the client's settings mutex is held
// (not before taking it: a direct push that succeeded while the retry waited for the mutex clears
// what was parked, and sending the value read earlier would put an older one back on the server for
// up to a whole retry interval -- or, if Kodi exits first, for good, because the next start syncs
// FROM the server). Each side is the parked edit, or nothing.
inline std::pair<std::optional<PaddingEdit>, std::optional<PaddingEdit>>
PendingPaddingEdits(const PendingPaddingPush& state)
{
  std::pair<std::optional<PaddingEdit>, std::optional<PaddingEdit>> out;
  if (state.pre)
    out.first = PaddingEdit{*state.pre, state.preSeq};
  if (state.post)
    out.second = PaddingEdit{*state.post, state.postSeq};
  return out;
}

// Whether a padding value just read from the server may be written over Kodi's own setting. Not when
// an edit is parked, not while a direct push is still in flight (the read may predate its PATCH),
// and not when an edit began after the read started (`nextSeqAtStart` is nextSeq as it was before
// the read): the user's newer value would be replaced by an older server one.
inline bool IsSafeToSyncPaddingFromServer(const PendingPaddingPush& state, uint64_t nextSeqAtStart)
{
  return !state.HasPending() && state.inFlightEdits == 0 && state.nextSeq == nextSeqAtStart;
}

enum class PaddingRetryOutcome
{
  kSent,       // the retry landed; whatever it sent is no longer pending
  kRetryLater, // it failed; try again next cycle
  kGaveUp,     // it failed too often; the edit is dropped and the user told
};

// A background retry that sent `sentPre`/`sentPost` (each nullopt when that side had nothing pending)
// finished. On success a side is settled only if what was sent is still that side's newest edit; if a
// newer edit has been made since, its value is parked to be sent (its own push may have landed before
// this older retry, which then put the old value back). On failure the same holds: the newest is
// parked, the failed value is not. After `maxRetries` failures in a row the pending edit is dropped:
// a non-admin account's push is refused every time, and the server's own value shows again after the
// next restart.
inline PaddingRetryOutcome ApplyPaddingRetryResult(PendingPaddingPush& state, std::optional<PaddingEdit> sentPre,
                                                   std::optional<PaddingEdit> sentPost, bool ok, int maxRetries)
{
  auto settle = [&state](bool isPre, const std::optional<PaddingEdit>& sent, bool succeeded)
  {
    if (!sent)
      return;
    std::optional<int>& side = isPre ? state.pre : state.post;
    const uint64_t sideSeq = isPre ? state.preSeq : state.postSeq;
    const std::optional<PaddingEdit>& newest = isPre ? state.preNewest : state.postNewest;
    const bool sentIsNewest = !newest || newest->seq == sent->seq;
    if (succeeded && sentIsNewest)
    {
      if (side && sideSeq == sent->seq)
        side.reset();
      return;
    }
    if (!sentIsNewest)
      ParkNewestPaddingEdit(state, isPre);
  };
  if (ok)
  {
    settle(true, sentPre, true);
    settle(false, sentPost, true);
    state.retries = 0;
    return PaddingRetryOutcome::kSent;
  }
  settle(true, sentPre, false);
  settle(false, sentPost, false);
  if (++state.retries >= maxRetries)
  {
    state.pre.reset();
    state.post.reset();
    state.retries = 0;
    return PaddingRetryOutcome::kGaveUp;
  }
  return PaddingRetryOutcome::kRetryLater;
}

} // namespace dispatcharr
