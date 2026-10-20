#include <kodi/AddonBase.h>
#include <kodi/General.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
AddonGlobalInterface* kodi::addon::CPrivateBase::m_interface = nullptr;
static void my_log(const KODI_ADDON_BACKEND_HDL, const int lvl, const char* msg)
{
  if (getenv("GLUE_LOG"))
    fprintf(stderr, "[kodi %d] %s\n", lvl, msg);
}
static void my_free(const KODI_ADDON_BACKEND_HDL, char* s)
{
  free(s);
}
// Read by stream_driver.cpp's scenarios that check what the viewer is told.
std::atomic<long> g_notifyCount{0};
std::mutex g_notifyMutex;
std::string g_lastNotify;
static bool my_notify(void*, int, const char* h, const char* m, const char*, unsigned int, bool, unsigned int)
{
  fprintf(stderr, "[notify] %s: %s\n", h, m);
  {
    std::lock_guard<std::mutex> lock(g_notifyMutex);
    g_lastNotify = m ? m : "";
  }
  g_notifyCount++;
  return true;
}
static AddonToKodiFuncTable_kodi g_kodi;
static AddonToKodiFuncTable_Addon g_toKodi;
static KodiToAddonFuncTable_Addon g_toAddon;
static AddonGlobalInterface g_iface;
void InitKodiStub()
{
  memset(&g_kodi, 0, sizeof g_kodi);
  g_kodi.queue_notification = my_notify;
  memset(&g_toKodi, 0, sizeof g_toKodi);
  g_toKodi.addon_log_msg = my_log;
  g_toKodi.free_string = my_free;
  g_toKodi.kodi = &g_kodi;
  g_iface.toKodi = &g_toKodi;
  g_iface.toAddon = &g_toAddon;
  kodi::addon::CPrivateBase::m_interface = &g_iface;
}
