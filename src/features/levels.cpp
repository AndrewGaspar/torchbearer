// Level select, boot-to-level, player where/teleport, and the in-world lab state.
//
// Everything here runs on the game thread from a hook on ModeSwitch::update (the per-frame mode
// manager, ModeSwitch.cpp in the game's debug paths): lab verbs queue jobs and wait for them.
// Addresses and how they were found: re/levels.md. Level ids: docs/levels.md.
#include "features/levels.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"

namespace tb {
namespace {

// ---- game addresses (RVAs, image base 0x140000000) ---------------------------------------

constexpr uintptr_t RVA_MODESWITCH_UPDATE = 0x263600;  // ModeSwitch::update(this)
constexpr uintptr_t RVA_MENU_ACTION = 0x24d8f0;        // MenuMode::action(this, cmd, arg)
constexpr uintptr_t RVA_MENU_ENTER_MAIN = 0x24d200;    // MenuMode: welcome -> main menu
constexpr uintptr_t RVA_MENU_CONTINUE = 0x257920;      // MenuMode: load the slot's checkpoint
constexpr uintptr_t RVA_STR_ASSIGN = 0x3bacc0;         // String::operator=(const char*)
constexpr uintptr_t RVA_STR_FREE = 0x3bac00;           // String::clear/free
constexpr uintptr_t RVA_SAVE_SLOT = 0x2bb1e0;          // SaveGameManager::setSlot(this, a, b)
constexpr uintptr_t RVA_SAVE_STATE = 0x2b9420;         // SaveGameManager::state(this): 1 = none
constexpr uintptr_t RVA_SAVE_DELETE = 0x2b9240;        // SaveGameManager::deleteCheckpoint(this)
constexpr uintptr_t RVA_CHAR_SETXFORM = 0x17df50;      // Character::teleport(this, xform*)
constexpr uintptr_t RVA_EULER = 0x14c4c0;              // rotation from euler(out, a, b, c, order)
constexpr uintptr_t RVA_VIDEO_STOP = 0x337090;         // VideoMode: stop the playing Bink
// Getters `mov rax,[rip+d]; ret`: the global is resolved from the instruction itself.
constexpr uintptr_t RVA_GET_MENU = 0x24d5f0;      // MenuMode*
constexpr uintptr_t RVA_GET_PROPS = 0x1da6b0;     // GameProperties* (+0: String level to load)
constexpr uintptr_t RVA_GET_DIFF = 0x1b09d0;      // DifficultySystem* (+0: int)
constexpr uintptr_t RVA_GET_SAVE = 0x2b9410;      // SaveGameManager*
constexpr uintptr_t RVA_GET_PLAYERS = 0x1873a0;   // player manager (+0x50: Character*)
constexpr uintptr_t RVA_GET_LOADING = 0x236ce0;   // LoadingMode* (+0x10 state, +0x48 world)
constexpr uintptr_t RVA_GET_PLATFORM = 0x2981c0;  // PlatformServices*
constexpr uintptr_t RVA_GET_OPTIONS = 0x195fc0;   // command-line options (+0xd developermenu)
constexpr uintptr_t RVA_SET_CONTROLS = 0x3ff690;  // EnablePlayerControls: two byte flags
constexpr uintptr_t RVA_GET_CAMMODE = 0x1d2540;   // `mov eax,[rip+d]; ret`: camera mode (int)
constexpr uintptr_t RVA_GET_PLAYERCAM = 0x1d2570; // PlayerCamera* (+0xcec position)
constexpr uintptr_t RVA_VT_MENUMODE = 0x764ac0;
constexpr uintptr_t RVA_VT_GAMEMODE = 0x75d308;
constexpr uintptr_t RVA_VT_LOADINGMODE = 0x7634f8;
constexpr uintptr_t RVA_VT_VIDEOMODE = 0x7726e8;
constexpr uintptr_t RVA_VT_CREDITSMODE = 0x757fc0;

struct Sig {
  uintptr_t rva;
  const char* hex;
};
const Sig kSigs[] = {
    {RVA_MODESWITCH_UPDATE, "48 8b c4 48 89 58 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57"},
    {RVA_MENU_ACTION, "48 8b c4 48 89 58 18 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57"},
    {RVA_MENU_ENTER_MAIN, "40 53 48 83 ec 20 83 b9 a8 02 00 00 01 48 8b d9"},
    {RVA_MENU_CONTINUE, "48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 55 48 8d 6c 24 80"},
    {RVA_STR_ASSIGN, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48"},
    {RVA_STR_FREE, "40 53 48 83 ec 20 48 8b d9 48 8b 09 48 85 c9 74"},
    {RVA_SAVE_SLOT, "48 89 5c 24 10 48 89 6c 24 18 56 57 41 56 48 83"},
    {RVA_SAVE_STATE, "48 89 5c 24 10 48 89 74 24 18 48 89 7c 24 20 55"},
    {RVA_SAVE_DELETE, "48 89 5c 24 08 55 48 8d ac 24 40 fe ff ff 48 81"},
    {RVA_CHAR_SETXFORM, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 60 48 8b da 48 8b f9"},
    {RVA_VIDEO_STOP, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b f9 48 8b 89 90 06 00"},
    {RVA_EULER, "48 8b c4 53 48 81 ec 90 00 00 00 0f 29 70 e8"},
    {0x1402652c0 - 0x140000000, "48 8d 81 bc 00 00 00 c3"},  // Character::transform: this+0xbc
    {0x140184010 - 0x140000000, "48 8b 41 50 c3"},           // players->character: +0x50
    {0x14017b3a0 - 0x140000000, "0f b6 81 c9 01 00 00 c3"},  // Character in vehicle: +0x1c9
    {RVA_SET_CONTROLS, "84 d2 74 07 88 0d ?? ?? ?? ?? c3 88 0d ?? ?? ?? ?? c3"},
};

bool sig_ok(uintptr_t rva, const char* hex) {
  const auto* p = reinterpret_cast<const uint8_t*>(game_image().at(rva));
  for (const char* h = hex; *h;) {
    while (*h == ' ') h++;
    if (!*h) break;
    if (h[0] == '?') {
      h += 2, p++;
      continue;
    }
    unsigned v;
    if (sscanf(h, "%2x", &v) != 1 || *p != v) return false;
    h += 2, p++;
  }
  return true;
}

// `mov rax, [rip+disp32]; ret` -> address of the global it reads.
void** rip_global(uintptr_t rva) {
  const auto* p = reinterpret_cast<const uint8_t*>(game_image().at(rva));
  if (p[0] != 0x48 || p[1] != 0x8b || p[2] != 0x05 || p[7] != 0xc3) return nullptr;
  int32_t d;
  memcpy(&d, p + 3, 4);
  return reinterpret_cast<void**>(const_cast<uint8_t*>(p) + 7 + d);
}

// The engine's String (MenuMode.cpp locals): data, inline capacity, capacity, length, inline
// pointer, inline buffer. Short strings stay in the inline buffer.
struct GStr {
  char* p;
  uint64_t icap, cap, len;
  char* ibuf;
  char buf[0x90];
  GStr() : p(buf), icap(0x80), cap(0x80), len(0), ibuf(buf) { buf[0] = 0; }
};

using ModeUpdateFn = void (*)(void*);
using MenuActionFn = void (*)(void*, GStr*, GStr*);
using MenuEnterMainFn = void (*)(void*);
using StrAssignFn = void* (*)(void*, const char*);
using StrFreeFn = void (*)(void*);
using SaveSlotFn = void (*)(void*, int, int);
using SaveStateFn = int (*)(void*);
using SaveDeleteFn = void (*)(void*);
using SetXformFn = void (*)(void*, float*);
using EulerFn = void (*)(float*, float, float, float, int);
using VideoStopFn = void (*)(void*);

struct Game {
  bool ok = false;
  ModeUpdateFn ms_update_orig = nullptr;
  MenuActionFn menu_action;
  MenuEnterMainFn menu_enter_main;
  MenuEnterMainFn menu_continue;
  StrAssignFn str_assign;
  StrFreeFn str_free;
  SaveSlotFn save_slot;
  SaveStateFn save_state;
  SaveDeleteFn save_delete;
  SetXformFn set_xform;
  EulerFn euler;
  VideoStopFn video_stop;
  void** menu;
  void** props;
  void** diff;
  void** save;
  void** players;
  void** loading;
  void** platform;
  void** options;
  volatile uint8_t* controls_a;  // set by EnablePlayerControls(x, true)
  volatile uint8_t* controls_b;  // ... (x, false)
  // Camera mode (CameraManager, FUN_1401d2fe0): 0 player camera, 1/2/6 free camera,
  // 3 cinematic, 4/5 animation preview, 7 death camera.
  volatile int* cam_mode;
  void** player_cam;
  uintptr_t vt_menu, vt_game, vt_loading, vt_video, vt_credits;
} G;

// ---- levels --------------------------------------------------------------------------------

struct Level {
  const char* id;     // short id used on the command line
  const char* world;  // world:level passed to the game
  const char* episode;
  const char* title;  // the game's own (commented-out) developer-menu label
};
const Level kLevels[] = {
    {"01", "gameworld:level_01", "Episode 1: Nightmare", "Intro"},
    {"02", "gameworld:level_02", "Episode 1: Nightmare", "Arrival"},
    {"03", "gameworld:level_03", "Episode 1: Nightmare", "Meeting Stucky"},
    {"04", "gameworld:level_04", "Episode 2: Taken", "Sheriff Station"},
    {"05", "gameworld:level_05", "Episode 2: Taken", "Elderwood"},
    {"09", "gameworld:level_09", "Episode 3: Ransom", "Trailer Park"},
    {"10", "gameworld:level_10", "Episode 3: Ransom", "Mirror Peak"},
    {"11", "gameworld:level_11", "Episode 4: The Truth", "Cauldron Lake Lodge"},
    {"12", "gameworld:level_12", "Episode 4: The Truth", "Andersson Farm"},
    {"13", "gameworld:level_13", "Episode 4: The Truth", "Cabin Dream"},
    {"14", "gameworld:level_14", "Episode 5: The Clicker", "Bright Falls"},
    {"15", "gameworld:level_15", "Episode 5: The Clicker", "Dam"},
    {"16", "gameworld:level_16", "Episode 6: Departure", "Final Fight"},
    {"17", "gameworld:level_17", "Episode 6: Departure", "End"},
    {"100", "dlc1_world:level_100", "Special 1: The Signal", "The Signal"},
    {"200", "dlc2_world:level_200", "Special 2: The Writer", "The Writer"},
};

// "5", "05", "level_05", "gameworld:level_05", "dlc1", "100" -> world name; "" if unknown.
// Anything containing ':' is passed through as is (another world, or a test).
std::string resolve_level(std::string s) {
  if (s.find(':') != std::string::npos) return s;
  for (auto& c : s) c = char(tolower(c));
  if (s == "dlc1" || s == "signal") s = "100";
  if (s == "dlc2" || s == "writer") s = "200";
  if (s.rfind("level_", 0) == 0) s = s.substr(6);
  if (s.size() == 1 && isdigit(s[0])) s = "0" + s;
  for (auto& l : kLevels)
    if (s == l.id) return l.world;
  return "";
}

int resolve_difficulty(std::string s) {
  for (auto& c : s) c = char(tolower(c));
  if (s == "easy" || s == "0") return 0;
  if (s == "normal" || s == "hard" || s == "1") return 1;
  if (s == "nightmare" || s == "2") return 2;
  return -1;
}

// ---- game-thread jobs ----------------------------------------------------------------------

std::mutex g_jobs_mu;
std::deque<std::function<void()>> g_jobs;

// Run fn on the game thread; wait up to timeout_ms. Returns false on timeout.
bool on_game_thread(std::function<void()> fn, DWORD timeout_ms = 3000) {
  struct Done {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
  };
  auto d = std::make_shared<Done>();
  {
    std::lock_guard lk(g_jobs_mu);
    g_jobs.push_back([fn, d] {
      fn();
      std::lock_guard l2(d->mu);
      d->done = true;
      d->cv.notify_all();
    });
  }
  std::unique_lock lk(d->mu);
  return d->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return d->done; });
}

// ---- state ---------------------------------------------------------------------------------

template <typename T>
T rd(const void* base, size_t off) {
  T v;
  memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof v);
  return v;
}

void* g_ms = nullptr;          // ModeSwitch this
std::string g_pending_level;   // queued level load (game thread only)
int g_pending_diff = -1;
std::string g_boot_level;      // [dev] start_level, consumed at the first main menu
int g_boot_diff = 0;
bool g_boot_done = false;
std::atomic<int> g_dev_menu{-1};
std::atomic<bool> g_skip_videos{false};  // [dev] skip_videos
bool g_skip_once = false;                // `skip` verb (game thread)
uint64_t g_video_frames = 0;

struct Snapshot {
  uintptr_t cur_vt = 0, prev_vt = 0;  // ModeSwitch +0xa0 (active) / +0x98 mode vtables
  int menu_kind = -1, menu_page = -1;  // MenuMode +0x14 (screen) / +0x1c (pending screen)
  int loading = -1;                    // LoadingMode +0x10
  void* player = nullptr;
  int controls_a = -1, controls_b = -1;
  int cam_mode = -1;
  bool video = false;  // VideoMode (ModeSwitch +0x58) active: a Bink cutscene is playing
  bool in_vehicle = false;
  std::string world;  // LoadingMode +0x48
};

const char* mode_name(uintptr_t vt) {
  if (!vt) return "none";
  if (vt == G.vt_menu) return "menu";
  if (vt == G.vt_game) return "game";
  if (vt == G.vt_loading) return "loading";
  if (vt == G.vt_video) return "video";
  if (vt == G.vt_credits) return "credits";
  return "other";
}

Snapshot snap() {
  Snapshot s;
  if (g_ms) {
    void* cur = rd<void*>(g_ms, 0xa0);
    void* prev = rd<void*>(g_ms, 0x98);
    if (cur) s.cur_vt = rd<uintptr_t>(cur, 0);
    if (prev) s.prev_vt = rd<uintptr_t>(prev, 0);
    void* vid = rd<void*>(g_ms, 0x58);
    if (vid && rd<uintptr_t>(vid, 0) == G.vt_video) s.video = rd<uint8_t>(vid, 0xc) != 0;
  }
  if (void* m = *G.menu) {
    s.menu_kind = rd<int>(m, 0x14);
    s.menu_page = rd<int>(m, 0x1c);
  }
  if (void* l = *G.loading) {
    s.loading = rd<int>(l, 0x10);
    if (const char* w = rd<const char*>(l, 0x48)) s.world.assign(w, strnlen(w, 128));
  }
  if (void* pm = *G.players) s.player = rd<void*>(pm, 0x50);
  if (s.player) s.in_vehicle = rd<uint8_t>(s.player, 0x1c9) != 0;
  s.cam_mode = *G.cam_mode;
  s.controls_a = *G.controls_a;
  s.controls_b = *G.controls_b;
  return s;
}

// MenuMode screens (+0x14), established live: 1 boot, 2 "Press any key to play", 3 attract
// video, 4 main menu, 5 pause menu (in-game). +0x1c is the screen being switched to (0: none).
constexpr int MENU_WELCOME = 2, MENU_MAIN = 4;

bool is_loading(const Snapshot& s) { return s.loading != 0 && s.loading != 0xc && s.loading != -1; }

std::string snap_json(const Snapshot& s) {
  char b[512];
  snprintf(b, sizeof b,
           "{\"mode\":%s,\"prev\":%s,\"menu_kind\":%d,\"menu_page\":%d,\"loading\":%d,"
           "\"player\":%s,\"camera\":%d,\"video\":%s,\"controls\":[%d,%d],\"in_vehicle\":%s,\"world\":%s}",
           json_str(mode_name(s.cur_vt)).c_str(), json_str(mode_name(s.prev_vt)).c_str(),
           s.menu_kind, s.menu_page, s.loading, s.player ? "true" : "false", s.cam_mode, s.video ? "true" : "false", s.controls_a,
           s.controls_b, s.in_vehicle ? "true" : "false", json_str(s.world).c_str());
  return b;
}

// ---- actions (game thread) -----------------------------------------------------------------

// Start a level the way the main menu's episode list does (MenuMode::action "load_episode"),
// after making sure none of its blocking platform dialogs can come up: an existing checkpoint
// save would ask to be overwritten, so it is deleted first (the game does the same on "yes").
std::string start_level_from_menu(const std::string& world, int diff) {
  void* menu = *G.menu;
  void* save = *G.save;
  void* plat = *G.platform;
  if (!menu || !save || !plat) return "game not ready";
  auto vt = *reinterpret_cast<void***>(plat);
  bool signed_in = reinterpret_cast<bool (*)(void*)>(vt[0x18 / 8])(plat);
  bool storage = reinterpret_cast<bool (*)(void*)>(vt[0x120 / 8])(plat);
  if (!signed_in || !storage) return "platform not signed in / no storage";
  G.save_slot(save, 0, 1);  // what the menu selects before starting an episode
  if (world == "continue") {
    // Main menu "Continue Game": load the checkpoint save (a fixture, or the last run's).
    if (G.save_state(save) == 1) return "no checkpoint save to continue from";
    G.menu_continue(menu);
    return "";
  }
  if (diff >= 0 && *G.diff) *reinterpret_cast<int*>(*G.diff) = diff;
  if (G.save_state(save) != 1) {
    TB_LOG("levels: deleting the existing checkpoint save before starting %s", world.c_str());
    G.save_delete(save);
  }
  GStr cmd, arg;
  G.str_assign(&cmd, "load_episode");
  G.str_assign(&arg, world.c_str());
  G.menu_action(menu, &cmd, &arg);
  G.str_free(&cmd);
  G.str_free(&arg);
  return "";
}

// In-world: hand the world to the mode switch the way the debug panel's "Load World" does
// (GameProperties' level string is picked up by ModeSwitch::update on its next frame).
std::string start_level_in_world(const std::string& world, int diff) {
  void* props = *G.props;
  if (!props) return "game not ready";
  if (diff >= 0 && *G.diff) *reinterpret_cast<int*>(*G.diff) = diff;
  G.str_assign(props, world.c_str());
  return "";
}

bool g_started_level = false;  // a level load we issued is in progress (for logging)

void run_pending(const Snapshot& s) {
  if (g_pending_level.empty()) return;
  std::string err;
  const char* how;
  if (s.cur_vt == G.vt_menu && s.player == nullptr) {
    if (s.menu_page != 0) return;  // a screen transition is running
    if (s.menu_kind == MENU_WELCOME) {
      // "Press any key to play": what the key press does once the platform user is signed in.
      void* plat = *G.platform;
      auto vt = plat ? *reinterpret_cast<void***>(plat) : nullptr;
      if (vt && reinterpret_cast<bool (*)(void*)>(vt[0x18 / 8])(plat)) {
        TB_LOG("levels: leaving the welcome screen for the main menu");
        G.menu_enter_main(*G.menu);
      }
      return;
    }
    if (s.menu_kind != MENU_MAIN) return;
    how = "main menu";
    err = start_level_from_menu(g_pending_level, g_pending_diff);
  } else if (s.cur_vt == G.vt_game && !is_loading(s) && g_pending_level != "continue") {
    how = "in-world";
    err = start_level_in_world(g_pending_level, g_pending_diff);
  } else {
    return;  // videos, loading: try again next frame
  }
  if (err.empty()) {
    TB_LOG("levels: started %s (difficulty %d) from the %s", g_pending_level.c_str(),
           g_pending_diff, how);
    g_started_level = true;
  } else {
    TB_LOG("levels: cannot start %s: %s", g_pending_level.c_str(), err.c_str());
    if (err == "game not ready") return;  // retry next frame
  }
  g_pending_level.clear();
}

// Skip the playing Bink cutscene the way the player's skip does (VideoMode::update, 0x337140,
// on ABORT_VIDEO + confirm): stop it and mark it finished; ModeSwitch then leaves VideoMode and
// runs whatever the end of the video triggers. Any queued follow-up videos are dropped too, as
// MenuMode's own "stop videos" (0x336040) does.
void skip_video_if_wanted(const Snapshot& s) {
  g_video_frames = s.video ? g_video_frames + 1 : 0;
  if (!s.video || g_video_frames < 30) return;  // let it start (half a second)
  if (!g_skip_videos && !g_skip_once) return;
  void* v = rd<void*>(g_ms, 0x58);
  auto* b = static_cast<uint8_t*>(v);
  if (b[0x618] || b[0x61a]) return;  // still starting, or already finished
  TB_LOG("levels: skipping the cutscene video");
  memcpy(b + 0x610, b + 0x614, 4);  // drop the queue
  G.video_stop(v);
  b[0x61a] = 1;
  g_skip_once = false;
}

// ---- lab state -----------------------------------------------------------------------------

std::string g_state;
uint64_t g_frames_ready = 0;
std::string g_last_fp;

void update_state(const Snapshot& s) {
  // Log every change of the raw picture (rare: mode switches, menu pages, control toggles).
  char fp[256];
  snprintf(fp, sizeof fp, "mode=%s prev=%s menu=%d/%d loading=%d player=%d camera=%d video=%d controls=%d%d world=%s",
           mode_name(s.cur_vt), mode_name(s.prev_vt), s.menu_kind, s.menu_page, s.loading,
           s.player != nullptr, s.cam_mode, s.video, s.controls_a, s.controls_b, s.world.c_str());
  if (g_last_fp != fp) {
    g_last_fp = fp;
    TB_LOG("levels: %s", fp);
  }

  // Player control: gameplay mode, nothing loading, a player, and the player camera (not a
  // cinematic, free or death camera). The EnablePlayerControls flags stay 1 through the intro
  // cinematics, so they are logged but not used.
  bool ready = s.cur_vt == G.vt_game && !is_loading(s) && s.player && s.cam_mode == 0 &&
               !s.video;
  g_frames_ready = ready ? g_frames_ready + 1 : 0;
  std::string st;
  if (is_loading(s) || s.cur_vt == G.vt_loading) st = "loading";
  else if (g_frames_ready >= 60) st = "in-world";  // a second of uninterrupted control
  else if (s.cur_vt == G.vt_menu) st = s.player ? "paused" : "menu";
  else if (s.cur_vt == G.vt_game) st = s.video ? "video" : "in-game";  // no control yet
  else st = "boot";
  if (st != g_state) {
    g_state = st;
    std::string lvl = s.world;
    lab_state(st, {"world=" + lvl, std::string("mode=") + mode_name(s.cur_vt)});
    TB_LOG("levels: state %s (world %s)", st.c_str(), lvl.c_str());
    if (st == "in-world") g_started_level = false;
  }
}

// ---- the hook ------------------------------------------------------------------------------

void ms_update_detour(void* self) {
  g_ms = self;
  {
    std::deque<std::function<void()>> jobs;
    {
      std::lock_guard lk(g_jobs_mu);
      jobs.swap(g_jobs);
    }
    for (auto& j : jobs) j();
  }
  int dm = g_dev_menu.load();
  if (dm >= 0 && *G.options) {
    auto* o = static_cast<uint8_t*>(*G.options);
    if (o[0xd] != uint8_t(dm)) {
      o[0xd] = uint8_t(dm);
      TB_LOG("levels: developer menu %s", dm ? "on" : "off");
    }
  }
  Snapshot s = snap();
  if (!g_boot_done && !g_boot_level.empty() && s.cur_vt == G.vt_menu && !s.player) {
    g_boot_done = true;
    g_pending_level = g_boot_level;
    g_pending_diff = g_boot_diff;
    TB_LOG("levels: menu up, booting into %s", g_boot_level.c_str());
  }
  run_pending(s);
  skip_video_if_wanted(s);
  update_state(s);
  G.ms_update_orig(self);
}

// ---- verbs ---------------------------------------------------------------------------------

std::string err(bool& ok, const std::string& m) {
  ok = false;
  return json_str(m);
}

std::string verb_level(const std::vector<std::string>& a, bool& ok) {
  if (!G.ok) return err(ok, "level tools disabled (see torchbearer.log)");
  if (a.empty()) {
    std::string o = "[";
    for (auto& l : kLevels) {
      if (o.size() > 1) o += ",";
      o += "{\"id\":" + json_str(l.id) + ",\"world\":" + json_str(l.world) +
           ",\"episode\":" + json_str(l.episode) + ",\"title\":" + json_str(l.title) + "}";
    }
    return o + "]";
  }
  std::string world = a[0] == "continue" ? "continue" : resolve_level(a[0]);
  if (world.empty()) return err(ok, "unknown level " + a[0] + " (call `level` for the list)");
  int diff = a.size() > 1 ? resolve_difficulty(a[1]) : 0;
  if (diff < 0) return err(ok, "difficulty: easy|normal|nightmare");
  if (!on_game_thread([&] {
        g_pending_level = world;
        g_pending_diff = diff;
      }))
    return err(ok, "game thread not running");
  return json_str((world == "continue" ? std::string("continuing from the checkpoint save")
                                       : "loading " + world) +
                  "; wait for in-world");
}

std::string verb_where(const std::vector<std::string>&, bool& ok) {
  if (!G.ok) return err(ok, "level tools disabled (see torchbearer.log)");
  std::string out;
  if (!on_game_thread([&] {
        Snapshot s = snap();
        if (!s.player) {
          out = "";
          return;
        }
        const float* m = reinterpret_cast<const float*>(static_cast<uint8_t*>(s.player) + 0xbc);
        float yaw = std::atan2(-m[2], m[0]) * 57.29578f;
        float cam[3] = {0, 0, 0};
        if (void* pc = *G.player_cam) memcpy(cam, static_cast<uint8_t*>(pc) + 0xcec, sizeof cam);
        char b[384];
        snprintf(b, sizeof b,
                 "{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"yaw\":%.2f,\"world\":%s,"
                 "\"camera\":[%.3f,%.3f,%.3f],\"camera_mode\":%d}",
                 m[9], m[10], m[11], yaw, json_str(s.world).c_str(), cam[0], cam[1], cam[2],
                 s.cam_mode);
        out = b;
      }))
    return err(ok, "game thread not running");
  if (out.empty()) return err(ok, "no player");
  return out;
}

std::string verb_teleport(const std::vector<std::string>& a, bool& ok) {
  if (!G.ok) return err(ok, "level tools disabled (see torchbearer.log)");
  if (a.size() < 3) return err(ok, "usage: teleport x y z [yaw]");
  float x = strtof(a[0].c_str(), nullptr), y = strtof(a[1].c_str(), nullptr),
        z = strtof(a[2].c_str(), nullptr);
  bool have_yaw = a.size() > 3;
  float yaw = have_yaw ? strtof(a[3].c_str(), nullptr) : 0;
  std::string e;
  if (!on_game_thread([&] {
        Snapshot s = snap();
        if (!s.player) return void(e = "no player");
        if (s.in_vehicle) return void(e = "player is in a vehicle");
        auto* p = static_cast<uint8_t*>(s.player);
        float xf[12];
        if (have_yaw) {
          G.euler(xf, 0.f, yaw * 0.017453292f, 0.f, 0);  // as the script's Teleport action
        } else {
          memcpy(xf, p + 0xbc, 9 * sizeof(float));  // keep the current heading
        }
        xf[9] = x, xf[10] = y, xf[11] = z;
        G.set_xform(p, xf);
        TB_LOG("levels: teleported to %.2f %.2f %.2f yaw %s", x, y, z,
               have_yaw ? a[3].c_str() : "(kept)");
      }))
    return err(ok, "game thread not running");
  if (!e.empty()) return err(ok, e);
  return verb_where({}, ok);
}

bool resolve_game() {
  for (auto& s : kSigs) {
    if (!sig_ok(s.rva, s.hex)) {
      TB_LOG("levels: signature mismatch at rva 0x%zx; level tools disabled", (size_t)s.rva);
      return false;
    }
  }
  const auto& img = game_image();
  G.menu_action = reinterpret_cast<MenuActionFn>(img.at(RVA_MENU_ACTION));
  G.menu_enter_main = reinterpret_cast<MenuEnterMainFn>(img.at(RVA_MENU_ENTER_MAIN));
  G.menu_continue = reinterpret_cast<MenuEnterMainFn>(img.at(RVA_MENU_CONTINUE));
  G.str_assign = reinterpret_cast<StrAssignFn>(img.at(RVA_STR_ASSIGN));
  G.str_free = reinterpret_cast<StrFreeFn>(img.at(RVA_STR_FREE));
  G.save_slot = reinterpret_cast<SaveSlotFn>(img.at(RVA_SAVE_SLOT));
  G.save_state = reinterpret_cast<SaveStateFn>(img.at(RVA_SAVE_STATE));
  G.save_delete = reinterpret_cast<SaveDeleteFn>(img.at(RVA_SAVE_DELETE));
  G.set_xform = reinterpret_cast<SetXformFn>(img.at(RVA_CHAR_SETXFORM));
  G.euler = reinterpret_cast<EulerFn>(img.at(RVA_EULER));
  G.video_stop = reinterpret_cast<VideoStopFn>(img.at(RVA_VIDEO_STOP));
  G.menu = rip_global(RVA_GET_MENU);
  G.props = rip_global(RVA_GET_PROPS);
  G.diff = rip_global(RVA_GET_DIFF);
  G.save = rip_global(RVA_GET_SAVE);
  G.players = rip_global(RVA_GET_PLAYERS);
  G.loading = rip_global(RVA_GET_LOADING);
  G.platform = rip_global(RVA_GET_PLATFORM);
  G.options = rip_global(RVA_GET_OPTIONS);
  for (void** g : {G.menu, G.props, G.diff, G.save, G.players, G.loading, G.platform, G.options})
    if (!g || !img.contains(reinterpret_cast<uintptr_t>(g))) {
      TB_LOG("levels: a global getter does not match; level tools disabled");
      return false;
    }
  const auto* c = reinterpret_cast<const uint8_t*>(img.at(RVA_SET_CONTROLS));
  int32_t d;
  memcpy(&d, c + 6, 4);
  G.controls_a = const_cast<uint8_t*>(c + 10 + d);
  memcpy(&d, c + 13, 4);
  G.controls_b = const_cast<uint8_t*>(c + 17 + d);
  {
    const auto* p = reinterpret_cast<const uint8_t*>(img.at(RVA_GET_CAMMODE));
    if (p[0] != 0x8b || p[1] != 0x05 || p[6] != 0xc3) {
      TB_LOG("levels: camera-mode getter does not match; level tools disabled");
      return false;
    }
    memcpy(&d, p + 2, 4);
    G.cam_mode = reinterpret_cast<volatile int*>(const_cast<uint8_t*>(p) + 6 + d);
  }
  G.player_cam = rip_global(RVA_GET_PLAYERCAM);
  if (!G.player_cam) {
    TB_LOG("levels: player-camera getter does not match; level tools disabled");
    return false;
  }
  G.vt_menu = img.at(RVA_VT_MENUMODE);
  G.vt_game = img.at(RVA_VT_GAMEMODE);
  G.vt_loading = img.at(RVA_VT_LOADINGMODE);
  G.vt_video = img.at(RVA_VT_VIDEOMODE);
  G.vt_credits = img.at(RVA_VT_CREDITSMODE);
  return true;
}

}  // namespace

void levels_register() {
  cfg_listen("dev.", [](const std::string& id, const std::string& v) {
    if (id == "dev.developer_menu") g_dev_menu = (v == "1" || v == "true") ? 1 : 0;
    if (id == "dev.skip_videos") g_skip_videos = (v == "1" || v == "true");
    if (id == "dev.start_level") {
      std::string w = v.empty() ? "" : v == "continue" ? v : resolve_level(v);
      if (!v.empty() && w.empty()) TB_LOG("levels: [dev] start_level: unknown level %s", v.c_str());
      on_game_thread([w] { g_boot_level = w; }, 0);  // read on the game thread
    }
    if (id == "dev.start_difficulty") {
      int d = resolve_difficulty(v);
      on_game_thread([d] { g_boot_diff = d < 0 ? 0 : d; }, 0);
    }
  });
  lab_verb("level", verb_level);
  lab_verb("where", verb_where);
  lab_verb("skip", [](const std::vector<std::string>&, bool& ok) -> std::string {
    if (!G.ok) return err(ok, "level tools disabled (see torchbearer.log)");
    bool video = false;
    if (!on_game_thread([&] {
          video = snap().video;
          if (video) g_skip_once = true;
        }))
      return err(ok, "game thread not running");
    return json_str(video ? "skipping the cutscene video" : "no video playing");
  });
  lab_verb("teleport", verb_teleport);
  lab_verb("game-state", [](const std::vector<std::string>&, bool& ok) {
    std::string out;
    if (!G.ok) return err(ok, "level tools disabled (see torchbearer.log)");
    if (!on_game_thread([&] { out = snap_json(snap()); })) return err(ok, "game thread not running");
    return out;
  });
}

void levels_start() {
  G.ok = resolve_game();
  if (!G.ok) return;
  void* target = reinterpret_cast<void*>(game_image().at(RVA_MODESWITCH_UPDATE));
  if (!hook_fn(target, reinterpret_cast<void*>(&ms_update_detour),
               reinterpret_cast<void**>(&G.ms_update_orig), "ModeSwitch::update"))
    G.ok = false;
  lab_state("boot");
}

}  // namespace tb
