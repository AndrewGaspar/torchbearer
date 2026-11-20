// First-person controls, scheme A "deliberate" (docs/first-person.md section 12).
//
// `[camera] controls = deliberate` (default) changes what the player's inputs do while first
// person is active; `game` (and third person) leaves the game's controls alone (every hook passes
// through):
//   - the gun is holstered by default: aim (the game's LIGHT_BOOST action: right mouse / LT) draws
//     and raises it, fire (left mouse / RT) shoots while it is drawn; after aim is released it is
//     holstered again after `holster_delay` seconds, firing and reloading reset the timer; fire
//     while holstered draws it (the shot goes off once it is up, if fire is still held);
//   - flashlight boost moves to its own button (`key_boost` = V, `pad_boost` = RB), held; aim alone
//     is the game's unboosted aim (what a half-pulled LT does on a pad), boost alone raises the
//     flashlight (the game's aim stance) without drawing the gun;
//   - the flashlight switches on and off (`key_flashlight` = T, `pad_flashlight` = D-pad up) through
//     the game's own light-source switch (0x22e320, what the game uses when Alan picks up another
//     light), switched back on when the scheme stops applying;
//   - the pad actions on RB and D-pad up move to other buttons (`pad_moves`: THROW_ITEM and
//     HOLD_FLARE to R3, TOGGLE_SIDE off, INVENTORY_UP to L3).
//
// How (static, Ghidra; live in the lab): PlayerInputReader::update (0x2a0940, game thread) asks
// two predicates for the boost action 0x39: 0x2a05c0 "aiming" (keyboard held, or LT past -0.9:
// raises the weapon, the camera's aim stance, the animation's aim) and 0x2a0660 "boosting" (LT
// past 0.2: the flashlight's boost). On a keyboard both are the same key; we answer them
// separately. Shooting goes through the reader's shoot state (0x2d68c0 update, 0x2d6840 pressed),
// which reads SHOOT (action 3); while holstered it is held in its idle state. The pad's action
// table (0xa4 entries of {type, mask, button}, the global behind 0x3fe5cc) is patched in place to
// move actions off our buttons while the scheme applies, and put back after.
#include "features/fpcontrols.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "features/settings.h"

namespace tb {
namespace {

constexpr uintptr_t RVA_READER_UPDATE = 0x2a0940;  // PlayerInputReader::update(this) (vtable 0x7694b8 +0x18)
constexpr uintptr_t RVA_AIM_PRED = 0x2a05c0;       // reader "aiming" (boost action held / LT > -0.9, boost allowed)
constexpr uintptr_t RVA_BOOST_PRED = 0x2a0660;     // reader "boosting" (... LT > 0.2)
constexpr uintptr_t RVA_SHOOT_UPDATE = 0x2d68c0;   // ShootInput::update(this): +8/+9 fire latches, +0xb held
constexpr uintptr_t RVA_SHOOT_PRESSED = 0x2d6840;  // ShootInput "fire held" (keyboard SHOOT / RT)
constexpr uintptr_t RVA_FL_SWITCH = 0x22e320;      // Flashlight::switchOn(this, bool): +0x38 on, fades
constexpr uintptr_t RVA_IN_KBHELD = 0x3fe3b0;      // Input: keyboard action held (keybind map -> key state)
constexpr uintptr_t RVA_IN_PAD = 0x3fe5a0;         // Input: pad action (digital) through the pad table
constexpr uintptr_t RVA_IN_AXIS = 0x3fe450;        // Input: pad action value (-1..1)
constexpr uintptr_t RVA_IN_DISABLED = 0x3fe2c0;    // `movzx eax,[rip]; ret`: keyboard input off
constexpr uintptr_t RVA_IN_KBMODE = 0x3fe410;      // `movzx eax,[rip]; ret`: keyboard/mouse controls in use
constexpr uintptr_t RVA_VT_GAMEPAD = 0x7824c8;     // input::Gamepad_Windows vtable (update 0x400190)

constexpr int kActShoot = 3, kActReload = 4, kActBoost = 0x39;
constexpr size_t kCharFlashlight = 0x230, kCharWeapon = 0x220;

struct Sig {
  uintptr_t rva;
  const char* hex;
};
const Sig kSigs[] = {
    // reader update: push rbx/rbp/rsi/rdi; sub rsp,0x78; mov rdi,rcx; mov rcx,[rcx+8] (character)
    {RVA_READER_UPDATE, "40 53 55 56 57 48 83 ec 78 48 8b f9 48 8b 49 08 48 8b 01"},
    // both predicates: ... call 0x3fe2c0 (keyboard off?) ... the analog threshold differs
    {RVA_AIM_PRED, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b f9 32 db e8 e7 dc 15 00 be 01 00 00 00"},
    {RVA_BOOST_PRED, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b f9 32 db e8 47 dc 15 00 be 01 00 00 00"},
    {RVA_SHOOT_UPDATE, "48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 30 33 d2 0f 29 74 24 20 48 8b f9 8d 4a 03"},
    {RVA_SHOOT_PRESSED, "48 83 ec 38 e8 77 7a 12 00 84 c0 75 1e e8 be 7b 12 00 84 c0 74 15 b9 03 00 00 00"},
    // switch: movsxd rbx,[rcx+0xb8] (type, -1 = none); mov [rcx+0x38],dl (on)
    {RVA_FL_SWITCH, "48 89 5c 24 08 57 48 83 ec 30 48 63 99 b8 00 00 00 48 8b f9 88 51 38"},
    // keyboard held: ... mov rcx,[rip -> key state]; test rcx,rcx; jz; movzx eax,word [rdx+0x20] (vk);
    // movzx eax,byte [rax+rcx+0x1c]; and al,1
    {0x3fe3eb, "48 8b 0d ?? ?? ?? ?? 48 85 c9 74 0c 0f b7 42 20 0f b6 44 08 1c 24 01 c3"},
    // pad: cmp [rip -> device count],0; ...; movsxd rdx,[rip -> device index]; mov rax,[rip -> devices];
    // mov r9,[rax+rdx*8]; ... mov rax,[rip -> pad table]; lea rdx,[rax+rcx*8] (0x18 per action)
    {RVA_IN_PAD, "48 83 3d ?? ?? ?? ?? 00 0f 84 93 00 00 00 48 63 15 ?? ?? ?? ?? 48 8b 05 ?? ?? ?? ?? 4c 8b 0c d0 4d "
                 "85 c9 74 7c 48 63 c1 48 8d 0c 40 48 8b 05 ?? ?? ?? ?? 48 8d 14 c8"},
    {RVA_IN_AXIS, "48 83 ec 38 48 83 3d ?? ?? ?? ?? 00 4c 63 c1"},
    {RVA_IN_DISABLED, "0f b6 05 ?? ?? ?? ?? c3"},
    {RVA_IN_KBMODE, "0f b6 05 ?? ?? ?? ?? c3"},
    // Gamepad_Windows::update: XInputGetState(user [+0x54]) -> +0x70 XINPUT_GAMEPAD
    {0x400190, "48 89 5c 24 10 48 89 74 24 18 57 48 83 ec 40 0f 29 74 24 30 0f 28 f1 48 8b f9 8b 49 54 83 f9 03"},
};

bool sig_ok(uintptr_t rva, const char* hex) { return fp_sig_ok(rva, hex); }
void* rip_target(uintptr_t insn, int dispoff, int len) {
  int32_t d;
  memcpy(&d, reinterpret_cast<const void*>(insn + dispoff), 4);
  return reinterpret_cast<void*>(insn + len + d);
}
template <typename T>
T rd(const void* b, size_t off) {
  T v;
  memcpy(&v, static_cast<const uint8_t*>(b) + off, sizeof v);
  return v;
}
bool readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
  if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mi.BaseAddress) + mi.RegionSize;
}

using PredFn = char(__fastcall*)(void* reader);
using VoidFn = void(__fastcall*)(void* obj);
using SwitchFn = void(__fastcall*)(void* fl, char on);
PredFn g_o_aim = nullptr, g_o_boost = nullptr, g_o_shoot_pressed = nullptr;
VoidFn g_o_reader = nullptr, g_o_shoot_update = nullptr;
SwitchFn g_switch = nullptr;
uint8_t** g_kbstate = nullptr;    // &key state (byte per VK at +0x1c, bit 0 = held)
void*** g_pad_devices = nullptr;  // &devices[]
int* g_pad_index = nullptr;
uint8_t** g_pad_table = nullptr;  // &table: 0x18 bytes per action {int type, u32 mask, int button, ...}
const uint8_t* g_in_disabled = nullptr;
const uint8_t* g_in_kbmode = nullptr;
bool g_ok = false;

// ---- config ------------------------------------------------------------------------------------
// Pad buttons as the game indexes them (Gamepad_Windows: bit i of XINPUT_GAMEPAD.wButtons, 16 LT,
// 17 RT; static, 0x400190).
const char* kPadNames[18] = {"DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Start", "Back", "L3", "R3", "LB",
                             "RB",     "Btn10",    "Btn11",    "A",         "B",     "X",    "Y",  "LT", "RT"};
int pad_from_name(const std::string& s) {
  if (_stricmp(s.c_str(), "none") == 0) return 10;  // a wButtons bit XInput never sets
  for (int i = 0; i < 18; i++)
    if (_stricmp(s.c_str(), kPadNames[i]) == 0) return i;
  return -1;
}
// Action names (the game's input action enum, static: the name table built by 0x19e20).
const char* action_name(int a) {
  static const char* k[] = {
      "MOVEMENT_X", "MOVEMENT_Y", "JUMP", "SHOOT", "RELOAD", "CAMERA_X", "CAMERA_Y", "CAMERA_FORWARD",
      "CAMERA_BACKWARD", "FREECAMERA_X", "FREECAMERA_Z", "FREECAMERA_HEADING", "FREECAMERA_PITCH", "FREECAMERA_UP",
      "FREECAMERA_DOWN", "FREECAMERA_FASTER", "FREECAMERA_SLOWER", "FREECAMERA_SCALEUP", "FREECAMERA_SCALEDOWN",
      "FREECAMERA_ZOOM_IN", "FREECAMERA_ZOOM_OUT", "TURBOBOOST", "TOGGLE_SPRINT", "ACCELERATE", "REVERSE", "STEER",
      "VEHICLE_YAW_X", "VEHICLE_YAW_Y", "VEHICLE_YAW_LEFT", "VEHICLE_YAW_RIGHT", "VEHICLE_PITCH",
      "VEHICLE_CENTRE_CAMERA", "TOW_VEHICLE", "ABORT_VIDEO1", "ABORT_VIDEO2", "ABORT_VIDEO3", "ABORT_VIDEO4",
      "ABORT_VIDEO_CONFIRM", "TOGGLE_MANUSCRIPT", "TOGGLE_MENU", "TOGGLE_LIGHTSOURCE", "INVENTORY_UP",
      "INVENTORY_RIGHT", "INVENTORY_DOWN", "INVENTORY_LEFT", "INVENTORY_EXTRA", "USE", "USE_CONFIRM",
      "USE_TRIGGER_CONFIRM", "PUTDOWN", "THROW", "THROW_ITEM", "HOLD_FLARE", "PLANT", "PUSH_BACK", "TOGGLE_SIDE",
      "RELOAD_BATTERY", "LIGHT_BOOST", "LIGHT_BOOST_VEHICLE", "REDIRECTED_LX", "REDIRECTED_LY", "REDIRECTED_RX",
      "REDIRECTED_RY", "REDIRECTED_BOOST", "ABORT_REDIRECTION_MODE", "REDIRECT_MODE_CONFIRM", "CROSS_UP",
      "CROSS_DOWN", "CROSS_LEFT", "CROSS_RIGHT", "EXIT_MINIGAME", "MINIGAME_ACTION", "MINIGAME_DEBUG_1",
      "TUTORIAL_PADUP", "TUTORIAL_PADDOWN", "TUTORIAL_LEFTTHUMBY", "TUTORIAL_SELECT", "LOOK_AT"};
  if (a >= 0 && a < int(sizeof k / sizeof *k)) return k[a];
  if (a >= 0x9f && a <= 0xa3) {
    static const char* m[] = {"MOVE_FORWARD", "MOVE_BACKWARD", "MOVE_LEFT", "MOVE_RIGHT", "WALK"};
    return m[a - 0x9f];
  }
  return "";
}
int action_from_name(const std::string& s) {
  for (int a = 0; a < 0xa4; a++)
    if (*action_name(a) && _stricmp(s.c_str(), action_name(a)) == 0) return a;
  return -1;
}

struct Move {
  int action, button;
};
struct Params {
  bool deliberate = true;
  double holster_delay = 3.0;
  std::vector<unsigned> key_boost{'V'};
  std::vector<unsigned> key_flashlight{'T'};
  int pad_boost = 9;       // RB
  int pad_flashlight = 0;  // D-pad up
  // pad actions moved while the scheme applies (RB's throw to R3, whose shoulder swap means
  // nothing in first person; D-pad up's long-gun select to L3, next to the contextual LOOK_AT)
  std::vector<Move> moves{{0x33, 7}, {0x34, 7}, {0x37, 10}, {0x29, 6}};
  int moves_gen = 0;
};
std::mutex g_pmu;
Params g_p;
Params params() {
  std::lock_guard lk(g_pmu);
  return g_p;
}
std::vector<unsigned> parse_keys(const std::string& v) {
  std::vector<unsigned> o;
  size_t i = 0;
  while (i <= v.size()) {
    size_t j = v.find(',', i);
    if (j == std::string::npos) j = v.size();
    std::string t = v.substr(i, j - i);
    GameKey k;
    if (key_from_name(t, &k) && k.vk_mem && k.vk_mem < 256) o.push_back(k.vk_mem);
    i = j + 1;
  }
  return o;
}
std::vector<Move> parse_moves(const std::string& v) {
  std::vector<Move> o;
  size_t i = 0;
  while (i <= v.size()) {
    size_t j = v.find(',', i);
    if (j == std::string::npos) j = v.size();
    std::string t = v.substr(i, j - i);
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    size_t c = t.find(':');
    if (c != std::string::npos) {
      int a = action_from_name(t.substr(0, c)), b = pad_from_name(t.substr(c + 1));
      if (a >= 0 && b >= 0) o.push_back({a, b});
      else if (!t.empty()) TB_LOG("fpcontrols: pad_moves: '%s' not understood (ACTION:Button)", t.c_str());
    }
    i = j + 1;
  }
  return o;
}

// ---- inputs ------------------------------------------------------------------------------------
bool kb_on() { return !*g_in_disabled && *g_in_kbmode; }
bool key_held(unsigned vk) {
  uint8_t* ks = g_kbstate ? *g_kbstate : nullptr;
  return ks && vk < 256 && (ks[0x1c + vk] & 1);
}
void* pad_device() {
  void** devs = g_pad_devices ? *g_pad_devices : nullptr;
  if (!devs) return nullptr;
  void* d = devs[*g_pad_index];
  if (!d || !readable(d, 0x80)) return nullptr;
  if (*static_cast<uintptr_t*>(d) != game_image().at(RVA_VT_GAMEPAD)) return nullptr;
  return rd<uint8_t>(d, 0x50) ? d : nullptr;  // connected
}
bool pad_held(int b) {
  void* d = pad_device();
  if (!d || b < 0) return false;
  if (b < 16) return rd<uint16_t>(d, 0x70) >> b & 1;
  if (b == 16) return rd<uint8_t>(d, 0x72) > 30;
  if (b == 17) return rd<uint8_t>(d, 0x73) > 30;
  return false;
}

// ---- state (game thread) -----------------------------------------------------------------------
struct State {
  bool in_use = false;       // the scheme applies (first person active, deliberate, player reader)
  bool drawn = false;        // the gun is out
  double timer = 0;          // s left before it is holstered again
  double drawn_age = 0;      // s since it was drawn (shots wait for the draw)
  double draw = 1;           // 0..1 draw blend for the viewmodel
  bool boost = false, aim_raw = false, fire_raw = false;
  bool prev_toggle = false;
  bool we_switched_off = false;  // the flashlight is off because of us
  void* fl_off = nullptr;        // ... that flashlight
  uint64_t toggles = 0, draws = 0, holsters = 0, blocked_frames = 0, updates = 0;
  // pad table patches: entry address, original button, ours
  struct Patch {
    uint8_t* entry;
    int orig, ours;
  };
  std::vector<Patch> patches;
  int patches_gen = -1;
} g_s;
std::mutex g_smu;  // the lab verb reads g_s
std::atomic<double> g_draw{1.0};
std::atomic<bool> g_in_use{false};

double game_dt() {
  float dt = reinterpret_cast<float(__fastcall*)()>(game_image().at(0x1df650))();
  return dt > 0 && dt < 0.25 ? dt : dt >= 0.25 ? 0.25 : 0;
}

bool has_weapon(void* ch) {
  void* wp = rd<void*>(ch, kCharWeapon);
  return wp && rd<uint8_t>(wp, 0x30) && rd<void*>(wp, 0x38);
}

// The pad table: our buttons' actions moved away (`pad_moves`), put back when the scheme stops.
void pad_patch(bool on, const Params& p) {
  uint8_t* t = g_pad_table ? *g_pad_table : nullptr;
  // undo: only entries still holding our value (the game's own apply 0x3fec00 may have swapped)
  if (!on || !t) {
    for (auto& x : g_s.patches)
      if (rd<int>(x.entry, 8) == x.ours) memcpy(x.entry + 8, &x.orig, 4);
    g_s.patches.clear();
    return;
  }
  if (g_s.patches_gen == p.moves_gen && !g_s.patches.empty() && g_s.patches.front().entry >= t &&
      g_s.patches.front().entry < t + 0xa4 * 0x18) {
    for (auto& x : g_s.patches)
      if (rd<int>(x.entry, 8) != x.ours) memcpy(x.entry + 8, &x.ours, 4);  // swapped back by the game
    return;
  }
  pad_patch(false, p);
  g_s.patches_gen = p.moves_gen;
  for (const Move& m : p.moves) {
    uint8_t* e = t + size_t(m.action) * 0x18;
    int type = rd<int>(e, 0);
    if (type < 5 || type > 10) continue;  // digital entries only
    int orig = rd<int>(e, 8);
    g_s.patches.push_back({e, orig, m.button});
    memcpy(e + 8, &m.button, 4);
  }
}

void switch_flashlight(void* fl, bool on) {
  if (fl && rd<int>(fl, 0xb8) != -1) g_switch(fl, on ? 1 : 0);
}

// Before the player's input reader runs: the scheme's state for this update.
void pre_update(void* reader) {
  void* ch = rd<void*>(reader, 8);
  Params p = params();
  bool use = g_ok && p.deliberate && ch && ch == fp_player_character() && fp_active_now();
  std::lock_guard lk(g_smu);
  g_s.updates++;
  if (!use) {
    if (g_s.in_use) {
      pad_patch(false, p);
      if (g_s.we_switched_off && g_s.fl_off == (ch ? rd<void*>(ch, kCharFlashlight) : nullptr) &&
          !rd<uint8_t>(g_s.fl_off, 0x38))
        switch_flashlight(g_s.fl_off, true);
      g_s.we_switched_off = false;
      g_s.in_use = false;
      // the gun's mesh back to the game's own state (hidden by us while holstered); hide = all in
      // first person re-hides it itself
      void* wp = ch ? rd<void*>(ch, kCharWeapon) : nullptr;
      void* ro = wp && rd<uint8_t>(wp, 0x30) ? rd<void*>(wp, 0x38) : nullptr;
      if (ro && (rd<uint32_t>(ro, 0x24) & 8) && !rd<uint8_t>(wp, 0x1c) && !(fp_active_now() && fp_hide_all()))
        reinterpret_cast<void(__fastcall*)(void*, uint32_t, char)>(game_image().at(0x463180))(ro, 8, 0);
    }
    g_in_use = false;
    g_draw = 1.0;
    return;
  }
  if (!g_s.in_use) {  // first person (deliberate) starts: holstered, nothing pressed
    g_s.in_use = true;
    g_s.drawn = false;
    g_s.timer = 0;
    g_s.draw = 0;
    g_s.prev_toggle = true;  // a key held across the switch does not toggle
  }
  pad_patch(true, p);
  double dt = game_dt();
  bool kb = kb_on();
  bool boost = pad_held(p.pad_boost);
  for (unsigned vk : p.key_boost) boost |= kb && key_held(vk);
  g_s.boost = boost;
  g_s.aim_raw = g_o_aim(reader) != 0;
  g_s.fire_raw = g_o_shoot_pressed(nullptr) != 0;
  bool reload = (kb && reinterpret_cast<char(__fastcall*)(int)>(game_image().at(RVA_IN_KBHELD))(kActReload)) ||
                reinterpret_cast<float(__fastcall*)(int)>(game_image().at(RVA_IN_AXIS))(kActReload) > 0.5f;
  // the flashlight switch (edge)
  bool toggle = pad_held(p.pad_flashlight);
  for (unsigned vk : p.key_flashlight) toggle |= kb && key_held(vk);
  if (toggle && !g_s.prev_toggle) {
    void* fl = rd<void*>(ch, kCharFlashlight);
    if (fl && rd<int>(fl, 0xb8) != -1) {
      bool on = rd<uint8_t>(fl, 0x38) != 0;
      switch_flashlight(fl, !on);
      g_s.we_switched_off = on;
      g_s.fl_off = fl;
      g_s.toggles++;
      TB_LOG("fpcontrols: flashlight %s", on ? "off" : "on");
    }
  }
  g_s.prev_toggle = toggle;
  // the gun: drawn by aim, fire or reload; holstered holster_delay s after the last of them
  bool weapon = has_weapon(ch);
  if (g_s.aim_raw || g_s.fire_raw || reload) {
    if (!g_s.drawn) {
      g_s.drawn = true;
      g_s.drawn_age = 0;
      g_s.draws++;
    }
    g_s.timer = p.holster_delay;
  } else if (g_s.drawn) {
    g_s.timer -= dt;
    if (g_s.timer <= 0) {
      g_s.drawn = false;
      g_s.holsters++;
    }
  }
  if (g_s.drawn) g_s.drawn_age += dt;
  if (!weapon) g_s.drawn = false;
  double a = dt > 0 ? 1.0 - std::exp(-dt / 0.07) : 1.0;  // ~0.2 s to settle
  g_s.draw += ((g_s.drawn ? 1.0 : 0.0) - g_s.draw) * a;
  if (std::fabs(g_s.draw - (g_s.drawn ? 1.0 : 0.0)) < 0.01) g_s.draw = g_s.drawn ? 1.0 : 0.0;
  g_draw = weapon ? g_s.draw : 1.0;
  g_in_use = true;
}

// After the reader: the holstered gun's mesh hidden once the arm is out of view; shown again
// (the game's own hidden state, WeaponHolder +0x1c) as soon as it is drawn.
void post_update(void* reader) {
  if (!g_in_use.load(std::memory_order_relaxed)) return;
  void* ch = rd<void*>(reader, 8);
  void* wp = ch ? rd<void*>(ch, kCharWeapon) : nullptr;
  if (!wp || !rd<uint8_t>(wp, 0x30)) return;
  void* ro = rd<void*>(wp, 0x38);
  if (!ro) return;
  bool hide = g_draw.load() < 0.05 || fp_hide_all() || rd<uint8_t>(wp, 0x1c);
  uint32_t f = rd<uint32_t>(ro, 0x24);
  bool hidden = f & 8;
  if (hide != hidden)
    reinterpret_cast<void(__fastcall*)(void*, uint32_t, char)>(game_image().at(0x463180))(ro, 8, hide ? 1 : 0);
}

bool fire_allowed() {
  if (!g_in_use.load(std::memory_order_relaxed)) return true;
  std::lock_guard lk(g_smu);
  return g_s.drawn && g_s.drawn_age >= 0.2;
}

// ---- hooks ---------------------------------------------------------------------------------------
void __fastcall hk_reader(void* reader) {
  pre_update(reader);
  g_o_reader(reader);
  post_update(reader);
}
bool is_player_reader(void* reader) {
  return reader && rd<void*>(reader, 8) == fp_player_character();
}
char __fastcall hk_aim(void* reader) {
  char r = g_o_aim(reader);
  if (!g_in_use.load(std::memory_order_relaxed) || !is_player_reader(reader)) return r;
  // aim (draws the gun) or boost (raises the flashlight); both need the game's boost permission
  // (Flashlight +0x158, which the original checks too)
  void* fl = rd<void*>(rd<void*>(reader, 8), kCharFlashlight);
  bool allowed = fl && rd<uint8_t>(fl, 0x158);
  std::lock_guard lk(g_smu);
  return (r || (g_s.boost && allowed)) ? 1 : 0;
}
char __fastcall hk_boost(void* reader) {
  char r = g_o_boost(reader);
  if (!g_in_use.load(std::memory_order_relaxed) || !is_player_reader(reader)) return r;
  void* fl = rd<void*>(rd<void*>(reader, 8), kCharFlashlight);
  bool allowed = fl && rd<uint8_t>(fl, 0x158);
  std::lock_guard lk(g_smu);
  return g_s.boost && allowed ? 1 : 0;
}
// ShootInput: [this] = Character*; while the gun is holstered (or still coming up) it stays idle.
void __fastcall hk_shoot_update(void* si) {
  if (!si || fire_allowed() || rd<void*>(si, 0) != fp_player_character()) return g_o_shoot_update(si);
  auto* b = static_cast<uint8_t*>(si);
  memset(b + 8, 0, 2);  // fire latches
  float z = 0;
  memcpy(b + 0x10, &z, 4);
  b[0xb] = 0;  // "held last update": a fire still held when the gun is up fires
  std::lock_guard lk(g_smu);
  g_s.blocked_frames++;
}
char __fastcall hk_shoot_pressed(void* any) {
  char r = g_o_shoot_pressed(any);
  return r && !fire_allowed() ? 0 : r;
}

std::string state_json() {
  Params p = params();
  std::lock_guard lk(g_smu);
  char b[900];
  snprintf(b, sizeof b,
           "{\"ok\":%s,\"controls\":\"%s\",\"in_use\":%s,\"drawn\":%s,\"timer\":%.2f,\"draw\":%.2f,\"boost\":%s,"
           "\"aim_raw\":%s,\"fire_raw\":%s,\"fire_allowed\":%s,\"flashlight_off_by_us\":%s,\"toggles\":%llu,"
           "\"draws\":%llu,\"holsters\":%llu,\"blocked_frames\":%llu,\"updates\":%llu,\"pad\":%s,"
           "\"pad_patches\":%zu,\"kb\":%s}",
           g_ok ? "true" : "false", p.deliberate ? "deliberate" : "game", g_s.in_use ? "true" : "false",
           g_s.drawn ? "true" : "false", g_s.timer, g_s.draw, g_s.boost ? "true" : "false",
           g_s.aim_raw ? "true" : "false", g_s.fire_raw ? "true" : "false",
           (!g_s.in_use || (g_s.drawn && g_s.drawn_age >= 0.2)) ? "true" : "false",
           g_s.we_switched_off ? "true" : "false", (unsigned long long)g_s.toggles, (unsigned long long)g_s.draws,
           (unsigned long long)g_s.holsters, (unsigned long long)g_s.blocked_frames,
           (unsigned long long)g_s.updates, pad_device() ? "true" : "false", g_s.patches.size(),
           g_ok && kb_on() ? "true" : "false");
  return b;
}

// The pad table as the game has it now: action, type, mask, button.
std::string pad_json() {
  uint8_t* t = g_pad_table ? *g_pad_table : nullptr;
  if (!t || !readable(t, 0xa4 * 0x18)) return json_str("no pad table");
  std::string o = "[";
  int k = 0;
  for (int a = 0; a < 0xa4; a++) {
    const uint8_t* e = t + size_t(a) * 0x18;
    int type = rd<int>(e, 0), b = rd<int>(e, 8);
    if (!type) continue;
    char s[200];
    snprintf(s, sizeof s, "%s[\"0x%x %s\",%d,\"0x%x\",\"%s\"]", k++ ? "," : "", a, action_name(a), type,
             rd<uint32_t>(e, 4), type >= 5 && type <= 10 && b >= 0 && b < 18 ? kPadNames[b] : std::to_string(b).c_str());
    o += s;
  }
  return o + "]";
}

}  // namespace

double fpc_draw() { return g_draw.load(std::memory_order_relaxed); }
bool fpc_in_use() { return g_in_use.load(std::memory_order_relaxed); }

void fpcontrols_register() {
  cfg_listen("camera.", [](const std::string& id, const std::string& v) {
    std::lock_guard lk(g_pmu);
    double n = strtod(v.c_str(), nullptr);
    if (id == "camera.controls") g_p.deliberate = v != "game";
    if (id == "camera.holster_delay") g_p.holster_delay = n < 0 ? 0 : n > 60 ? 60 : n;
    if (id == "camera.key_boost") g_p.key_boost = parse_keys(v);
    if (id == "camera.key_flashlight") g_p.key_flashlight = parse_keys(v);
    if (id == "camera.pad_boost") g_p.pad_boost = v.empty() ? 9 : pad_from_name(v);
    if (id == "camera.pad_flashlight") g_p.pad_flashlight = v.empty() ? 0 : pad_from_name(v);
    if (id == "camera.pad_moves") g_p.moves = parse_moves(v), g_p.moves_gen++;  // empty: nothing moves
  });
  // `quiver lab call <i> fpc [state|pad]`
  lab_verb("fpc", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    if (a.empty() || a[0] == "state") return state_json();
    if (a[0] == "pad") return pad_json();
    ok = false;
    return json_str("usage: fpc [state|pad]");
  });
}

void fpcontrols_start() {
  for (auto& s : kSigs)
    if (!sig_ok(s.rva, s.hex)) {
      TB_LOG("fpcontrols: signature mismatch at rva 0x%zx; controls = deliberate unavailable (the game's "
             "controls)",
             (size_t)s.rva);
      return;
    }
  const auto& img = game_image();
  g_kbstate = static_cast<uint8_t**>(rip_target(img.at(0x3fe3eb), 3, 7));
  g_pad_index = static_cast<int*>(rip_target(img.at(RVA_IN_PAD) + 0xe, 3, 7));
  g_pad_devices = static_cast<void***>(rip_target(img.at(RVA_IN_PAD) + 0x15, 3, 7));
  g_pad_table = static_cast<uint8_t**>(rip_target(img.at(RVA_IN_PAD) + 0x2c, 3, 7));
  g_in_disabled = static_cast<const uint8_t*>(rip_target(img.at(RVA_IN_DISABLED), 3, 7));
  g_in_kbmode = static_cast<const uint8_t*>(rip_target(img.at(RVA_IN_KBMODE), 3, 7));
  for (const void* p : {(const void*)g_kbstate, (const void*)g_pad_index, (const void*)g_pad_devices,
                        (const void*)g_pad_table, (const void*)g_in_disabled, (const void*)g_in_kbmode})
    if (!img.contains(reinterpret_cast<uintptr_t>(p))) {
      TB_LOG("fpcontrols: a global resolves outside the image; controls = deliberate unavailable");
      return;
    }
  g_switch = reinterpret_cast<SwitchFn>(img.at(RVA_FL_SWITCH));
  struct H {
    uintptr_t rva;
    void* detour;
    void** orig;
    const char* name;
  } hooks[] = {
      {RVA_AIM_PRED, (void*)&hk_aim, (void**)&g_o_aim, "PlayerInputReader aiming"},
      {RVA_BOOST_PRED, (void*)&hk_boost, (void**)&g_o_boost, "PlayerInputReader boosting"},
      {RVA_SHOOT_UPDATE, (void*)&hk_shoot_update, (void**)&g_o_shoot_update, "ShootInput::update"},
      {RVA_SHOOT_PRESSED, (void*)&hk_shoot_pressed, (void**)&g_o_shoot_pressed, "ShootInput pressed"},
      {RVA_READER_UPDATE, (void*)&hk_reader, (void**)&g_o_reader, "PlayerInputReader::update"},
  };
  for (auto& h : hooks)
    if (!hook_fn(reinterpret_cast<void*>(img.at(h.rva)), h.detour, h.orig, h.name)) {
      // the hooks queued so far stay in and pass through (g_ok false)
      TB_LOG("fpcontrols: hook %s failed; controls = deliberate unavailable", h.name);
      return;
    }
  g_ok = true;
  TB_LOG("fpcontrols: ready");
}

}  // namespace tb
