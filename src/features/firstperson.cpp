// First-person camera (docs/first-person.md).
//
// Render-side only: the game keeps simulating its own third-person camera (PlayerCamera and the
// "Cameraman camera" BaseCamera it feeds), and every gameplay system that reads the camera (aim,
// movement relative to the camera, AI, sound listener) reads BaseCamera's float transform at
// +0xcc8, which is left alone. What changes is the transform the renderer gets: a camera filter
// (core/camera.h, run from stereo's View::setViewToWorld hook before any eye offset) moves the
// main view to the player's eyes, keeping the game camera's orientation. The player's body is
// hidden with the game's own per-character hide mask, the bit its debug panel's "Hide character"
// uses (Character update re-applies it every frame, so our hook only has to OR it in).
//
// Active only while the player camera is the game's camera (camera mode 0), the view being set is
// the Cameraman camera, a player exists and is not in a vehicle; anything else (cutscene /
// cinematic camera, free camera, death camera, driving) renders the game's camera unchanged.
#include "features/firstperson.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/camera.h"
#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"

namespace tb {
namespace {

// ---- game addresses (RVAs, image base 0x140000000; docs/first-person.md) --------------------
constexpr uintptr_t RVA_GET_PLAYERS = 0x1873a0;   // `mov rax,[rip+d]; ret`: player manager (+0x50 Character*)
constexpr uintptr_t RVA_GET_CAMMODE = 0x1d2540;   // `mov eax,[rip+d]; ret`: CameraManager mode
constexpr uintptr_t RVA_ACTIVE_CAM = 0x1d2430;    // CameraManager::active(withEffects), the mode-0 branch
constexpr uintptr_t RVA_BONE_SLOT = 0x179f10;     // Character::slotTransform(this, float out[12], slot)
constexpr uintptr_t RVA_HAS_VEHICLE = 0x159d60;   // VehicleLink::has(link): handle resolves
constexpr uintptr_t RVA_HIDE_MASK = 0x1ef4f0;     // CharacterHide::set(this, bool on, u32 bits)
constexpr uintptr_t RVA_DEBUG_HIDE = 0x17e716;    // Character update: debug "Hide character" -> bit 8
constexpr uintptr_t RVA_FL_UPDATE = 0x22f370;     // Flashlight::updateLights(this): beam on iff !this[0]
constexpr uintptr_t RVA_FL_HIDE = 0x22cb60;       // Flashlight::setHidden(this, bool): mesh + this[0]
constexpr uintptr_t RVA_WEAPON_HIDE = 0x340340;   // WeaponHolder::setHidden(this, bool): mesh

constexpr int kSlotHead = 0x1d;            // head slot (FUN_14017a420 "head heading", Taken head shots)
constexpr int kSlotGround = 30;            // straight below the root at the feet (live census)
constexpr size_t kCharTransform = 0xbc;    // float[12] world transform (rotation rows, position)
constexpr size_t kCharVehicle = 0x218;     // vehicle link (+0xc handle, +0x8 driving)
constexpr size_t kCharHide = 0x210;        // CharacterHide* (+0 Character*, +8 mask, +0xc applied)
constexpr size_t kCharIsPlayer = 0x1c9;    // u8, set on the player character
constexpr uint32_t kHideDebugBit = 8;      // the debug panel's bit: hides meshes, keeps physics
// PerspectiveView parameters (floats), as in stereo/engine.h.
constexpr size_t kViewFov = 0xcb0, kViewAspect = 0xcb8, kViewNear = 0xcbc;
constexpr size_t kViewDirtyProj = 0x4a8, kViewDirtyWorld = 0x4a9;

struct Sig {
  uintptr_t rva;
  const char* hex;
};
const Sig kSigs[] = {
    {RVA_GET_PLAYERS, "48 8b 05 ?? ?? ?? ?? c3"},
    {RVA_GET_CAMMODE, "8b 05 ?? ?? ?? ?? c3"},
    // call FUN_140172bf0; mov rcx,rax; call FUN_140164db0; mov rcx,[rip -> Cameraman camera];
    // mov rdx,rax; call BaseCamera::setTransform
    {RVA_ACTIVE_CAM, "e8 bb 07 fa ff 48 8b c8 e8 73 29 f9 ff 48 8b 0d ?? ?? ?? ?? 48 8b d0 e8 c4 bd f8 ff"},
    {RVA_BONE_SLOT, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b 01 41 8b f0 48 8b da 48 8b f9 ff 90 c8 02 00 00 84 c0 74 4e 48 8b 8f b8 01 00 00"},
    {RVA_HAS_VEHICLE, "8b 41 0c 85 c0 75 09 33 c0 48 85 c0 0f 95 c0 c3"},
    {RVA_HIDE_MASK, "48 89 5c 24 20 57 48 83 ec 20 48 8b d9 8b 49 08 41 8b f8 84 d2 74 05 41 0b c8 eb 06 8b c7 f7 d0"},
    // mov rax,[rip -> AnimationPage]; cmp byte [rax+0x146],0; setnz dl; mov r8d,8;
    // mov rcx,[rsi+0x210]; call CharacterHide::set
    {RVA_DEBUG_HIDE, "48 8b 05 ?? ?? ?? ?? 80 b8 46 01 00 00 00 0f 95 c2 41 b8 08 00 00 00 48 8b 8e 10 02 00 00 e8"},
    // Character::slotTransform's helpers: pose = [rcx+8] (0x184160), joint = [pose+slot*4+0x50]
    {0x184160, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b da 48 8b f1 48 8b 49 08 41 8b d0 41 8b f8 e8"},
    {0x414a30, "48 63 c2 8b 44 81 50 c3"},
    // test aid (fp freecam): free-camera toggle and CameraManager::setMode
    {0x1d2b40, "48 89 5c 24 08 57 48 83 ec 20 48 8b 05 ?? ?? ?? ?? 80 b8 1a 01 00 00 00"},
    {0x1d20f0, "40 57 48 83 ec 20 8b 05 ?? ?? ?? ?? 8b f9 83 f8 07 75 58 84 d2 75 54 e8"},
    {RVA_FL_UPDATE,"48 8b c4 55 53 56 57 48 8d 68 a1 48 81 ec 88 00 00 00 0f 29 78 b8 40 32"},
    {RVA_FL_HIDE, "48 89 5c 24 08 57 48 83 ec 20 80 b9 c0 00 00 00 00 0f b6 fa 48 8b d9 74"},
    {RVA_WEAPON_HIDE, "80 79 30 00 88 51 1c 74 12 48 8b 49 38 44 0f b6 c2 ba 08 00 00 00 e9"},
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

// Address of the global a rip-relative instruction at `insn` (with its disp32 at insn+dispoff and
// length len) refers to.
void* rip_target(uintptr_t insn, int dispoff, int len) {
  int32_t d;
  memcpy(&d, reinterpret_cast<const void*>(insn + dispoff), 4);
  return reinterpret_cast<void*>(insn + len + d);
}

using BoneSlotFn = void(__fastcall*)(void* character, float* out, int slot);
using HasVehicleFn = char(__fastcall*)(void* link);
using HideMaskFn = void(__fastcall*)(void* hide, char on, uint32_t bits);
using Obj1Fn = void(__fastcall*)(void* obj);
using ObjBoolFn = void(__fastcall*)(void* obj, char on);
constexpr size_t kCharFlashlight = 0x230;  // Flashlight* (FUN_14017a660)
constexpr size_t kCharWeapon = 0x220;      // weapon holder (FUN_14017ae20)

struct Game {
  bool ok = false;
  void** players = nullptr;
  volatile int* cam_mode = nullptr;
  void** cameraman = nullptr;  // the "Cameraman camera" BaseCamera: the rendered camera in mode 0
  BoneSlotFn bone_slot = nullptr;
  HasVehicleFn has_vehicle = nullptr;
  HideMaskFn o_hide = nullptr;
  Obj1Fn fl_update = nullptr;
  ObjBoolFn fl_hide = nullptr;
  ObjBoolFn weapon_hide = nullptr;
} G;

// ---- config --------------------------------------------------------------------------------
enum class Anchor { Head, Body };
struct Params {
  bool first_person = false;
  Anchor anchor = Anchor::Head;
  double eye_up = 0.05;       // m above the head slot (world up)
  double eye_forward = 0.10;  // m ahead of it, along the camera's horizontal forward
  double eye_height = 1.70;   // anchor = body: m above the character's root
  enum class Hide { None, Body, All } hide = Hide::All;
  double fov_deg = 0;         // horizontal FOV at 16:9, degrees; 0 = the game's own
  double smooth = 0.1;        // s: low-pass time constant on the head's offset from the root
  double aim_distance = 15;   // m: >0 = look at the point this far along the game camera's ray
};
std::mutex g_pmu;
Params g_p;
std::atomic<bool> g_on{false};  // fast check: mode = first_person
Params params() {
  std::lock_guard lk(g_pmu);
  return g_p;
}

// ---- state (game thread) ---------------------------------------------------------------------
std::atomic<bool> g_active{false};       // first person was applied on the last main-view set
std::atomic<int> g_reason{0};
const char* kReasons[] = {"off", "active", "not the player camera (camera mode != 0)", "no player",
                          "player in a vehicle", "head slot unavailable",
                          "scripted shot (game camera away from the player)"};
// Scripted shots inside the player camera (its Rail / Path / Attached / Reveal camera modes, e.g.
// the helicopter fly-by in level 15) move the game camera well away from Alan while the player
// keeps control; the follow camera stays within about 3 m of the head (live). Beyond this, the
// game's own shot is shown.
constexpr double kScriptedShotDistance = 6.0;
std::atomic<uint64_t> g_filter_calls{0}, g_hide_calls{0}, g_hide_forced{0};
uint64_t g_cache_frame = ~0ull;
double g_eye[3], g_head[3], g_root[3], g_cam[3];
double g_smooth_off[3];
bool g_smooth_valid = false;
LARGE_INTEGER g_last_qpc{};

void* player() {
  void* pm = G.players ? *G.players : nullptr;
  return pm ? *reinterpret_cast<void**>(static_cast<uint8_t*>(pm) + 0x50) : nullptr;
}

template <typename T>
T rd(const void* b, size_t off) {
  T v;
  memcpy(&v, static_cast<const uint8_t*>(b) + off, sizeof v);
  return v;
}

int why_inactive(void* view, void*& ch) {
  ch = nullptr;
  if (!G.ok || !g_on.load(std::memory_order_relaxed)) return 0;
  if (*G.cam_mode != 0) return 2;
  ch = player();
  if (!ch || !rd<uint8_t>(ch, kCharIsPlayer)) return 3;
  void* link = static_cast<uint8_t*>(ch) + kCharVehicle;
  if (G.has_vehicle(link)) return 4;
  (void)view;
  return 1;
}

// What Character::slotTransform checks before it reads a slot: the character has an animated
// skeleton (vtable +0x2c8) and the pose's slot table (+0x1b8 -> +8 -> +0x50, int per slot) maps
// the slot to a joint (static: 0x179f10 -> 0x184160 -> 0x414a30).
bool has_slot(void* ch, int slot) {
  auto vt = *static_cast<void***>(ch);
  if (!reinterpret_cast<char(__fastcall*)(void*)>(vt[0x2c8 / 8])(ch)) return false;
  void* anim = rd<void*>(ch, 0x1b8);
  void* pose = anim ? rd<void*>(anim, 8) : nullptr;
  return pose && rd<int>(pose, 0x50 + size_t(slot) * 4) != -1;
}

double dt_seconds() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  double dt = g_last_qpc.QuadPart ? double(c.QuadPart - g_last_qpc.QuadPart) / double(f.QuadPart) : 0;
  g_last_qpc = c;
  return dt > 0.25 ? 0.25 : dt;
}

// The eye position for this frame (computed once per presented frame, so every set of the main
// view within a frame agrees).
bool eye_position(void* ch, const double* m, const Params& p) {
  uint64_t f = present_frame();
  if (f == g_cache_frame) return true;
  const float* root = reinterpret_cast<const float*>(static_cast<uint8_t*>(ch) + kCharTransform);
  for (int i = 0; i < 3; i++) g_root[i] = root[9 + i];
  for (int i = 0; i < 3; i++) g_cam[i] = m[9 + i];
  double off[3];
  if (p.anchor == Anchor::Head) {
    if (!has_slot(ch, kSlotHead)) return false;  // the getter would hand back the root
    float b[12];
    memcpy(b, root, sizeof b);
    G.bone_slot(ch, b, kSlotHead);
    for (int i = 0; i < 3; i++) g_head[i] = b[9 + i];
    for (int i = 0; i < 3; i++) off[i] = g_head[i] - g_root[i];
  } else {
    // The character's transform sits about 1.08 m above its feet (the capsule's centre, live);
    // slot 30 is the ground point below it.
    float b[12];
    memcpy(b, root, sizeof b);
    G.bone_slot(ch, b, kSlotGround);
    double feet = b[10] - root[10];
    if (!(feet < -0.5 && feet > -1.6)) feet = -1.08;
    off[0] = off[2] = 0;
    off[1] = feet + p.eye_height;
    for (int i = 0; i < 3; i++) g_head[i] = g_root[i] + off[i];
  }
  double dt = dt_seconds();
  if (p.smooth > 0 && g_smooth_valid && dt > 0) {
    double a = 1.0 - std::exp(-dt / p.smooth);
    for (int i = 0; i < 3; i++) g_smooth_off[i] += (off[i] - g_smooth_off[i]) * a;
  } else {
    memcpy(g_smooth_off, off, sizeof off);
  }
  g_smooth_valid = true;
  // Horizontal camera forward (rows: right 0..2, up 3..5, forward 6..8).
  double fx = m[6], fz = m[8], n = std::sqrt(fx * fx + fz * fz);
  if (n > 1e-6) fx /= n, fz /= n;
  else fx = fz = 0;
  g_eye[0] = g_root[0] + g_smooth_off[0] + fx * p.eye_forward;
  g_eye[1] = g_root[1] + g_smooth_off[1] + (p.anchor == Anchor::Head ? p.eye_up : 0);
  g_eye[2] = g_root[2] + g_smooth_off[2] + fz * p.eye_forward;
  g_cache_frame = f;
  return true;
}

void cross(const double* a, const double* b, double* o) {
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}
void normalize(double* v) {
  double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (n > 1e-9) v[0] /= n, v[1] /= n, v[2] /= n;
}

// `fp slots N` census, answered on the game thread.
std::mutex g_slots_mu;
std::atomic<int> g_slots_req{0};
std::string g_slots_out;
void slots_census() {
  std::lock_guard lk(g_slots_mu);
  void* ch = player();
  std::string o = "[";
  if (ch) {
    const float* root = reinterpret_cast<const float*>(static_cast<uint8_t*>(ch) + kCharTransform);
    for (int s = 0; s < g_slots_req; s++) {
      float b[12];
      memcpy(b, root, sizeof b);
      G.bone_slot(ch, b, s);
      char t[96];
      snprintf(t, sizeof t, "%s[%d,%.3f,%.3f,%.3f]", s ? "," : "", s, b[9] - root[9], b[10] - root[10],
               b[11] - root[11]);
      o += t;
    }
  }
  g_slots_out = o + "]";
  g_slots_req = 0;
}

// `fp freecam on|off` (test aid for the fallback): the debug free camera toggle (0x1d2b40, the
// TOGGLE_FREECAMERA action: player camera -> free camera at the current pose) and
// CameraManager::setMode(0) (0x1d20f0) back. Run on the game thread from the filter.
std::atomic<int> g_freecam_req{-1};
void freecam_apply() {
  int r = g_freecam_req.exchange(-1);
  if (r == 1 && *G.cam_mode == 0) reinterpret_cast<void(__fastcall*)()>(game_image().at(0x1d2b40))();
  if (r == 0) reinterpret_cast<void(__fastcall*)(int, char)>(game_image().at(0x1d20f0))(0, 0);
}

// core/camera.h filter: the main view's transform -> the player's eyes.
bool fp_filter(void* view, double* m) {
  if (g_slots_req.load(std::memory_order_relaxed) && G.ok && G.cameraman && view == *G.cameraman) slots_census();
  if (g_freecam_req.load(std::memory_order_relaxed) >= 0 && G.ok) freecam_apply();
  if (!g_on.load(std::memory_order_relaxed)) {
    if (g_active.exchange(false)) g_reason = 0;
    return false;
  }
  if (!G.cameraman || view != *G.cameraman) return false;
  g_filter_calls++;
  void* ch;
  int r = why_inactive(view, ch);
  Params p = params();
  if (r == 1 && !eye_position(ch, m, p)) r = 5;
  if (r == 1) {
    double d2 = 0;
    for (int i = 0; i < 3; i++) d2 += (m[9 + i] - g_head[i]) * (m[9 + i] - g_head[i]);
    if (d2 > kScriptedShotDistance * kScriptedShotDistance) r = 6;
  }
  if (g_reason.exchange(r) != r) TB_LOG("firstperson: %s", kReasons[r]);
  if (r != 1) {
    g_active = false;
    g_smooth_valid = false;
    g_cache_frame = ~0ull;
    return false;
  }
  if (p.aim_distance > 0) {
    // Look at the point the game camera's centre ray reaches at aim_distance (where the reticle
    // points at that range), so the reticle stays on target from the new viewpoint.
    double t[3] = {m[9] + m[6] * p.aim_distance, m[10] + m[7] * p.aim_distance, m[11] + m[8] * p.aim_distance};
    double fwd[3] = {t[0] - g_eye[0], t[1] - g_eye[1], t[2] - g_eye[2]};
    normalize(fwd);
    double up0[3] = {0, 1, 0}, right[3], up[3];
    cross(up0, fwd, right);  // left-handed: right = up x forward
    normalize(right);
    cross(fwd, right, up);
    for (int i = 0; i < 3; i++) m[i] = right[i], m[3 + i] = up[i], m[6 + i] = fwd[i];
  }
  m[9] = g_eye[0];
  m[10] = g_eye[1];
  m[11] = g_eye[2];
  g_active = true;
  return true;
}

// core/camera.h pre-render: first-person FOV.
void fp_prerender(void* view) {
  if (!g_on.load(std::memory_order_relaxed) || !G.cameraman) return;
  // Which camera is rendered, as it changes (the Cameraman camera in gameplay; the cinematic,
  // free or death camera otherwise).
  static void* last = nullptr;
  if (view != last) {
    last = view;
    TB_LOG("firstperson: rendering %s camera %p (camera mode %d)", view == *G.cameraman ? "the Cameraman" : "another",
           view, *G.cam_mode);
  }
  if (view != *G.cameraman) {
    // Another camera is rendered (cinematic, free, death...): the Cameraman camera is no longer
    // set, so the filter would not notice; stop hiding the body.
    if (g_active.exchange(false)) {
      g_reason = 2;
      TB_LOG("firstperson: %s", kReasons[2]);
    }
    return;
  }
  if (!g_active.load(std::memory_order_relaxed)) return;
  Params p = params();
  if (p.fov_deg <= 1 || p.fov_deg >= 170) return;
  float aspect = rd<float>(view, kViewAspect);
  if (!(aspect > 0.1f && aspect < 10.f)) return;
  // The setting is the horizontal FOV of a 16:9 view; keep the vertical FOV for other aspects.
  double v = 2.0 * std::atan(std::tan(p.fov_deg * M_PI / 360.0) / (16.0 / 9.0));
  float h = float(2.0 * std::atan(std::tan(v * 0.5) * aspect));
  auto* b = static_cast<uint8_t*>(view);
  if (rd<float>(view, kViewFov) != h) {
    memcpy(b + kViewFov, &h, sizeof h);
    b[kViewDirtyProj] = 0;
    b[kViewDirtyWorld] = 0;
  }
}

// CharacterHide::set: the player's Character update calls it every frame with the debug panel's
// "Hide character" flag for bit 8; while first person is active we OR ours in. The mask hides the
// character's meshes, its weapon and its flashlight, and the flashlight's "hidden" flag also turns
// its beam off; the setter only does work when the mask changes (+0xc caches the applied mask), so
// right after it applied our state the held items are put back as configured.
void __fastcall hk_hide(void* hide, char on, uint32_t bits) {
  if (bits != kHideDebugBit) return G.o_hide(hide, on, bits);
  g_hide_calls++;
  void* ch = hide ? *static_cast<void**>(hide) : nullptr;
  if (!on && ch && g_active.load(std::memory_order_relaxed) && ch == player()) {
    auto mode = params().hide;
    if (mode != Params::Hide::None) {
      auto* h = static_cast<uint8_t*>(hide);
      uint32_t before = rd<uint32_t>(h, 0xc);
      G.o_hide(hide, 1, bits);
      g_hide_forced++;
      uint32_t mask = rd<uint32_t>(h, 8);
      // Only when ours is the only reason the character is hidden (a script or cutscene hiding it
      // sets other bits; leave those alone).
      if (before != mask && (mask & 0xffff) == kHideDebugBit && !(mask & 0x60000)) {
        void* fl = rd<void*>(ch, kCharFlashlight);
        void* wp = rd<void*>(ch, kCharWeapon);
        if (mode == Params::Hide::Body) {
          if (fl) G.fl_hide(fl, 0);       // flashlight mesh and beam back
          if (wp) G.weapon_hide(wp, 0);   // weapon mesh back
        } else if (fl && rd<uint8_t>(fl, 0)) {
          static_cast<uint8_t*>(fl)[0] = 0;  // mesh stays hidden; the beam comes back
          G.fl_update(fl);
        }
      }
      return;
    }
  }
  G.o_hide(hide, on, bits);
}

bool resolve() {
  for (auto& s : kSigs) {
    if (!sig_ok(s.rva, s.hex)) {
      TB_LOG("firstperson: signature mismatch at rva 0x%zx; first person disabled", (size_t)s.rva);
      return false;
    }
  }
  const auto& img = game_image();
  G.players = static_cast<void**>(rip_target(img.at(RVA_GET_PLAYERS), 3, 7));
  G.cam_mode = static_cast<volatile int*>(rip_target(img.at(RVA_GET_CAMMODE), 2, 6));
  G.cameraman = static_cast<void**>(rip_target(img.at(RVA_ACTIVE_CAM) + 0xd, 3, 7));
  for (void* g : {(void*)G.players, (void*)G.cam_mode, (void*)G.cameraman})
    if (!img.contains(reinterpret_cast<uintptr_t>(g))) {
      TB_LOG("firstperson: a global resolves outside the image; first person disabled");
      return false;
    }
  G.bone_slot = reinterpret_cast<BoneSlotFn>(img.at(RVA_BONE_SLOT));
  G.has_vehicle = reinterpret_cast<HasVehicleFn>(img.at(RVA_HAS_VEHICLE));
  G.fl_update = reinterpret_cast<Obj1Fn>(img.at(RVA_FL_UPDATE));
  G.fl_hide = reinterpret_cast<ObjBoolFn>(img.at(RVA_FL_HIDE));
  G.weapon_hide = reinterpret_cast<ObjBoolFn>(img.at(RVA_WEAPON_HIDE));
  return true;
}

std::string fmt3(const double* v) {
  char b[96];
  snprintf(b, sizeof b, "[%.3f,%.3f,%.3f]", v[0], v[1], v[2]);
  return b;
}

std::string status() {
  Params p = params();
  char b[1024];
  snprintf(b, sizeof b,
           "{\"mode\":\"%s\",\"ok\":%s,\"hooks\":%s,\"active\":%s,\"reason\":%s,\"camera_mode\":%d,"
           "\"eye\":%s,\"head\":%s,\"root\":%s,\"game_camera\":%s,\"filter_calls\":%llu,"
           "\"hide_calls\":%llu,\"hide_forced\":%llu,\"anchor\":\"%s\",\"fov\":%.1f}",
           p.first_person ? "first_person" : "third_person", G.ok ? "true" : "false",
           camera_hooked() ? "true" : "false", g_active.load() ? "true" : "false",
           json_str(kReasons[g_reason.load()]).c_str(), G.cam_mode ? *G.cam_mode : -1, fmt3(g_eye).c_str(),
           fmt3(g_head).c_str(), fmt3(g_root).c_str(), fmt3(g_cam).c_str(),
           (unsigned long long)g_filter_calls.load(), (unsigned long long)g_hide_calls.load(),
           (unsigned long long)g_hide_forced.load(), p.anchor == Anchor::Head ? "head" : "body", p.fov_deg);
  return b;
}

}  // namespace

void firstperson_register() {
  cfg_listen("camera.", [](const std::string& id, const std::string& v) {
    std::lock_guard lk(g_pmu);
    double n = strtod(v.c_str(), nullptr);
    if (id == "camera.mode") {
      g_p.first_person = v == "first_person" || v == "first" || v == "fp";
      g_on = g_p.first_person;
      TB_LOG("firstperson: mode %s", g_p.first_person ? "first_person" : "third_person");
    }
    if (id == "camera.anchor") g_p.anchor = v == "body" ? Anchor::Body : Anchor::Head;
    if (id == "camera.eye_up") g_p.eye_up = n;
    if (id == "camera.eye_forward") g_p.eye_forward = n;
    if (id == "camera.eye_height") g_p.eye_height = n > 0.3 ? n : 1.7;
    if (id == "camera.hide")
      g_p.hide = v == "none" ? Params::Hide::None : v == "body" ? Params::Hide::Body : Params::Hide::All;
    if (id == "camera.fov") g_p.fov_deg = n;
    if (id == "camera.smooth") g_p.smooth = n > 0 ? n : 0;
    if (id == "camera.aim_distance") g_p.aim_distance = n > 0 ? n : 0;
  });
  camera_filter_add(0, fp_filter);
  camera_prerender_add(fp_prerender);
  // `quiver lab call <i> fp [status]`; `fp slot N`: the player's slot N world position (probe).
  lab_verb("fp", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    if (a.empty() || a[0] == "status") return status();
    if (a[0] == "slots" && G.ok) {
      // Positions of slots 0..N-1 relative to the root, read on the game thread by the filter.
      int n = a.size() > 1 ? atoi(a[1].c_str()) : 0x30;
      {
        std::lock_guard lk(g_slots_mu);
        g_slots_out.clear();
        g_slots_req = n > 0 && n <= 0x60 ? n : 0x30;
      }
      for (int i = 0; i < 200; i++) {
        Sleep(10);
        std::lock_guard lk(g_slots_mu);
        if (g_slots_req == 0) return g_slots_out;
      }
      ok = false;
      return json_str("timeout (no main-camera update: in-world?)");
    }
    if (a[0] == "freecam" && a.size() > 1 && G.ok) {
      g_freecam_req = a[1] == "on" ? 1 : 0;
      return json_str(a[1] == "on" ? "free camera requested" : "player camera requested");
    }
    ok = false;
    return json_str("usage: fp [status|slots N|freecam on|off]");
  });
}

void firstperson_start() {
  G.ok = resolve();
  if (!G.ok) return;
  if (!hook_fn(reinterpret_cast<void*>(game_image().at(RVA_HIDE_MASK)), reinterpret_cast<void*>(&hk_hide),
               reinterpret_cast<void**>(&G.o_hide), "CharacterHide::set"))
    TB_LOG("firstperson: hide hook failed; the body stays visible in first person");
  TB_LOG("firstperson: ready (rendered camera at %p)", (void*)G.cameraman);
}

}  // namespace tb
