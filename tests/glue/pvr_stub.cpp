// Minimal Kodi runtime for driving PVRDispatcharr outside Kodi: settings, user path, VFS on the real
// filesystem, notifications, logging, and the PVR "to Kodi" callbacks (collecting transferred entries).
#include <kodi/AddonBase.h>
#include <kodi/General.h>
#include <kodi/addon-instance/PVR.h>
#include <kodi/Filesystem.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

AddonGlobalInterface* kodi::addon::CPrivateBase::m_interface = nullptr;

std::mutex g_setMu;
std::map<std::string, std::string> g_settings;
std::atomic<bool> g_dropSetSetting{false}; // Kodi drops addon writes while its settings dialog is open
std::atomic<long> g_setCount{0}, g_notifyCount{0}, g_triggerCount{0};
std::string g_userPath;

static char* dupstr(const std::string& s)
{
  return strdup(s.c_str());
}
static void my_log(const KODI_ADDON_BACKEND_HDL, const int lvl, const char* msg)
{
  static int minLvl = getenv("GLUE_LOG") ? atoi(getenv("GLUE_LOG")) : 99;
  if (lvl >= minLvl)
    fprintf(stderr, "[kodi %d] %s\n", lvl, msg);
}
static void my_free(const KODI_ADDON_BACKEND_HDL, char* s)
{
  free(s);
}
static void my_free_arr(const KODI_ADDON_BACKEND_HDL, char** a, int n)
{
  for (int i = 0; i < n; i++)
    free(a[i]);
  free(a);
}
static bool my_notify(void*, int, const char* h, const char* m, const char*, unsigned int, bool, unsigned int)
{
  g_notifyCount++;
  if (getenv("SHOW_NOTIFY"))
    fprintf(stderr, "[notify] %s: %s\n", h, m);
  return true;
}
static char* get_user_path(const KODI_ADDON_BACKEND_HDL)
{
  return dupstr(g_userPath);
}
static char* get_addon_path(const KODI_ADDON_BACKEND_HDL)
{
  return dupstr(g_userPath);
}
static bool lookup(const char* id, std::string& out)
{
  std::lock_guard<std::mutex> g(g_setMu);
  auto it = g_settings.find(id);
  if (it == g_settings.end())
    return false;
  out = it->second;
  return true;
}
static bool gs_bool(const KODI_ADDON_BACKEND_HDL, const char* id, bool* v)
{
  std::string s;
  if (!lookup(id, s))
    return false;
  *v = s == "true" || s == "1";
  return true;
}
static bool gs_int(const KODI_ADDON_BACKEND_HDL, const char* id, int* v)
{
  std::string s;
  if (!lookup(id, s))
    return false;
  *v = atoi(s.c_str());
  return true;
}
static bool gs_float(const KODI_ADDON_BACKEND_HDL, const char* id, float* v)
{
  std::string s;
  if (!lookup(id, s))
    return false;
  *v = (float)atof(s.c_str());
  return true;
}
static bool gs_str(const KODI_ADDON_BACKEND_HDL, const char* id, char** v)
{
  std::string s;
  if (!lookup(id, s))
    return false;
  *v = dupstr(s);
  return true;
}
static bool ss(const char* id, const std::string& v)
{
  g_setCount++;
  if (g_dropSetSetting)
    return true; // Kodi reports success and drops it
  std::lock_guard<std::mutex> g(g_setMu);
  g_settings[id] = v;
  return true;
}
static bool ss_bool(const KODI_ADDON_BACKEND_HDL, const char* id, bool v)
{
  return ss(id, v ? "true" : "false");
}
static bool ss_int(const KODI_ADDON_BACKEND_HDL, const char* id, int v)
{
  return ss(id, std::to_string(v));
}
static bool ss_float(const KODI_ADDON_BACKEND_HDL, const char* id, float v)
{
  return ss(id, std::to_string(v));
}
static bool ss_str(const KODI_ADDON_BACKEND_HDL, const char* id, const char* v)
{
  return ss(id, v);
}

// VFS on the real filesystem
static bool fs_exists(void*, const char* f, bool)
{
  struct stat st;
  return stat(f, &st) == 0 && S_ISREG(st.st_mode);
}
static bool fs_direxists(void*, const char* f)
{
  struct stat st;
  return stat(f, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool fs_mkdir(void*, const char* f)
{
  return mkdir(f, 0755) == 0;
}
static bool fs_delete(void*, const char* f)
{
  return unlink(f) == 0;
}
static bool fs_rename(void*, const char* a, const char* b)
{
  return rename(a, b) == 0;
}
static void* fs_open(void*, const char* f, unsigned int)
{
  return fopen(f, "rb");
}
static void* fs_openw(void*, const char* f, bool)
{
  return fopen(f, "wb");
}
static ssize_t fs_read(void*, void* fp, void* p, size_t n)
{
  return (ssize_t)fread(p, 1, n, (FILE*)fp);
}
static ssize_t fs_write(void*, void* fp, const void* p, size_t n)
{
  return (ssize_t)fwrite(p, 1, n, (FILE*)fp);
}
static void fs_close(void*, void* fp)
{
  fclose((FILE*)fp);
}
static void fs_flush(void*, void* fp)
{
  fflush((FILE*)fp);
}
static int64_t fs_len(void*, void* fp)
{
  long c = ftell((FILE*)fp);
  fseek((FILE*)fp, 0, SEEK_END);
  long e = ftell((FILE*)fp);
  fseek((FILE*)fp, c, SEEK_SET);
  return e;
}

static AddonToKodiFuncTable_kodi g_kodi;
static AddonToKodiFuncTable_kodi_addon g_kodiAddon;
static AddonToKodiFuncTable_kodi_filesystem g_fs;
static AddonToKodiFuncTable_Addon g_toKodi;
static KodiToAddonFuncTable_Addon g_toAddon;
static AddonGlobalInterface g_iface;

void InitKodiStub()
{
  memset(&g_kodi, 0, sizeof g_kodi);
  g_kodi.queue_notification = my_notify;
  memset(&g_kodiAddon, 0, sizeof g_kodiAddon);
  g_kodiAddon.get_user_path = get_user_path;
  g_kodiAddon.get_addon_path = get_addon_path;
  g_kodiAddon.get_setting_bool = gs_bool;
  g_kodiAddon.get_setting_int = gs_int;
  g_kodiAddon.get_setting_float = gs_float;
  g_kodiAddon.get_setting_string = gs_str;
  g_kodiAddon.set_setting_bool = ss_bool;
  g_kodiAddon.set_setting_int = ss_int;
  g_kodiAddon.set_setting_float = ss_float;
  g_kodiAddon.set_setting_string = ss_str;
  memset(&g_fs, 0, sizeof g_fs);
  g_fs.file_exists = fs_exists;
  g_fs.directory_exists = fs_direxists;
  g_fs.create_directory = fs_mkdir;
  g_fs.delete_file = fs_delete;
  g_fs.rename_file = fs_rename;
  g_fs.open_file = fs_open;
  g_fs.open_file_for_write = fs_openw;
  g_fs.read_file = fs_read;
  g_fs.write_file = fs_write;
  g_fs.close_file = fs_close;
  g_fs.flush_file = fs_flush;
  g_fs.get_file_length = fs_len;
  memset(&g_toKodi, 0, sizeof g_toKodi);
  g_toKodi.addon_log_msg = my_log;
  g_toKodi.free_string = my_free;
  g_toKodi.free_string_array = my_free_arr;
  g_toKodi.kodi = &g_kodi;
  g_toKodi.kodi_addon = &g_kodiAddon;
  g_toKodi.kodi_filesystem = &g_fs;
  memset(&g_iface, 0, sizeof g_iface);
  g_iface.toKodi = &g_toKodi;
  g_iface.toAddon = &g_toAddon;
  kodi::addon::CPrivateBase::m_interface = &g_iface;
}

// ---------------- PVR callbacks
struct Collected
{
  std::mutex mu;
  std::vector<PVR_CHANNEL> channels;
  std::vector<PVR_CHANNEL_GROUP> groups;
  std::vector<PVR_CHANNEL_GROUP_MEMBER> members;
  std::vector<EPG_TAG> epg;
  std::vector<std::string> epgTitles;
  std::vector<PVR_RECORDING> recordings;
  std::vector<PVR_TIMER> timers;
};
static void t_chan(void*, const PVR_HANDLE h, const PVR_CHANNEL* c)
{
  auto* r = (Collected*)h;
  std::lock_guard<std::mutex> g(r->mu);
  r->channels.push_back(*c);
}
static void t_group(void*, const PVR_HANDLE h, const PVR_CHANNEL_GROUP* c)
{
  auto* r = (Collected*)h;
  std::lock_guard<std::mutex> g(r->mu);
  r->groups.push_back(*c);
}
static void t_member(void*, const PVR_HANDLE h, const PVR_CHANNEL_GROUP_MEMBER* c)
{
  auto* r = (Collected*)h;
  std::lock_guard<std::mutex> g(r->mu);
  r->members.push_back(*c);
}
static void t_epg(void*, const PVR_HANDLE h, const EPG_TAG* c)
{
  auto* r = (Collected*)h;
  std::lock_guard<std::mutex> g(r->mu);
  r->epg.push_back(*c);
  // the pointers inside are only valid during the call -- copy the title now (what Kodi does)
  r->epgTitles.push_back(c->strTitle ? c->strTitle : "");
  r->epg.back().strTitle = nullptr;
  r->epg.back().strPlot = nullptr;
  r->epg.back().strPlotOutline = nullptr;
  r->epg.back().strOriginalTitle = nullptr;
  r->epg.back().strCast = nullptr;
  r->epg.back().strDirector = nullptr;
  r->epg.back().strWriter = nullptr;
  r->epg.back().strIMDBNumber = nullptr;
  r->epg.back().strIconPath = nullptr;
  r->epg.back().strGenreDescription = nullptr;
  r->epg.back().strEpisodeName = nullptr;
  r->epg.back().strSeriesLink = nullptr;
  r->epg.back().strFirstAired = nullptr;
  r->epg.back().strParentalRatingCode = nullptr;
}
static void t_rec(void*, const PVR_HANDLE h, const PVR_RECORDING* c)
{
  auto* r = (Collected*)h;
  std::lock_guard<std::mutex> g(r->mu);
  r->recordings.push_back(*c);
}
static void t_timer(void*, const PVR_HANDLE h, const PVR_TIMER* c)
{
  auto* r = (Collected*)h;
  std::lock_guard<std::mutex> g(r->mu);
  r->timers.push_back(*c);
}
static void t_trigger(void*)
{
  g_triggerCount++;
}
static void t_trigger_epg(void*, unsigned int)
{
  g_triggerCount++;
}
static void t_conn(void*, const char*, PVR_CONNECTION_STATE, const char*)
{
}
static void t_menu(void*, const PVR_MENUHOOK*)
{
}
static void t_recnote(void*, const char*, const char*, bool)
{
}
static void t_epgstate(void*, EPG_TAG*, EPG_EVENT_STATE)
{
}

AddonToKodiFuncTable_PVR g_pvrToKodi;
KodiToAddonFuncTable_PVR g_pvrToAddon;
AddonProperties_PVR g_pvrProps;
AddonInstance_PVR g_pvr;
KODI_ADDON_INSTANCE_INFO g_info;
KODI_ADDON_INSTANCE_FUNC g_instFuncs;
KODI_ADDON_INSTANCE_FUNC_CB g_instFuncsCb;
KODI_ADDON_INSTANCE_STRUCT g_inst;

KODI_ADDON_INSTANCE_STRUCT* MakePvrInstanceStruct()
{
  memset(&g_pvrToKodi, 0, sizeof g_pvrToKodi);
  g_pvrToKodi.TransferChannelEntry = t_chan;
  g_pvrToKodi.TransferChannelGroup = t_group;
  g_pvrToKodi.TransferChannelGroupMember = t_member;
  g_pvrToKodi.TransferEpgEntry = t_epg;
  g_pvrToKodi.TransferRecordingEntry = t_rec;
  g_pvrToKodi.TransferTimerEntry = t_timer;
  g_pvrToKodi.TriggerChannelUpdate = t_trigger;
  g_pvrToKodi.TriggerChannelGroupsUpdate = t_trigger;
  g_pvrToKodi.TriggerProvidersUpdate = t_trigger;
  g_pvrToKodi.TriggerRecordingUpdate = t_trigger;
  g_pvrToKodi.TriggerTimerUpdate = t_trigger;
  g_pvrToKodi.TriggerEpgUpdate = t_trigger_epg;
  g_pvrToKodi.ConnectionStateChange = t_conn;
  g_pvrToKodi.AddMenuHook = t_menu;
  g_pvrToKodi.RecordingNotification = t_recnote;
  g_pvrToKodi.EpgEventStateChange = t_epgstate;
  memset(&g_pvrToAddon, 0, sizeof g_pvrToAddon);
  g_pvrProps.strUserPath = g_userPath.c_str();
  g_pvrProps.strClientPath = g_userPath.c_str();
  g_pvrProps.iEpgMaxFutureDays = 7;
  g_pvrProps.iEpgMaxPastDays = 3;
  g_pvr.props = &g_pvrProps;
  g_pvr.toKodi = &g_pvrToKodi;
  g_pvr.toAddon = &g_pvrToAddon;
  memset(&g_info, 0, sizeof g_info);
  g_info.type = ADDON_INSTANCE_PVR;
  g_info.id = "pvr";
  g_info.version = "9.0.0";
  g_info.first_instance = true;
  memset(&g_instFuncsCb, 0, sizeof g_instFuncsCb);
  g_info.functions = &g_instFuncsCb;
  memset(&g_instFuncs, 0, sizeof g_instFuncs);
  memset(&g_inst, 0, sizeof g_inst);
  g_inst.info = &g_info;
  g_inst.functions = &g_instFuncs;
  g_inst.pvr = &g_pvr;
  return &g_inst;
}
