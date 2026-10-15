#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace dispatcharr
{

// The recorded streams Kodi 22 holds open, keyed by the id OpenRecordedStream() handed back. Kodi may open a second
// stream before it closes the first (PVRCapabilities::SetSupportsMultipleRecordedStreams()), and a Close for the old id
// can arrive after the new one's Open, so every call has to find its own stream rather than "the" stream.
//
// The mutex is a leaf: it guards the map only and is never held across a stream call or a network call. A caller gets a
// shared_ptr, so a stream removed while another thread is mid-read stays alive until that read returns.
template <typename T> class RecordedStreamRegistry
{
public:
  // Ids count up from 1 and are never reused, so a stale id from an earlier stream can never reach a later one, and 0
  // and negatives (what Kodi uses for "no stream") are never issued.
  int64_t Add(std::shared_ptr<T> stream)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t id = ++m_lastId;
    m_streams.emplace(id, std::move(stream));
    return id;
  }

  std::shared_ptr<T> Find(int64_t id) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_streams.find(id);
    return it == m_streams.end() ? nullptr : it->second;
  }

  // Null when the id is unknown or already removed, so a second Close of the same id is harmless.
  std::shared_ptr<T> Remove(int64_t id)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_streams.find(id);
    if (it == m_streams.end())
      return nullptr;
    std::shared_ptr<T> stream = std::move(it->second);
    m_streams.erase(it);
    return stream;
  }

  std::vector<std::shared_ptr<T>> TakeAll()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::shared_ptr<T>> all;
    all.reserve(m_streams.size());
    for (auto& entry : m_streams)
      all.push_back(std::move(entry.second));
    m_streams.clear();
    return all;
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_streams.size();
  }

private:
  mutable std::mutex m_mutex;
  std::map<int64_t, std::shared_ptr<T>> m_streams;
  int64_t m_lastId = 0;
};

} // namespace dispatcharr
