#pragma once

#include <algorithm>
#include <chrono>
#include <vector>

namespace dispatcharr
{

// How long the re-delivery waits for Kodi to finish creating the PVR client before giving up (PVRDispatcharr::
// RequestSettingsRedelivery()). Kodi creates every enabled PVR client in turn before it lists any of them, so another
// addon timing out on its own server delays the signal.
constexpr std::chrono::seconds kSettingsRedeliveryMaxWait{300};

// Which PVR instances, still being constructed, missed a settings delivery.
//
// CAddonDispatcharr::SetSetting() can deliver a change only to instances it has registered, and an instance is
// registered only once its constructor returns. Kodi holds only its instance-creation lock across the constructor and a
// settings save does not take it, so a save that lands mid-construction (the constructor can wait a whole request
// timeout on a host that does not answer) reached no instance and nothing asked for the restart a connection change
// needs (docs/CLOSED_ITEMS.md, "A settings save delivered while the PVR instance is still being constructed is
// dropped"). This records, per construction, whether a delivery came in from another thread meanwhile; the
// constructor's own setting writes come back synchronously on its own thread and are not counted.
//
// Not thread-safe: the caller holds its own mutex around every call. ThreadId is std::thread::id in the addon, any
// equality-comparable type in the tests.
template <typename ThreadId> class ConstructionDeliveryTracker
{
public:
  void BeginConstruction(const ThreadId& thread)
  {
    m_constructions.push_back({thread, false});
  }

  void OnDelivery(const ThreadId& thread)
  {
    for (Construction& construction : m_constructions)
    {
      if (!(construction.thread == thread))
        construction.missedDelivery = true;
    }
  }

  // Ends the construction begun on `thread` (the most recent one, if the same thread somehow has two) and answers
  // whether a delivery came in from another thread while it ran.
  bool EndConstruction(const ThreadId& thread)
  {
    for (auto it = m_constructions.rbegin(); it != m_constructions.rend(); ++it)
    {
      if (it->thread == thread)
      {
        const bool missed = it->missedDelivery;
        m_constructions.erase(std::next(it).base());
        return missed;
      }
    }
    return false;
  }

  bool Constructing() const
  {
    return !m_constructions.empty();
  }

private:
  struct Construction
  {
    ThreadId thread;
    bool missedDelivery;
  };
  std::vector<Construction> m_constructions;
};

} // namespace dispatcharr
