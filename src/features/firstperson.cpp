// First-person camera (docs/first-person.md).
//
// Render-side only: the game keeps simulating its own third-person camera (PlayerCamera and the
// "Cameraman camera" BaseCamera it feeds), and every gameplay system that reads the camera (aim,
// movement relative to the camera, AI, sound listener) reads BaseCamera's float transform at
// +0xcc8, which is left alone. What changes is the transform the renderer gets: a camera filter
// (core/camera.h, run from stereo's View::setViewToWorld hook before any eye offset) moves the
// main view to the player's eyes, keeping the game camera's orientation. What of the player is
// drawn depends on `hide`: head (default) leaves only the head's submeshes out of the main views
// (they stay in the shadow maps), so arms, hands, held items, torso and legs show; body / all hide
// the body with render-object flags (never the game's hide mask, which freezes "use").
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
#include <functional>
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
constexpr uintptr_t RVA_HIDE_BODY = 0x1ef602;     // ... its "apply" sequence (the calls below)
constexpr uintptr_t RVA_RO_FLAG = 0x463180;       // RenderObject::setFlags(this, bits, on): 8 = hidden
constexpr uintptr_t RVA_ATT_HIDE = 0x2d8760;      // attachment slots (inventory +0x48): flag 8 on each
constexpr uintptr_t RVA_ENT_HIDE = 0x308110;      // inventory +0x18: linked entity setHidden(this, on, release)
constexpr uintptr_t RVA_HELD_HIDE = 0x2f1420;     // held throwable (Character +0x240) setHidden(this, on)
constexpr uintptr_t RVA_FL_HIDE = 0x22cb60;       // Flashlight::setHidden: [+0xc0] ? flag 8 on [+0xc8]
constexpr uintptr_t RVA_WEAPON_HIDE = 0x340340;   // WeaponHolder::setHidden: [+0x30] ? flag 8 on [+0x38]
constexpr uintptr_t RVA_USE_MGR = 0x321e10;       // `mov rax,[rip+d]; ret`: UseTarget manager (+4 target handle)
constexpr uintptr_t RVA_USE_TARGET = 0x321f20;    // UseTarget::target(mgr): handle at +4 -> object
constexpr uintptr_t RVA_OBJ_QUERY = 0x1d9190;     // objects of a type mask (out, shape or null, mask, flag)

constexpr int kSlotHead = 0x1d;            // head slot (FUN_14017a420 "head heading", Taken head shots)
constexpr int kSlotGround = 30;            // straight below the root at the feet (live census)
constexpr size_t kCharTransform = 0xbc;    // float[12] world transform (rotation rows, position)
constexpr size_t kCharVehicle = 0x218;     // vehicle link (+0xc handle, +0x8 driving)
constexpr size_t kCharHide = 0x210;        // CharacterHide* (+0 Character*, +8 mask, +0xc applied)
constexpr size_t kCharIsPlayer = 0x1c9;    // u8, set on the player character
constexpr size_t kCharInventory = 0x200;   // FUN_14017a5c0
constexpr size_t kCharHeld = 0x240;        // held throwable or null (FUN_14017acb0)
constexpr size_t kCharRender = 0x2a0;      // vtable slot: render slot (+0 u8 present, +8 RenderObject*)
constexpr size_t kRoFlags = 0x24;          // RenderObject flags (u32); 8 = hidden
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
    // CharacterHide::set's apply sequence, which the first-person hide mirrors: inventory
    // (0x17a5c0) +0x18 (0x203220) -> 0x308110; inventory +0x48 (0x21cad0) -> 0x2d8760;
    // vtable +0x2a0 -> [+8] -> RenderObject flag 8 (0x463180); Character +0x240 (0x17acb0) -> 0x2f1420
    {RVA_HIDE_BODY, "e8 b9 af f8 ff 48 8b c8 e8 11 3c 01 00 48 8b c8 44 0f b6 c6 40 0f b6 d7 e8 f1 8a 11 00 "
                    "48 8b 0b e8 99 af f8 ff 48 8b c8 e8 a1 d4 02 00 48 8b c8 41 0f b6 d6 e8 25 91 0e 00 "
                    "48 8b 0b 48 8b 01 ff 90 a0 02 00 00 45 0f b6 c6 ba 08 00 00 00 48 8b 48 08 e8 27 3b 27 00 "
                    "48 8b 0b e8 4f b6 f8 ff"},
    {0x17a5c0, "48 8b 81 00 02 00 00 c3"},
    {0x17acb0, "48 8b 81 40 02 00 00 c3"},
    {0x203220, "48 8d 41 18 c3"},
    {0x21cad0, "48 8d 41 48 c3"},
    {RVA_RO_FLAG, "44 8b 49 24 45 84 c0 74 05 44 0b ca eb 07 8b c2 f7 d0 44 23 c8 44 89 49 24 48 8b 49 08 48 85 c9"},
    {RVA_ATT_HIDE, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 0f b6 f2 48 8d 59 30 bf 08 00 00 00"},
    {RVA_ENT_HIDE, "40 53 48 83 ec 20 8b 41 28 48 8b d9 85 c0 74 5f"},
    {RVA_HELD_HIDE, "40 53 48 83 ec 20 48 8b d9 88 11 48 8b 49 10 48 85 c9 74 05 e8"},
    // Character::slotTransform's helpers: pose = [rcx+8] (0x184160), joint = [pose+slot*4+0x50]
    {0x184160, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b da 48 8b f1 48 8b 49 08 41 8b d0 41 8b f8 e8"},
    {0x414a30, "48 63 c2 8b 44 81 50 c3"},
    // test aid (fp freecam): free-camera toggle and CameraManager::setMode
    {0x1d2b40, "48 89 5c 24 08 57 48 83 ec 20 48 8b 05 ?? ?? ?? ?? 80 b8 1a 01 00 00 00"},
    {0x1d20f0, "40 57 48 83 ec 20 8b 05 ?? ?? ?? ?? 8b f9 83 f8 07 75 58 84 d2 75 54 e8"},
    // the held items' meshes: cmp byte [rcx+0xc0],0 ... flag 8 on [rcx+0xc8] (flashlight);
    // cmp byte [rcx+0x30],0 ... flag 8 on [rcx+0x38] (weapon)
    {RVA_FL_HIDE, "48 89 5c 24 08 57 48 83 ec 20 80 b9 c0 00 00 00 00 0f b6 fa 48 8b d9 74 15 48 8b 89 c8 00 00 00"},
    {RVA_WEAPON_HIDE, "80 79 30 00 88 51 1c 74 12 48 8b 49 38 44 0f b6 c2 ba 08 00 00 00 e9"},
    // diagnostics (fp use / fp usables): UseTarget manager getter, its target resolver, the
    // object-list query by type mask
    {RVA_USE_MGR, "48 8b 05 ?? ?? ?? ?? c3"},
    {RVA_USE_TARGET, "8b 41 04 85 c0 75 03 33 c0 c3 8b c8 48 8d 15 ?? ?? ?? ?? 81 e1 ff 1f 00 00 c1 e8 0d"},
    {RVA_OBJ_QUERY, "4c 8b dc 49 89 5b 10 49 89 6b 20 45 89 43 18 56 57 41 54 41 56 41 57 48 81 ec b0 00 00 00"},
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
using ObjBoolFn = void(__fastcall*)(void* obj, char on);
using RoFlagFn = void(__fastcall*)(void* ro, uint32_t bits, char on);
using EntHideFn = void(__fastcall*)(void* obj, char on, char release);
constexpr size_t kCharFlashlight = 0x230;  // Flashlight* (FUN_14017a660)
constexpr size_t kCharWeapon = 0x220;      // weapon holder (FUN_14017ae20)

struct Game {
  bool ok = false;
  void** players = nullptr;
  volatile int* cam_mode = nullptr;
  void** cameraman = nullptr;  // the "Cameraman camera" BaseCamera: the rendered camera in mode 0
  BoneSlotFn bone_slot = nullptr;
  HasVehicleFn has_vehicle = nullptr;
  HideMaskFn o_hide = nullptr;  // CharacterHide::set (the original, behind our hook)
  RoFlagFn ro_flag = nullptr;
  ObjBoolFn att_hide = nullptr;
  EntHideFn ent_hide = nullptr;
  ObjBoolFn held_hide = nullptr;
} G;

// ---- config --------------------------------------------------------------------------------
enum class Anchor { Head, Body };
struct Params {
  bool first_person = false;
  Anchor anchor = Anchor::Head;
  double eye_up = 0.05;       // m above the head slot (world up)
  double eye_forward = 0.10;  // m ahead of it, along the camera's horizontal forward
  double eye_height = 1.70;   // anchor = body: m above the character's root
  enum class Hide { None, Head, Body, All } hide = Hide::Head;
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
std::atomic<bool> g_hide_test{false};  // `fp hidetest on`: first-person hiding in any camera mode (test aid)
std::atomic<uint64_t> g_filter_calls{0}, g_hide_applied{0}, g_hide_restored{0};
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

// Diagnostics run on the game thread (from the filter): one job at a time, answered to the verb.
std::mutex g_job_mu;
std::function<std::string()> g_job;
std::string g_job_out;
std::atomic<bool> g_job_pending{false};
void job_run() {
  std::lock_guard lk(g_job_mu);
  if (g_job) g_job_out = g_job();
  g_job = nullptr;
  g_job_pending = false;
}
std::string job_call(std::function<std::string()> fn, bool& ok) {
  {
    std::lock_guard lk(g_job_mu);
    g_job = std::move(fn);
    g_job_out.clear();
    g_job_pending = true;
  }
  for (int i = 0; i < 200; i++) {
    Sleep(10);
    if (!g_job_pending) {
      std::lock_guard lk(g_job_mu);
      return g_job_out;
    }
  }
  std::lock_guard lk(g_job_mu);
  g_job = nullptr;
  g_job_pending = false;
  ok = false;
  return json_str("timeout (no main-camera update: in-world?)");
}

// A game object: type (vtable +0x20), world transform (vtable +8, position at +0x24).
std::string obj_json(void* o) {
  if (!o) return "null";
  auto vt = *static_cast<void***>(o);
  int type = reinterpret_cast<int(__fastcall*)(void*)>(vt[0x20 / 8])(o);
  const float* t = reinterpret_cast<const float*(__fastcall*)(void*)>(vt[8 / 8])(o);
  char b[256];
  snprintf(b, sizeof b, "{\"ptr\":\"%p\",\"vtable\":\"0x%llx\",\"type\":%d,\"pos\":[%.2f,%.2f,%.2f]}", o,
           (unsigned long long)(reinterpret_cast<uintptr_t>(vt) - reinterpret_cast<uintptr_t>(game_image().at(0)) +
                                0x140000000ull),
           type, t[9], t[10], t[11]);
  return b;
}

void* use_target() {
  void* mgr = reinterpret_cast<void*(__fastcall*)()>(game_image().at(RVA_USE_MGR))();
  return mgr ? reinterpret_cast<void*(__fastcall*)(void*)>(game_image().at(RVA_USE_TARGET))(mgr) : nullptr;
}

// Objects of the type mask within r metres of the player (the UseTarget query uses 0x4d).
std::string usables(uint32_t mask, double r) {
  void* ch = player();
  if (!ch) return json_str("no player");
  const float* root = reinterpret_cast<const float*>(static_cast<uint8_t*>(ch) + kCharTransform);
  static int buf[2 + 2 * 16384];
  reinterpret_cast<void(__fastcall*)(int*, void*, uint32_t, char)>(game_image().at(RVA_OBJ_QUERY))(buf, nullptr,
                                                                                                    mask, 0);
  std::string o = "[";
  int n = buf[0] < 16384 ? buf[0] : 16384, k = 0;
  for (int i = 0; i < n; i++) {
    void* obj = *reinterpret_cast<void**>(&buf[2 + 2 * i]);
    if (!obj) continue;
    auto vt = *static_cast<void***>(obj);
    const float* t = reinterpret_cast<const float*(__fastcall*)(void*)>(vt[1])(obj);
    double dx = t[9] - root[9], dy = t[10] - root[10], dz = t[11] - root[11];
    double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d > r) continue;
    char db[32];
    snprintf(db, sizeof db, ",\"d\":%.1f}", d);
    std::string j = obj_json(obj);
    j.pop_back();
    o += (k++ ? "," : "") + j + db;
    if (k >= 200) break;
  }
  return o + "]";
}

// ---- render-object census (`fp parts`) -------------------------------------------------------
bool readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
  if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  auto end = static_cast<const uint8_t*>(mi.BaseAddress) + mi.RegionSize;
  return static_cast<const uint8_t*>(p) + n <= end;
}

std::string cstr_at(const void* p, size_t max = 64) {
  if (!readable(p, 1)) return "";
  std::string s;
  auto* c = static_cast<const char*>(p);
  for (size_t i = 0; i < max && readable(c + i, 1) && c[i]; i++) {
    unsigned char ch = c[i];
    if (ch < 0x20 || ch > 0x7e) return s.empty() ? "" : s + "?";
    s += ch == '"' || ch == '\\' ? '_' : char(ch);
  }
  return s;
}

bool submesh_min_y(void* sm, float& mn, float& mx);

// A render object, its mesh and submeshes (material effect, bind-pose height range of the
// vertices); with `bones`, the mesh's bone names and bounding spheres.
std::string ro_json(void* ro, bool bones) {
  if (!readable(ro, 0x1f8)) return "null";
  char b[512];
  void* mesh = rd<void*>(ro, 0x80);
  int nsub = mesh && readable(mesh, 0x280) ? rd<int>(mesh, 0x128) : -1;
  int nlod = nsub >= 0 ? rd<int>(mesh, 0x244) : -1;
  int nbone = nsub >= 0 ? rd<int>(mesh, 0x240) : -1;
  void* ba = rd<void*>(ro, 0xe8);
  snprintf(b, sizeof b,
           "{\"ro\":\"%p\",\"flags\":\"0x%x\",\"mask\":\"0x%x\",\"mesh\":\"%p\",\"submeshes\":%d,\"lods\":%d,"
           "\"lod_w\":[%.2f,%.2f,%.2f],\"bones\":%d,\"bone_array\":%d,\"pos\":[%.2f,%.2f,%.2f],"
           "\"bbox\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],\"parts\":[",
           ro, rd<uint32_t>(ro, 0x24), rd<uint32_t>(ro, 0x158), mesh, nsub, nlod, rd<float>(ro, 0x178),
           rd<float>(ro, 0x17c), rd<float>(ro, 0x180), nbone, ba && readable(ba, 0x20) ? rd<int>(ba, 0x18) : -1,
           rd<float>(ro, 0xac), rd<float>(ro, 0xb0), rd<float>(ro, 0xb4), nsub >= 0 ? rd<float>(mesh, 0x268) : 0.f,
           nsub >= 0 ? rd<float>(mesh, 0x26c) : 0.f, nsub >= 0 ? rd<float>(mesh, 0x270) : 0.f,
           nsub >= 0 ? rd<float>(mesh, 0x274) : 0.f, nsub >= 0 ? rd<float>(mesh, 0x278) : 0.f,
           nsub >= 0 ? rd<float>(mesh, 0x27c) : 0.f);
  std::string o = b;
  for (int i = 0; i < nsub && i < 32; i++) {
    void* sm = rd<void*>(mesh, 0x28 + size_t(i) * 8);
    if (!readable(sm, 0x130)) {
      o += i ? ",null" : "null";
      continue;
    }
    void* mat = rd<void*>(sm, 8);
    std::string name;
    if (readable(mat, 0x90)) {
      void* fx = rd<void*>(mat, 0);
      name = fx && readable(fx, 8) ? cstr_at(rd<void*>(fx, 0)) : cstr_at(static_cast<uint8_t*>(mat) + 8);
    }
    char t[300];
    float mn = 0, mx = 0;
    bool vok = submesh_min_y(sm, mn, mx);
    snprintf(t, sizeof t,
             "%s{\"i\":%d,\"lod\":%d,\"fx\":\"%s\",\"mflags\":\"0x%x\",\"verts\":%d,\"y\":[%.2f,%.2f]}",
             i ? "," : "", i, rd<int>(sm, 0x90), name.c_str(), readable(mat, 0x90) ? rd<uint32_t>(mat, 0x88) : 0,
             rd<int>(sm, 0x94), vok ? mn : -1.f, vok ? mx : -1.f);
    o += t;
  }
  o += "]";
  if (bones && nbone > 0) {
    o += ",\"bone_list\":[";
    auto* bl = rd<uint8_t*>(mesh, 0x238);
    for (int i = 0; i < nbone && i < 256 && readable(bl + size_t(i) * 0x80, 0x80); i++) {
      auto* e = bl + size_t(i) * 0x80;
      char t[256];
      // name (char[0x40]), +0x40..0x6c inverse bind, +0x70 bounding sphere (centre, radius) in
      // bind-pose model space
      snprintf(t, sizeof t, "%s[%d,\"%s\",[%.3f,%.3f,%.3f,%.3f]]", i ? "," : "", i, cstr_at(e, 0x40).c_str(),
               rd<float>(e, 0x70), rd<float>(e, 0x74), rd<float>(e, 0x78), rd<float>(e, 0x7c));
      o += t;
    }
    o += "]";
  }
  return o + "}";
}

// ---- hiding the player ----------------------------------------------------------------------
// Not with the game's CharacterHide mask (docs/first-person.md, "Use and pickups"): GameMode skips
// a character's input reader (Character +0x1a0, PlayerInputReader::update 0x2a0940, which handles
// "use", pickups and the other action presses) whenever the mask's low 16 bits are exactly 2 or 8
// (0x34dcb0 -> 0x1ef6e0), so with the debug "Hide character" bit the use prompt still showed but
// pressing use did nothing. Instead the visual part of what CharacterHide::set does for a hidden
// character is applied directly: render-object flag 8 on the body, its attachment slots, the
// inventory's linked entity and the held throwable, and (hide = all) on the weapon and flashlight
// meshes (only the mesh: the beam stays on). The game re-applies its own visibility whenever its
// mask changes (CharacterHide::set's applied-mask cache, +0xc; e.g. a use animation hides the
// weapon with 0x10000 and shows everything again after), so the hook below re-applies ours right
// after; to stop hiding, CharacterHide::set is made to re-apply the game's own mask.
struct HideState {
  void* ch = nullptr;  // the character ours is applied to
  bool on = false;
  Params::Hide mode = Params::Hide::None;
} g_hs;

void* render_object(void* ch) {
  auto vt = *static_cast<void***>(ch);
  auto* r = reinterpret_cast<uint8_t*(__fastcall*)(void*)>(vt[kCharRender / 8])(ch);
  return r && r[0] ? rd<void*>(r, 8) : nullptr;
}

// Flag 8 on a held item's mesh, as its setHidden does, without the item's own hidden state
// (the flashlight's [0] also switches its beam off).
void hide_mesh(void* item, size_t present, size_t ro) {
  if (item && rd<uint8_t>(item, present)) G.ro_flag(rd<void*>(item, ro), 8, 1);
}

// The game's mask hides the whole character (scripts, cutscenes): low 16 bits set.
bool game_hides(void* ch) {
  void* h = rd<void*>(ch, kCharHide);
  return !h || (rd<uint32_t>(h, 8) & 0xffff) != 0;
}

void hide_apply(void* ch, Params::Hide mode) {
  if (game_hides(ch)) return;  // already hidden by the game, held items included
  uint32_t mask = rd<uint32_t>(rd<void*>(ch, kCharHide), 8);
  if (void* ro = render_object(ch)) G.ro_flag(ro, 8, 1);
  if (auto* inv = rd<uint8_t*>(ch, kCharInventory)) {
    G.ent_hide(inv + 0x18, 1, 0);
    G.att_hide(inv + 0x48, 1);
  }
  if (void* held = rd<void*>(ch, kCharHeld)) G.held_hide(held, 1);
  if (mode == Params::Hide::All) {
    hide_mesh(rd<void*>(ch, kCharWeapon), 0x30, 0x38);
    if (!(mask & 0x60000)) hide_mesh(rd<void*>(ch, kCharFlashlight), 0xc0, 0xc8);
  }
  g_hide_applied++;
}

// Back to the game's own visibility: make CharacterHide::set re-apply its current mask.
void hide_restore(void* ch) {
  void* h = rd<void*>(ch, kCharHide);
  if (!h) return;
  auto* hb = static_cast<uint8_t*>(h);
  uint32_t stale = rd<uint32_t>(h, 8) ^ 0x80000000u;
  memcpy(hb + 0xc, &stale, 4);
  G.o_hide(h, 0, 0);
  g_hide_restored++;
}

// ---- hide = head: the head's submeshes, left out of the main views only ----------------------
// The player's body is one rend::Object (Character +0x2d8 render slot -> +8; 0x2afcc0) with one
// rend::Mesh (+0x80): up to 32 submeshes (mesh +0x28[i], count +0x128), each with its own material
// (head skin, hair, eyes, teeth, hands, jacket, hoodie, jeans...). rend::Object +0x158 is a
// per-submesh draw mask (bit i = submesh i, all set by the ctor 0x46cd00), tested both by the
// main-view collection (0x4cad50, reached through rend::Object vtable +0x40 = 0x46dfb0) and by the
// shadow collection (0x4cd150). The head's submeshes are those whose bind-pose vertices all lie
// above the neck (read from the CPU copy of the vertex buffer, as the engine's debug normal drawer
// 0x4d0c30 does); the hook on the main collection clears their bits around the call, so they stay
// in the shadow maps. Anything unexpected (signatures, layout, no head found) falls back to
// hiding the body as `hide = body` does.
constexpr uintptr_t RVA_OBJ_COLLECT = 0x4cad50;  // main-view collection (collector, ctx, rend::Object*)
constexpr size_t kRoSubmeshMask = 0x158;          // rend::Object: bit i = draw submesh i
using CollectFn = void(__fastcall*)(void* collector, void* ctx, void* obj);
const Sig kHeadSigs[] = {
    {RVA_OBJ_COLLECT, "48 8b c4 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8d a8 28 fd ff ff "
                      "48 81 ec 98 03 00 00"},
    // rend::Object vtable +0x40: if visible and a LOD weight > 0, tail-call 0x4cad50(collector, ctx, this)
    {0x46dfb0, "80 3d ?? ?? ?? ?? 00 74 35 80 b9 70 01 00 00 00 75 2c f3 0f 10 81 78 01 00 00 0f 57 c9 0f 2f c1 "
               "77 0d f3 0f 10 81 7c 01 00 00 0f 2f c1 76 0f 4c 8b c1 48 8b 0d ?? ?? ?? ?? e9 62 cd 05 00"},
    {0x4cbf42, "45 85 b5 58 01 00 00 0f 84"},  // main: test [obj+0x158], submesh bit
    {0x4cd6a5, "44 85 ae 58 01 00 00 0f 84"},  // shadow: the same mask
    // debug normal drawer: VB = [sm+0x80], vtable +0x30 maps it; format at sm+0x10; stride
    // [sm+0x54]; vertex count [sm+0x94]; ... unmap through vtable +0x38
    {0x4d0c30, "40 53 55 56 57 41 55 41 56 48 81 ec e8 00 00 00 48 8b f9 48 8b f2 48 8b 89 80 00 00 00 48 8b 01 "
               "ff 50 30 45 33 c9 48 8d 4f 10 45 33 c0 33 d2 4c 8b f0 e8 ?? ?? ?? ?? 45 33 c9 48 63 e8 33 d2 48 "
               "8d 4f 10 45 8d 41 01 e8 ?? ?? ?? ?? 44 8b 6f 54 33 db 39 9f 94 00 00 00"},
    {0x4d0f75, "48 8b 8f 80 00 00 00 48 8b 01 48 81 c4 e8 00 00 00 41 5e 41 5d 5f 5e 5d 5b 48 ff 60 38"},
    // vertex format: 4-byte entries {stream, type, semantic, index}, count at +0x40
    {0x4ba240, "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 0f b6 71 40"},
    // rend::Mesh bones: count +0x240, array +0x238 (0x80 per bone: name, inverse bind, sphere)
    {0x4b2499, "39 9a 40 02 00 00"},
    {0x4b24c0, "49 8b 86 38 02 00 00"},
};
CollectFn g_o_collect = nullptr;
bool g_head_ok = false;                    // signatures matched and the hook is in
std::atomic<void*> g_head_ro{nullptr};     // the body render object whose head is left out
std::atomic<uint32_t> g_head_bits{0};      // ... these submeshes
std::atomic<uint64_t> g_collect_hits{0}, g_collect_other_thread{0}, g_collect_other_view{0};
std::atomic<uint32_t> g_game_tid{0};
struct HeadCache {
  void* mesh = nullptr;
  int nsub = -1;
  uint32_t bits = 0;
  float threshold = 0;
} g_hc;

// Lowest bind-pose vertex height (model space, metres above the feet) of a submesh.
bool submesh_min_y(void* sm, float& mn, float& mx) {
  if (!readable(sm, 0xa0)) return false;
  void* vb = rd<void*>(sm, 0x80);
  int nv = rd<int>(sm, 0x94), stride = rd<int>(sm, 0x54), voff = rd<int>(sm, 0x9c);
  int nfmt = rd<uint8_t>(sm, 0x50);
  if (!vb || !readable(vb, 8) || nv <= 0 || nv > 1 << 20 || stride < 12 || stride > 256 || voff < 0 || nfmt <= 0 ||
      nfmt > 16)
    return false;
  // position: the float3 (type 2) element of semantic 0; its offset sums the sizes of the
  // elements before it in the same stream (0x4ba240)
  const uint8_t* fmt = static_cast<uint8_t*>(sm) + 0x10;
  int stream = -1, pos = -1;
  for (int e = 0; e < nfmt && stream < 0; e++)
    if (fmt[e * 4 + 2] == 0 && fmt[e * 4 + 1] == 2) stream = fmt[e * 4];
  if (stream < 0) return false;
  static const int kSize[] = {4, 8, 12, 16, 4, 4, 4, 4, 8, 4, 8, 4, 8};  // per type, as 0x4ba240
  for (int e = 0, off = 0; e < nfmt; e++) {
    const uint8_t* fe = fmt + e * 4;
    if (fe[0] != stream) continue;
    if (fe[2] == 0 && fe[1] == 2) {
      pos = off;
      break;
    }
    off += fe[1] < 13 ? kSize[fe[1]] : 0;
  }
  if (pos < 0 || pos + 12 > stride) return false;
  auto vt = *static_cast<void***>(vb);
  auto* base = reinterpret_cast<uint8_t*(__fastcall*)(void*)>(vt[0x30 / 8])(vb);
  bool ok = base && readable(base + voff, size_t(nv - 1) * stride + pos + 12);
  if (ok) {
    mn = 1e9f, mx = -1e9f;
    for (int v = 0; v < nv; v++) {
      float y;
      memcpy(&y, base + voff + size_t(v) * stride + pos + 4, 4);
      if (!(y > -100.f && y < 100.f)) {
        ok = false;
        break;
      }
      mn = y < mn ? y : mn, mx = y > mx ? y : mx;
    }
  }
  reinterpret_cast<void(__fastcall*)(void*)>(vt[0x38 / 8])(vb);
  return ok;
}

// The head's submeshes of the body's mesh (cached per mesh): 0 = none found / not classifiable.
uint32_t head_bits(void* ro) {
  void* mesh = readable(ro, 0x1f8) ? rd<void*>(ro, 0x80) : nullptr;
  if (!mesh || !readable(mesh, 0x280)) return 0;
  int nsub = rd<int>(mesh, 0x128);
  if (mesh == g_hc.mesh && nsub == g_hc.nsub) return g_hc.bits;
  g_hc = {mesh, nsub, 0, 0};
  if (nsub <= 1 || nsub > 32) {
    TB_LOG("firstperson: hide = head: body mesh has %d submeshes; hiding the body instead", nsub);
    return 0;
  }
  // Neck height: the bounding sphere centre (+0x74 = y) of the "Neck0" bone, minus 0.2 m (the
  // head-skin submesh runs down to the collar: 1.46 m for a neck at 1.61, live); without that
  // bone, 75 % of the mesh's height.
  float top = rd<float>(mesh, 0x278), thr = 0.75f * top;
  int nb = rd<int>(mesh, 0x240);
  auto* bl = rd<uint8_t*>(mesh, 0x238);
  for (int i = 0; i < nb && i < 1024 && readable(bl + size_t(i) * 0x80, 0x80); i++) {
    std::string n = cstr_at(bl + size_t(i) * 0x80, 0x40);
    if (n == "Neck0" || n == "Neck") {
      float y = rd<float>(bl + size_t(i) * 0x80, 0x74) - 0.2f;
      if (y > 0.6f * top && y < 0.95f * top) thr = y;
      break;
    }
  }
  if (!(top > 0.5f && top < 4.f)) {
    TB_LOG("firstperson: hide = head: body mesh height %.2f unexpected; hiding the body instead", top);
    return 0;
  }
  uint32_t bits = 0;
  std::string desc;
  for (int i = 0; i < nsub; i++) {
    float mn, mx;
    void* sm = rd<void*>(mesh, 0x28 + size_t(i) * 8);
    if (!submesh_min_y(sm, mn, mx) || mx > top + 0.05f) {
      TB_LOG("firstperson: hide = head: submesh %d unreadable; hiding the body instead", i);
      return 0;
    }
    if (mn >= thr) bits |= 1u << i;
    char t[48];
    snprintf(t, sizeof t, " %d:%.2f-%.2f%s", i, mn, mx, mn >= thr ? "*" : "");
    desc += t;
  }
  uint32_t all = nsub == 32 ? ~0u : (1u << nsub) - 1;
  if (!bits || bits == all) {
    TB_LOG("firstperson: hide = head: no separate head parts (threshold %.2f m:%s); hiding the body instead", thr,
           desc.c_str());
    return 0;
  }
  g_hc.bits = bits;
  g_hc.threshold = thr;
  TB_LOG("firstperson: hide = head: head parts 0x%x of %d submeshes (above %.2f m; * = head:%s)", bits, nsub, thr,
         desc.c_str());
  return bits;
}

// Main-view collection of a rend::Object: the player's head submeshes masked out for the call.
void __fastcall hk_collect(void* collector, void* ctx, void* obj) {
  if (obj != g_head_ro.load(std::memory_order_relaxed)) return g_o_collect(collector, ctx, obj);
  uint32_t bits = g_head_bits.load(std::memory_order_relaxed);
  g_collect_hits++;
  if (GetCurrentThreadId() != g_game_tid.load(std::memory_order_relaxed)) g_collect_other_thread++;
  if (!G.cameraman || rd<void*>(ctx, 0x90) != *G.cameraman) g_collect_other_view++;
  auto* m = static_cast<uint8_t*>(obj) + kRoSubmeshMask;
  uint32_t was;
  memcpy(&was, m, 4);
  uint32_t now = was & ~bits;
  memcpy(m, &now, 4);
  g_o_collect(collector, ctx, obj);
  memcpy(m, &was, 4);
}

// Game thread, every main-view set and render: apply or undo ours as first person starts/stops.
void hide_sync() {
  if (!G.ok || !G.o_hide) return;
  void* ch = player();
  bool want = (g_active.load(std::memory_order_relaxed) || g_hide_test.load(std::memory_order_relaxed)) && ch;
  Params::Hide mode = want ? params().hide : Params::Hide::None;
  void* head_ro = nullptr;
  uint32_t hbits = 0;
  if (mode == Params::Hide::Head) {
    // The head's parts left out of the main views; the game hiding the character itself
    // (scripts, cutscenes) needs nothing from us. Not available: the body is hidden instead.
    if (game_hides(ch)) {
      mode = Params::Hide::None;
    } else {
      void* ro = g_head_ok ? render_object(ch) : nullptr;
      hbits = ro ? head_bits(ro) : 0;
      head_ro = hbits ? ro : nullptr;
      mode = head_ro ? Params::Hide::None : Params::Hide::Body;
    }
  }
  g_head_bits.store(hbits, std::memory_order_relaxed);
  g_head_ro.store(head_ro, std::memory_order_relaxed);
  if (mode == Params::Hide::None) want = false;
  if (g_hs.on && g_hs.ch != ch) g_hs.on = false;  // another character (level load): nothing to undo
  if (g_hs.on && (!want || g_hs.mode != mode)) {
    hide_restore(g_hs.ch);
    g_hs.on = false;
  }
  if (!want) return;
  if (!g_hs.on) {
    hide_apply(ch, mode);
    g_hs = {ch, true, mode};
    return;
  }
  // Something showed a mesh without a mask change (a weapon drawn, a new render object, a mask
  // change and back within one frame): apply again.
  auto shown = [](void* item, size_t present, size_t ro) {
    return item && rd<uint8_t>(item, present) && !(rd<uint32_t>(rd<void*>(item, ro), kRoFlags) & 8);
  };
  void* ro = render_object(ch);
  bool again = ro && !(rd<uint32_t>(ro, kRoFlags) & 8);
  if (mode == Params::Hide::All) {
    again = again || shown(rd<void*>(ch, kCharWeapon), 0x30, 0x38);
    if (!(rd<uint32_t>(rd<void*>(ch, kCharHide), 8) & 0x60000))
      again = again || shown(rd<void*>(ch, kCharFlashlight), 0xc0, 0xc8);
  }
  if (again && !game_hides(ch)) hide_apply(ch, mode);
}

// CharacterHide::set: pass-through; when the game has just re-applied the player's visibility (its
// applied-mask cache changed) while ours is on, ours goes back on top in the same call.
void __fastcall hk_hide(void* hide, char on, uint32_t bits) {
  if (!g_hs.on || !hide) return G.o_hide(hide, on, bits);
  uint32_t before = rd<uint32_t>(hide, 0xc);
  G.o_hide(hide, on, bits);
  if (*static_cast<void**>(hide) == g_hs.ch && rd<uint32_t>(hide, 0xc) != before && g_hs.ch == player())
    hide_apply(g_hs.ch, g_hs.mode);
}

// core/camera.h filter: the main view's transform -> the player's eyes.
bool fp_filter(void* view, double* m) {
  if (g_job_pending.load(std::memory_order_relaxed) && G.ok && G.cameraman && view == *G.cameraman) job_run();
  if (g_slots_req.load(std::memory_order_relaxed) && G.ok && G.cameraman && view == *G.cameraman) slots_census();
  if (g_freecam_req.load(std::memory_order_relaxed) >= 0 && G.ok) freecam_apply();
  g_game_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  if (!g_on.load(std::memory_order_relaxed)) {
    if (g_active.exchange(false)) g_reason = 0;
    if (g_hs.on || g_head_ro.load(std::memory_order_relaxed) || g_hide_test.load(std::memory_order_relaxed))
      hide_sync();
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
    hide_sync();
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
  hide_sync();
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
    hide_sync();
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
  G.ro_flag = reinterpret_cast<RoFlagFn>(img.at(RVA_RO_FLAG));
  G.att_hide = reinterpret_cast<ObjBoolFn>(img.at(RVA_ATT_HIDE));
  G.ent_hide = reinterpret_cast<EntHideFn>(img.at(RVA_ENT_HIDE));
  G.held_hide = reinterpret_cast<ObjBoolFn>(img.at(RVA_HELD_HIDE));
  return true;
}

std::string fmt3(const double* v) {
  char b[96];
  snprintf(b, sizeof b, "[%.3f,%.3f,%.3f]", v[0], v[1], v[2]);
  return b;
}

const char* kHideNames[] = {"none", "head", "body", "all"};

std::string status() {
  Params p = params();
  char b[1536];
  snprintf(b, sizeof b,
           "{\"mode\":\"%s\",\"ok\":%s,\"hooks\":%s,\"active\":%s,\"reason\":%s,\"camera_mode\":%d,"
           "\"eye\":%s,\"head\":%s,\"root\":%s,\"game_camera\":%s,\"filter_calls\":%llu,"
           "\"hidden\":%s,\"hide_applied\":%llu,\"hide_restored\":%llu,\"anchor\":\"%s\",\"fov\":%.1f,"
           "\"hide\":\"%s\",\"head_ok\":%s,\"head_parts\":\"0x%x\",\"head_collects\":%llu,"
           "\"head_collects_other_thread\":%llu,\"head_collects_other_view\":%llu}",
           p.first_person ? "first_person" : "third_person", G.ok ? "true" : "false",
           camera_hooked() ? "true" : "false", g_active.load() ? "true" : "false",
           json_str(kReasons[g_reason.load()]).c_str(), G.cam_mode ? *G.cam_mode : -1, fmt3(g_eye).c_str(),
           fmt3(g_head).c_str(), fmt3(g_root).c_str(), fmt3(g_cam).c_str(),
           (unsigned long long)g_filter_calls.load(), g_hs.on ? "true" : "false",
           (unsigned long long)g_hide_applied.load(), (unsigned long long)g_hide_restored.load(), p.anchor == Anchor::Head ? "head" : "body", p.fov_deg,
           kHideNames[int(p.hide)], g_head_ok ? "true" : "false",
           g_head_ro.load() ? g_head_bits.load() : 0u, (unsigned long long)g_collect_hits.load(),
           (unsigned long long)g_collect_other_thread.load(), (unsigned long long)g_collect_other_view.load());
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
      g_p.hide = v == "none"   ? Params::Hide::None
                 : v == "head" ? Params::Hide::Head
                 : v == "body" ? Params::Hide::Body
                               : Params::Hide::All;
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
    if (a[0] == "use" && G.ok) {
      // The game's current use target (what "use" acts on: the interaction prompt), the game's
      // own hide mask on the player and whether first person's hiding is applied.
      return job_call([] {
        void* ch = player();
        void* h = ch ? rd<void*>(ch, kCharHide) : nullptr;
        char b[96];
        snprintf(b, sizeof b, ",\"hide_mask\":\"0x%x\",\"hidden\":%s}", h ? rd<uint32_t>(h, 8) : 0,
                 g_hs.on ? "true" : "false");
        return "{\"target\":" + obj_json(use_target()) + b;
      }, ok);
    }
    if (a[0] == "usables" && G.ok) {
      uint32_t mask = a.size() > 1 ? strtoul(a[1].c_str(), nullptr, 0) : 4;
      double r = a.size() > 2 ? strtod(a[2].c_str(), nullptr) : 50;
      return job_call([mask, r] { return usables(mask, r); }, ok);
    }
    if (a[0] == "parts" && G.ok) {
      // The player's render objects: body, attachment slots, weapon, flashlight; their submeshes.
      bool bones = a.size() > 1 && a[1] == "bones";
      return job_call([bones] {
        void* ch = player();
        if (!ch) return json_str("no player");
        std::string o = "{\"body\":" + ro_json(render_object(ch), bones) + ",\"attachments\":[";
        auto* inv = rd<uint8_t*>(ch, kCharInventory);
        for (int i = 0; inv && i < 8; i++) {
          uint8_t* e = inv + 0x48 + size_t(i) * 0x40;
          o += std::string(i ? "," : "") + (rd<uint8_t>(e, 0x28) ? ro_json(rd<void*>(e, 0x30), false) : "null");
        }
        o += "]";
        void* w = rd<void*>(ch, kCharWeapon);
        o += ",\"weapon\":" + (w && rd<uint8_t>(w, 0x30) ? ro_json(rd<void*>(w, 0x38), false) : std::string("null"));
        void* f = rd<void*>(ch, kCharFlashlight);
        o += ",\"flashlight\":" +
             (f && rd<uint8_t>(f, 0xc0) ? ro_json(rd<void*>(f, 0xc8), false) : std::string("null"));
        return o + "}";
      }, ok);
    }
    if (a[0] == "give" && a.size() > 1 && G.ok) {
      // Test aid: what the game's Developer Menu cheats "cheat_receive_weapons" /
      // "cheat_receive_flashlight" do (MenuMode::action 0x24d8f0): WeaponHolder::give (0x340d10)
      // the revolver, flare gun and shotgun; Flashlight::setType (0x22d6b0) + batteries (0x22d240).
      static const Sig kGive[] = {
          {0x340d10, "48 89 5c 24 10 48 89 6c 24 18 56 48 83 ec 20 48 8b f2 48 8b e9 33 db e8"},
          {0x22d6b0, "48 89 5c 24 10 48 89 6c 24 18 56 48 83 ec 30 48 8b f2 48 8b e9 33 db e8"},
          {0x22d240, "01 91 44 01 00 00 ba 14 00 00 00"},
      };
      for (auto& s : kGive)
        if (!sig_ok(s.rva, s.hex)) {
          ok = false;
          return json_str("signature mismatch");
        }
      bool weapons = a[1] == "weapons";
      return job_call([weapons] {
        void* ch = player();
        if (!ch) return json_str("no player");
        if (weapons) {
          auto give = reinterpret_cast<void(__fastcall*)(void*, const char*)>(game_image().at(0x340d10));
          for (const char* w : {"Revolver", "Flaregun", "PumpShotgun"}) give(rd<void*>(ch, kCharWeapon), w);
          return json_str("Revolver, Flaregun, PumpShotgun");
        }
        void* fl = rd<void*>(ch, kCharFlashlight);
        reinterpret_cast<void(__fastcall*)(void*, const char*)>(game_image().at(0x22d6b0))(
            fl, "flashlight_normal_power_large_clip_A");
        reinterpret_cast<void(__fastcall*)(void*, int)>(game_image().at(0x22d240))(fl, 10);
        return json_str("flashlight_normal_power_large_clip_A, 10 batteries");
      }, ok);
    }
    if (a[0] == "mask" && a.size() > 1 && G.ok) {
      // Test aid: the body render object's per-submesh draw mask (+0x158, bit i = submesh i).
      uint32_t m = strtoul(a[1].c_str(), nullptr, 0);
      return job_call([m] {
        void* ch = player();
        void* ro = ch ? render_object(ch) : nullptr;
        if (!ro) return json_str("no player");
        uint32_t old = rd<uint32_t>(ro, 0x158);
        memcpy(static_cast<uint8_t*>(ro) + 0x158, &m, 4);
        char b[64];
        snprintf(b, sizeof b, "{\"old\":\"0x%x\",\"new\":\"0x%x\"}", old, m);
        return std::string(b);
      }, ok);
    }
    if (a[0] == "hidetest" && a.size() > 1) {
      // Test aid: first person's hiding of the player whatever the camera mode (third person too).
      g_hide_test = a[1] == "on";
      return json_str(g_hide_test ? "player hidden in any mode" : "player hidden in first person only");
    }
    if (a[0] == "freecam" && a.size() > 1 && G.ok) {
      g_freecam_req = a[1] == "on" ? 1 : 0;
      return json_str(a[1] == "on" ? "free camera requested" : "player camera requested");
    }
    ok = false;
    return json_str(
        "usage: fp [status|slots N|parts [bones]|mask HEX|give weapons|flashlight|freecam on|off|use|"
        "usables [mask] [radius]|hidetest on|off]");
  });
}

void firstperson_start() {
  G.ok = resolve();
  if (!G.ok) return;
  if (!hook_fn(reinterpret_cast<void*>(game_image().at(RVA_HIDE_MASK)), reinterpret_cast<void*>(&hk_hide),
               reinterpret_cast<void**>(&G.o_hide), "CharacterHide::set")) {
    // Without the hook the hiding still works, but a mesh the game shows again (after a use
    // animation, say) is only hidden again on the next frame.
    G.o_hide = reinterpret_cast<HideMaskFn>(game_image().at(RVA_HIDE_MASK));
    TB_LOG("firstperson: hide hook failed; hiding re-applied per frame only");
  }
  // hide = head: the main-view collection hook (falls back to hiding the body without it).
  g_head_ok = true;
  for (auto& s : kHeadSigs)
    if (!sig_ok(s.rva, s.hex)) {
      TB_LOG("firstperson: signature mismatch at rva 0x%zx; hide = head hides the body instead", (size_t)s.rva);
      g_head_ok = false;
      break;
    }
  if (g_head_ok && !hook_fn(reinterpret_cast<void*>(game_image().at(RVA_OBJ_COLLECT)),
                            reinterpret_cast<void*>(&hk_collect), reinterpret_cast<void**>(&g_o_collect),
                            "rend::Object main collect")) {
    TB_LOG("firstperson: collect hook failed; hide = head hides the body instead");
    g_head_ok = false;
  }
  TB_LOG("firstperson: ready (rendered camera at %p)", (void*)G.cameraman);
}

}  // namespace tb
