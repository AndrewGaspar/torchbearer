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
// the body with render-object flags (never the game's hide mask, which freezes "use"). With
// framing = close (default) the arms are re-posed as a viewmodel so the flashlight and gun stay in
// frame (the arm pose's outputs are hooked: render bones and attachment slots; see "framing").
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
constexpr uintptr_t RVA_FRAME_DT = 0x1df650;      // `movss xmm0,[rip+d]; ret`: the game's frame time (s)

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
    {RVA_FRAME_DT, "f3 0f 10 05 ?? ?? ?? ?? c3"},
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
  double near_m = 0;          // m: the near plane in first person; 0 = auto (close 0.05, else the game's)
  double smooth = 0.1;        // s: low-pass time constant on the head's offset from the root
  double aim_distance = 15;   // m: >0 = look at the point this far along the game camera's ray
  // framing = close (viewmodel arms); the knobs below only matter then
  bool framing_close = true;
  double hands_forward = 0.34;  // m: resting wrists this far ahead of the eye
  double hands_drop = 0.65;     // ... this fraction of the half-height below the centre
  double hands_side = 0.40;     // ... this fraction of the half-width out to their side
  double hands_sight = 0.60;    // aiming: the gun hand this fraction of the half-height below the centre
  double hands_aim = 1.0;       // 0..1: turn the held items towards the view's aim point
  double hands_follow = 0.25;   // s: animation motion faster than this passes through to the hands
  double hands_motion = 0.5;    // ... scaled by this
  int hands_arms = 3;           // test aid: bit 0 left arm, bit 1 right arm
  double hands_jacket = 0.5;    // the jacket's side panels follow this much of the upper arm's turn
  double hands_shrug = 0.35;    // the clavicles take this much of the arms' swing
  double hands_sway = 0.06;     // s: the hands trail the view's rotation by this time constant
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
double g_eye[3], g_head[3], g_root[3], g_cam[3], g_camfwd[3];
double g_smooth_off[3];
bool g_smooth_valid = false;
double g_frame_dt = 0;  // s, between the last two presented frames' eye updates

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

// The game's frame time (GameTimer, 0x1df650: the dt Character::update uses), so smoothing runs
// on game time (it pauses with the game and follows a lab's virtual clock).
double game_dt() {
  double dt = reinterpret_cast<float(__fastcall*)()>(game_image().at(RVA_FRAME_DT))();
  return dt > 0 && dt < 0.25 ? dt : dt >= 0.25 ? 0.25 : 0;
}
double dt_seconds() { return game_dt(); }

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
  g_frame_dt = dt;
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

// ---- framing = close: the arms as a viewmodel ------------------------------------------------
// The third-person animation holds the flashlight and gun low (out of a first-person frame except
// when aiming). With framing = close each arm is re-posed after the animation, before anything
// reads it, so the wrist sits inside a camera-relative box in the lower frame: a two-bone IK
// (upper arm, forearm; the animated elbow direction is the pole) moves the wrist there and the hand
// keeps its animated orientation (so recoil, reloads, use gestures and the flashlight boost still
// read), with the flashlight hand turned to point at the view's aim point. When the animation
// already has a hand inside the box (aiming), that arm does not move.
//
// The pose itself is not rewritten, its two outputs are. Character::postAnimation (0x17c080; the
// synchronous path 0x17f600 does the same) exports the model-space pose for the render bones
// through 0x184080 (-> 0x414a40: pose joint map[i] -> render bone i), then updates the flashlight
// (lights, mesh, the light-damage cone 0x22c3e0) and the weapon from Character::slotTransform
// (0x179f10). Both are hooked and apply the same per-segment rigid deltas, so the skinned arm, the
// flashlight and its beam, the gun and the damage cone agree. The deltas are computed at the
// export from this frame's pose and the camera of the last main-view set, kept relative to the
// character (walking does not leave the hands behind).
constexpr uintptr_t RVA_POSE_EXPORT = 0x184080;  // (anim, float out[n][12], n)
constexpr uintptr_t RVA_JOINT_WORLD = 0x179e30;  // Character::jointTransform(this, float out[12], joint): world
const Sig kVmSigs[] = {
    // lea r8,[rcx+0x48f8] (render map); mov rcx,[rcx+8]; jmp 0x414a40
    {RVA_POSE_EXPORT, "4c 8d 81 f8 48 00 00 48 8b 49 08 e9"},
    // 0x414a40: [rcx+8] -> model-space pose (0x64ff34); out[i] = model[map[i]] for i < map count
    {0x414a40, "48 89 5c 24 10 48 89 6c 24 18 48 89 7c 24 20 41 56 48 83 ec 60 48 8b 49 08 4d 8b f0 48 8b da e8"},
    {RVA_JOINT_WORLD, "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48 8b 01 41 8b f0 48 8b da 48 8b f9 ff 90 c8 02 00 "
                      "00 84 c0 74 56 48 8b 8f b8 01 00 00 e8"},
    // controller height (Character +0x1c0 -> +0xd0), subtracted from slot / joint heights
    {0x4083f0, "f3 0f 10 81 d0 00 00 00 c3"},
    // render bone count: [anim+0x4900]
    {0x184680, "8b 81 00 49 00 00 c3"},
};
constexpr size_t kAnimRenderMap = 0x48f8, kAnimRenderCount = 0x4900;
// The animation's skeleton instance ([Character +0x1b8] +8): +0x50 slot -> joint (int); its pose
// [+8] +0 the skeleton: +8 parent indices (int16), +0x10 joint count (live, `fp skel`).
constexpr size_t kXSlots = 0x50;
// Slots (semantic: the slot table maps them to joints): per side the upper arm, forearm, hand and
// the held item's attachment (0 = flashlight, 9 = weapon), live `fp skel` / `fp slotm`.
struct ArmSlots {
  int up, fore, hand, attach;
};
constexpr ArmSlots kArm[2] = {{6, 4, 1, 0}, {16, 14, 11, 9}};  // left (flashlight), right (gun)
using PoseExportFn = void(__fastcall*)(void* anim, float* out, int n);
PoseExportFn g_o_export = nullptr;
BoneSlotFn g_o_slot = nullptr;  // Character::slotTransform behind our hook
bool g_vm_ok = false;           // signatures matched, hooks in

// Row-vector rigid transform as the game stores it: rows right, up, forward, then position;
// p' = p * r + t. xf_mul(a, b) = a then b.
struct Xf {
  double r[3][3];
  double t[3];
};
Xf xf_id() { return Xf{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}}; }
Xf xf_from(const float* f) {
  Xf x;
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 3; k++) x.r[i][k] = f[i * 3 + k];
  for (int k = 0; k < 3; k++) x.t[k] = f[9 + k];
  return x;
}
void xf_to(const Xf& x, float* f) {
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 3; k++) f[i * 3 + k] = float(x.r[i][k]);
  for (int k = 0; k < 3; k++) f[9 + k] = float(x.t[k]);
}
Xf xf_mul(const Xf& a, const Xf& b) {
  Xf c;
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 3; k++) c.r[i][k] = a.r[i][0] * b.r[0][k] + a.r[i][1] * b.r[1][k] + a.r[i][2] * b.r[2][k];
  for (int k = 0; k < 3; k++) c.t[k] = a.t[0] * b.r[0][k] + a.t[1] * b.r[1][k] + a.t[2] * b.r[2][k] + b.t[k];
  return c;
}
Xf xf_inv(const Xf& a) {  // general 3x3 (bones may carry scale)
  const double(*m)[3] = a.r;
  double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  Xf o = xf_id();
  if (std::fabs(det) < 1e-12) return o;
  double id = 1.0 / det;
  o.r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
  o.r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
  o.r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
  o.r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
  o.r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
  o.r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
  o.r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
  o.r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
  o.r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
  for (int k = 0; k < 3; k++) o.t[k] = -(a.t[0] * o.r[0][k] + a.t[1] * o.r[1][k] + a.t[2] * o.r[2][k]);
  return o;
}
void v_sub(const double* a, const double* b, double* o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
double v_dot(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double v_len(const double* a) { return std::sqrt(v_dot(a, a)); }
void xf_pt(const Xf& x, const double* p, double* o) {
  for (int k = 0; k < 3; k++) o[k] = p[0] * x.r[0][k] + p[1] * x.r[1][k] + p[2] * x.r[2][k] + x.t[k];
}
// Rotation (row-vector 3x3) turning direction a towards b: the fraction w of the angle, at most
// max_rad.
void rot_arc(const double* a0, const double* b0, double w, double max_rad, double r[3][3]) {
  double a[3] = {a0[0], a0[1], a0[2]}, b[3] = {b0[0], b0[1], b0[2]}, ax[3];
  normalize(a), normalize(b);
  cross(a, b, ax);
  double s = v_len(ax), c = v_dot(a, b), ang = std::atan2(s, c) * w;
  if (ang > max_rad) ang = max_rad;
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 3; k++) r[i][k] = i == k;
  if (s < 1e-9 || std::fabs(ang) < 1e-9) return;
  for (double& v : ax) v /= s;
  double cs = std::cos(ang), sn = std::sin(ang), t = 1 - cs, x = ax[0], y = ax[1], z = ax[2];
  // the column-vector Rodrigues matrix, stored transposed (row-vector convention)
  double m[3][3] = {{t * x * x + cs, t * x * y - sn * z, t * x * z + sn * y},
                    {t * x * y + sn * z, t * y * y + cs, t * y * z - sn * x},
                    {t * x * z - sn * y, t * y * z + sn * x, t * z * z + cs}};
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 3; k++) r[i][k] = m[k][i];
}
// The fraction k of a rotation (row-vector 3x3), about the same axis.
void rot_scale(const double r[3][3], double k, double o[3][3]) {
  // column-vector form c = r^T: angle from the trace, axis from the antisymmetric part
  double tr = r[0][0] + r[1][1] + r[2][2], ca = (tr - 1) / 2;
  ca = ca > 1 ? 1 : ca < -1 ? -1 : ca;
  double ang = std::acos(ca);
  double ax[3] = {r[1][2] - r[2][1], r[2][0] - r[0][2], r[0][1] - r[1][0]};  // c32-c23, c13-c31, c21-c12
  for (int i = 0; i < 3; i++)
    for (int c = 0; c < 3; c++) o[i][c] = i == c;
  double n = v_len(ax);
  if (n < 1e-9 || ang < 1e-9) return;
  for (double& v : ax) v /= n;
  double a = ang * k, cs = std::cos(a), sn = std::sin(a), t = 1 - cs, x = ax[0], y = ax[1], z = ax[2];
  double m[3][3] = {{t * x * x + cs, t * x * y - sn * z, t * x * z + sn * y},
                    {t * x * y + sn * z, t * y * y + cs, t * y * z - sn * x},
                    {t * x * z - sn * y, t * y * z + sn * x, t * z * z + cs}};
  for (int i = 0; i < 3; i++)
    for (int c = 0; c < 3; c++) o[i][c] = m[c][i];
}
// Rigid delta: rotate by r about `from`, then move `from` to `to` (new = old * delta).
Xf xf_pivot(const double* from, const double r[3][3], const double* to) {
  Xf d;
  memcpy(d.r, r, sizeof d.r);
  for (int k = 0; k < 3; k++) d.t[k] = to[k] - (from[0] * r[0][k] + from[1] * r[1][k] + from[2] * r[2][k]);
  return d;
}

// Each joint's segment: side * 3 + {0 upper arm, 1 forearm, 2 hand}, or none; cached per skeleton.
constexpr uint8_t kSegNone = 0xff;
struct VmSkel {
  void* skel = nullptr;
  int n = 0;
  bool ok = false;
  int joint[2][4] = {};  // per side: upper arm, forearm, hand, attachment
  int jacket[2] = {-1, -1};  // per side: the jacket's shoulder panel (Jkt_L/R_Shoulder), or -1
  int clav[2] = {-1, -1};    // per side: the clavicle (the upper arm's parent)
  uint8_t seg[1024];
} g_vs;

void* anim_x(void* ch) {
  void* anim = rd<void*>(ch, 0x1b8);
  return anim ? rd<void*>(anim, 8) : nullptr;
}

bool vm_skeleton(void* ch) {
  void* x = anim_x(ch);
  void* pose = x && readable(x, 0x160) ? rd<void*>(x, 8) : nullptr;
  void* skel = pose && readable(pose, 8) ? rd<void*>(pose, 0) : nullptr;
  if (!skel || !readable(skel, 0x28)) return false;
  int n = rd<int>(skel, 0x10);
  if (skel == g_vs.skel && n == g_vs.n) return g_vs.ok;
  g_vs.skel = skel, g_vs.n = n, g_vs.ok = false;
  auto* par = rd<int16_t*>(skel, 8);
  if (n <= 8 || n > 1024 || !readable(par, size_t(n) * 2) || par[0] != -1) {
    TB_LOG("firstperson: framing = close: unexpected skeleton (%d joints); hands stay as animated", n);
    return false;
  }
  for (int s = 0; s < 2; s++) {
    const int slot[4] = {kArm[s].up, kArm[s].fore, kArm[s].hand, kArm[s].attach};
    for (int k = 0; k < 4; k++) g_vs.joint[s][k] = rd<int>(x, kXSlots + size_t(slot[k]) * 4);
  }
  // each chain must be upper arm -> forearm -> hand, with the attachment below the hand
  for (int s = 0; s < 2; s++) {
    const int* j = g_vs.joint[s];
    bool chain = j[0] > 0 && j[0] < n && j[1] > 0 && j[1] < n && j[2] > 0 && j[2] < n && par[j[1]] == j[0] &&
                 par[j[2]] == j[1];
    int a = j[3];
    for (int g = 0; chain && a >= 0 && a < n && a != j[2] && g < 64; g++) a = par[a];
    if (!chain || a != j[2]) {
      TB_LOG("firstperson: framing = close: arm %d is not upper arm %d -> forearm %d -> hand %d (attachment %d); "
             "hands stay as animated",
             s, j[0], j[1], j[2], j[3]);
      return false;
    }
  }
  for (int i = 0; i < n; i++) {
    uint8_t sg = kSegNone;  // the nearest arm joint up the hierarchy decides
    for (int a = i, g = 0; a >= 0 && a < n && g < 64 && sg == kSegNone; a = par[a], g++)
      for (int s = 0; s < 2; s++)
        for (int k = 0; k < 3; k++)
          if (a == g_vs.joint[s][k] && sg == kSegNone) sg = uint8_t(s * 3 + k);
    g_vs.seg[i] = sg;
  }
  // The jacket's side panels (Jkt_L/R_Shoulder and their Front/Back/Rib chains, children of the
  // chest) are skinned up into the armpits: a raised arm stretches them into a flap of cloth in
  // front of the lens (live). They follow part of the upper arm's turn (segment 6 + side). Found
  // by name through the render bones (mesh +0x238: 0x80 per bone, name first; render map).
  g_vs.jacket[0] = g_vs.jacket[1] = -1;
  void* ro = render_object(ch);
  void* mesh = ro && readable(ro, 0x1f8) ? rd<void*>(ro, 0x80) : nullptr;
  void* anim = rd<void*>(ch, 0x1b8);
  auto* map = anim ? rd<int*>(anim, kAnimRenderMap) : nullptr;
  int nmap = anim ? rd<int>(anim, kAnimRenderCount) : 0;
  if (mesh && readable(mesh, 0x280) && map && nmap > 0 && nmap <= 0x100 && readable(map, size_t(nmap) * 4)) {
    auto* bl = rd<uint8_t*>(mesh, 0x238);
    int nb = rd<int>(mesh, 0x240);
    for (int i = 0; i < nb && i < nmap && readable(bl + size_t(i) * 0x80, 0x40); i++) {
      std::string nm = cstr_at(bl + size_t(i) * 0x80, 0x40);
      if (nm == "Jkt_L_Shoulder" && map[i] > 0 && map[i] < n) g_vs.jacket[0] = map[i];
      if (nm == "Jkt_R_Shoulder" && map[i] > 0 && map[i] < n) g_vs.jacket[1] = map[i];
    }
  }
  for (int i = 0; i < n; i++)
    for (int a = i, g = 0; g_vs.seg[i] == kSegNone && a >= 0 && a < n && g < 64; a = par[a], g++)
      for (int s = 0; s < 2; s++)
        if (a == g_vs.jacket[s]) g_vs.seg[i] = uint8_t(6 + s);
  for (int s = 0; s < 2; s++) {
    int c = par[g_vs.joint[s][0]];
    g_vs.clav[s] = c > 0 && g_vs.seg[c] == kSegNone ? c : -1;
    if (g_vs.clav[s] > 0) g_vs.seg[c] = uint8_t(8 + s);
  }
  g_vs.ok = true;
  TB_LOG("firstperson: framing = close: skeleton of %d joints, left arm %d/%d/%d (flashlight %d), right arm "
         "%d/%d/%d (weapon %d), jacket shoulders %d/%d",
         n, g_vs.joint[0][0], g_vs.joint[0][1], g_vs.joint[0][2], g_vs.joint[0][3], g_vs.joint[1][0],
         g_vs.joint[1][1], g_vs.joint[1][2], g_vs.joint[1][3], g_vs.jacket[0], g_vs.jacket[1]);
  return true;
}

// Character model space (the pose's, and the render bones') -> world: the controller height
// down, then the character transform (as Character::slotTransform does).
Xf char_model_to_world(void* ch) {
  Xf c = xf_from(reinterpret_cast<const float*>(static_cast<uint8_t*>(ch) + kCharTransform));
  void* ctl = rd<void*>(ch, 0x1c0);
  Xf y = xf_id();
  y.t[1] = ctl ? -rd<float>(ctl, 0xd0) : 0;
  return xf_mul(y, c);
}

struct VmState {
  bool cam_ok = false;    // a first-person camera was set since first person started
  Xf cam_m;               // the last main-view camera (mono eye), character model space
  Xf cam_smooth;          // ... low-passed (sway), model space
  bool smooth_ok = false;
  double aim_m[3] = {};   // the view's aim point, model space
  double tan_v = 0.5;     // tan(vertical half FOV) of the rendered view
  bool valid = false;     // the deltas are usable for g_vm_ch
  Xf d_world[10];         // per segment: new_world = old_world * d
  Xf d_model[10];         // the same in model space
  double wrist_cam[2][3] = {};  // diagnostics: animated wrist and its target, camera space
  double target_cam[2][3] = {};
  double aim_err_deg = 0;
  double raise[2] = {}, aim_out[2] = {};
  bool holds[2] = {};
  double hold_w[2] = {};
  // filters (camera space): slow wrist positions, aim factors, held-item directions
  bool filt_ok = false;
  double slow[2][3] = {}, aimf[2] = {}, fwd[2][3] = {};
  uint64_t computes = 0, applies = 0, slot_applies = 0;
} g_vm;
std::atomic<void*> g_vm_ch{nullptr};  // character the deltas apply to (null = none)
std::atomic<bool> g_vm_test{false};   // `fp vmtest on`: pose the arms in third person too (test aid)
std::mutex g_vm_mu;

// Per-segment deltas for this frame's pose (world space). g_vm_mu held.
//
// Per arm, in camera space (x right, y up, z ahead): the animated wrist w is split into its slow
// part (low-passed, hands_follow) and its fast part (recoil, reload and use gestures, the walk's
// swing). The slow part is replaced by a pose: "rest" (low in the frame at the arm's side) or
// "aim" (the gun just under the screen centre; the flashlight hand keeps its animated offset from
// the gun hand, so the two-handed grip stays), blended by how far the animation has raised that
// arm (the wrist's height against the shoulder). The fast part is added back. A two-bone IK puts
// the wrist there; the hand turns with the forearm and then towards its aim: the flashlight always
// at the view's aim point (Alan Wake's beam follows the camera), the gun at it when aiming and at a
// lower "ready" point at rest. The turn is computed from the low-passed held-item direction, so a
// recoil kick still shows.
double vm_dt() { return game_dt(); }

struct VmArm {
  bool ok = false;
  double S[3], E[3], W[3], C[3] = {}, la, lb;
  bool has_c = false;
  double w[3];      // wrist, camera space
  double raise;     // wrist height above the shoulder / arm length (-1 hanging .. 0 level)
  double attach[12];
};

bool vm_compute(void* ch, const Params& p) {
  if (!vm_skeleton(ch)) return false;
  Xf m = char_model_to_world(ch);
  Xf cam = xf_mul(g_vm.cam_smooth, m);
  double aim[3];
  xf_pt(m, g_vm.aim_m, aim);
  // The hands' frame: the view's, with its pitch limited to [-70, +30] deg: looking further up,
  // the hands stay where the shoulders can hold them and leave the bottom of the frame (an arm
  // posed up in front of the face folds the sleeves badly, live).
  double hr[3], hu[3], hf[3];
  {
    const double* vf = cam.r[2];
    double pitch = std::asin(vf[1] < -1 ? -1 : vf[1] > 1 ? 1 : vf[1]), h[3] = {vf[0], 0, vf[2]};
    if (v_len(h) < 1e-6) h[0] = -cam.r[0][2], h[2] = cam.r[0][0];  // straight up/down: right x up
    normalize(h);
    double pc = pitch < -70 * M_PI / 180 ? -70 * M_PI / 180 : pitch > 30 * M_PI / 180 ? 30 * M_PI / 180 : pitch;
    double up0[3] = {0, 1, 0};
    for (int i = 0; i < 3; i++) hf[i] = h[i] * std::cos(pc) + up0[i] * std::sin(pc);
    cross(up0, hf, hr);  // left-handed: right = up x forward
    normalize(hr);
    cross(hf, hr, hu);
  }
  const double *r = hr, *u = hu, *f = hf, *e = cam.t;
  double tv = g_vm.tan_v, th = tv * 16.0 / 9.0;
  double dt = vm_dt();
  bool reset = !g_vm.filt_ok || dt <= 0;
  auto lp = [&](double& state, double v, double tau) {
    state = reset || tau <= 0 ? v : state + (v - state) * (1.0 - std::exp(-dt / tau));
    return state;
  };
  auto to_cam = [&](const double* pw, double* o) {
    double d[3];
    v_sub(pw, e, d);
    o[0] = v_dot(d, r), o[1] = v_dot(d, u), o[2] = v_dot(d, f);
  };
  auto dir_cam = [&](const double* dw, double* o) { o[0] = v_dot(dw, r), o[1] = v_dot(dw, u), o[2] = v_dot(dw, f); };
  auto from_cam = [&](const double* c, double* o) {
    for (int i = 0; i < 3; i++) o[i] = e[i] + c[0] * r[i] + c[1] * u[i] + c[2] * f[i];
  };
  auto jw = reinterpret_cast<void(__fastcall*)(void*, float*, int)>(game_image().at(RVA_JOINT_WORLD));
  // an arm is posed only while it holds its item (the flashlight's or the weapon's mesh shown);
  // an empty hand stays as animated
  void* fl = rd<void*>(ch, kCharFlashlight);
  void* wp = rd<void*>(ch, kCharWeapon);
  bool holds[2] = {fl && rd<uint8_t>(fl, 0xc0) && !(rd<uint32_t>(rd<void*>(fl, 0xc8), kRoFlags) & 8),
                   wp && rd<uint8_t>(wp, 0x30) && !(rd<uint32_t>(rd<void*>(wp, 0x38), kRoFlags) & 8)};
  for (int s = 0; s < 2; s++) holds[s] = holds[s] && (p.hands_arms >> s & 1);
  VmArm A[2];
  for (int s = 0; s < 2; s++) {
    g_vm.d_world[s * 3] = g_vm.d_world[s * 3 + 1] = g_vm.d_world[s * 3 + 2] = g_vm.d_world[6 + s] =
        g_vm.d_world[8 + s] = xf_id();
    VmArm& a = A[s];
    g_vm.holds[s] = holds[s];
    float b[12];
    double* P[3] = {a.S, a.E, a.W};
    for (int k = 0; k < 3; k++) {
      jw(ch, b, g_vs.joint[s][k]);
      for (int i = 0; i < 3; i++) P[k][i] = b[9 + i];
    }
    double se[3], ew[3];
    v_sub(a.E, a.S, se), v_sub(a.W, a.E, ew);
    a.la = v_len(se), a.lb = v_len(ew);
    if (a.la < 0.05 || a.lb < 0.05 || a.la > 1 || a.lb > 1) continue;
    to_cam(a.W, a.w);
    a.raise = (a.W[1] - a.S[1]) / (a.la + a.lb);
    if (g_vs.clav[s] > 0) {
      jw(ch, b, g_vs.clav[s]);
      for (int i = 0; i < 3; i++) a.C[i] = b[9 + i];
      a.has_c = true;
    }
    g_o_slot(ch, b, kArm[s].attach);
    for (int i = 0; i < 12; i++) a.attach[i] = b[i];
    a.ok = true;
  }
  if (!A[0].ok && !A[1].ok) return false;
  // slow parts and aim factors (camera space)
  double slow[2][3], aimf[2], hw[2];
  for (int s = 0; s < 2; s++) {
    if (!A[s].ok) continue;
    // fades the pose in / out as the item is drawn or put away (or hidden by a use animation)
    hw[s] = lp(g_vm.hold_w[s], holds[s] ? 1.0 : 0.0, 0.15);
    for (int i = 0; i < 3; i++) slow[s][i] = lp(g_vm.slow[s][i], A[s].w[i], p.hands_follow);
    // live (`fp vm`, level 03): flashlight arm at rest -0.62, gun arm hanging -0.95, both
    // aiming -0.07..0
    double raw = (A[s].raise + 0.55) / 0.45;
    aimf[s] = lp(g_vm.aimf[s], raw < 0 ? 0 : raw > 1 ? 1 : raw, 0.08);
  }
  double z = p.hands_forward;
  double rest[2][3] = {{-p.hands_side * th * z, -p.hands_drop * tv * z, z},
                       {p.hands_side * th * z, -p.hands_drop * tv * z, z}};
  double zaim = z + 0.08;
  double aimpos[2][3] = {{-0.10 * th * zaim, -p.hands_sight * tv * zaim, zaim},
                         {0.10 * th * zaim, -p.hands_sight * tv * zaim, zaim}};
  // two-handed: the flashlight hand keeps its animated offset from the gun hand
  if (A[0].ok && A[1].ok) {
    double k = aimf[1];
    for (int i = 0; i < 3; i++) aimpos[0][i] += ((aimpos[1][i] + slow[0][i] - slow[1][i]) - aimpos[0][i]) * k;
  }
  for (int s = 0; s < 2; s++) {
    VmArm& a = A[s];
    if (!a.ok) continue;
    // the fast part, scaled by hands_motion and at most 5 cm (the walk swings the wrists by up to
    // 50 cm, live; a raise into aim is mostly slow and must not overshoot)
    double fast[3], t[3];
    v_sub(a.w, slow[s], fast);
    double fl = v_len(fast) * p.hands_motion, fmax = 0.05, fk = p.hands_motion * (fl > fmax ? fmax / fl : 1.0);
    for (int i = 0; i < 3; i++) {
      double base = rest[s][i] + (aimpos[s][i] - rest[s][i]) * aimf[s];
      t[i] = base + fast[i] * fk;
      t[i] = a.w[i] + (t[i] - a.w[i]) * hw[s];
    }
    double zmin = a.w[2] < 0.18 ? a.w[2] + (0.18 - a.w[2]) * hw[s] : 0.18;  // never into the lens
    if (t[2] < zmin) t[2] = zmin;
    memcpy(g_vm.wrist_cam[s], a.w, sizeof a.w);
    memcpy(g_vm.target_cam[s], t, sizeof t);
    g_vm.raise[s] = a.raise;
    g_vm.aim_out[s] = aimf[s];
    if (hw[s] < 1e-3) continue;  // as animated
    double Wt[3], d[3], se[3], ew[3];
    from_cam(t, Wt);
    // The clavicle takes hands_shrug of the arm's swing (the shoulders come forward with the
    // arms, which spreads the bend over two joints: a 150 deg swing at the shoulder alone pulls
    // the armpit's cloth into a flap, live); the animated arm moves with it before the IK.
    Xf dc = xf_id();
    double rc[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (a.has_c && p.hands_shrug > 0) {
      double sw0[3], tw[3];
      v_sub(a.W, a.S, sw0), v_sub(Wt, a.S, tw);
      rot_arc(sw0, tw, p.hands_shrug * hw[s], M_PI, rc);
      dc = xf_pivot(a.C, rc, a.C);
      double q[3];
      for (double* pt : {a.S, a.E, a.W}) xf_pt(dc, pt, q), memcpy(pt, q, sizeof q);
      double ap[3] = {a.attach[9], a.attach[10], a.attach[11]};
      xf_pt(dc, ap, q);
      for (int r3 = 0; r3 < 3; r3++) {
        double row[3] = {a.attach[r3 * 3], a.attach[r3 * 3 + 1], a.attach[r3 * 3 + 2]};
        for (int i = 0; i < 3; i++)
          a.attach[r3 * 3 + i] = row[0] * rc[0][i] + row[1] * rc[1][i] + row[2] * rc[2][i];
      }
      for (int i = 0; i < 3; i++) a.attach[9 + i] = q[i];
    }
    v_sub(a.E, a.S, se), v_sub(a.W, a.E, ew);
    // two-bone IK: shoulder fixed, wrist to Wt, the elbow in the animated elbow's plane
    v_sub(Wt, a.S, d);
    double L = v_len(d), lo = std::fabs(a.la - a.lb) + 1e-3, hi = (a.la + a.lb) * 0.999;
    if (L < 1e-4) continue;
    L = L < lo ? lo : L > hi ? hi : L;
    double n[3] = {d[0], d[1], d[2]};
    normalize(n);
    for (int i = 0; i < 3; i++) Wt[i] = a.S[i] + n[i] * L;
    double ca = (a.la * a.la + L * L - a.lb * a.lb) / (2 * a.la * L);
    ca = ca > 1 ? 1 : ca < -1 ? -1 : ca;
    // the pole: the animated elbow after swinging the whole arm rigidly onto the new shoulder ->
    // wrist line, so the arm's plane (and the upper arm's roll) changes as little as possible
    double sw0[3], sw[3][3], ses[3];
    v_sub(a.W, a.S, sw0);
    rot_arc(sw0, n, 1, M_PI, sw);
    for (int i = 0; i < 3; i++) ses[i] = se[0] * sw[0][i] + se[1] * sw[1][i] + se[2] * sw[2][i];
    double sa = std::sqrt(1 - ca * ca), pole[3], kk = v_dot(ses, n);
    for (int i = 0; i < 3; i++) pole[i] = ses[i] - n[i] * kk;
    if (v_len(pole) < 1e-4)
      for (int i = 0; i < 3; i++) pole[i] = -u[i];
    normalize(pole);
    double Et[3], se2[3], ew2[3], rot_u[3][3], rot_b[3][3], rot_f[3][3], v[3];
    for (int i = 0; i < 3; i++) Et[i] = a.S[i] + n[i] * a.la * ca + pole[i] * a.la * sa;
    v_sub(Et, a.S, se2);
    v_sub(Wt, Et, ew2);
    // The upper arm takes the arm's plane (shoulder, elbow, wrist) onto the new one, so the elbow
    // stays a hinge (a shortest-arc turn of each bone twists the elbow, which shows as a ribbon of
    // sleeve, live); with a straight animated arm (no plane), the shortest arc.
    double n1[3], n2[3];
    cross(se, ew, n1);
    cross(se2, ew2, n2);
    if (v_len(n1) > 1e-3 * a.la * a.lb && v_len(n2) > 1e-3 * a.la * a.lb) {
      double a1[3] = {se[0], se[1], se[2]}, a2[3] = {se2[0], se2[1], se2[2]}, b1[3], b2[3];
      normalize(a1), normalize(a2), normalize(n1), normalize(n2);
      cross(a1, n1, b1);
      cross(a2, n2, b2);
      const double* F1[3] = {a1, n1, b1};
      const double* F2[3] = {a2, n2, b2};
      for (int i = 0; i < 3; i++)  // R = F1^T F2 (row vectors: a1 -> a2, n1 -> n2)
        for (int c = 0; c < 3; c++) rot_u[i][c] = F1[0][i] * F2[0][c] + F1[1][i] * F2[1][c] + F1[2][i] * F2[2][c];
    } else {
      rot_arc(se, se2, 1, M_PI, rot_u);
    }
    for (int i = 0; i < 3; i++) v[i] = ew[0] * rot_u[0][i] + ew[1] * rot_u[1][i] + ew[2] * rot_u[2][i];
    rot_arc(v, ew2, 1, M_PI, rot_b);
    for (int i = 0; i < 3; i++)
      for (int c = 0; c < 3; c++)
        rot_f[i][c] = rot_u[i][0] * rot_b[0][c] + rot_u[i][1] * rot_b[1][c] + rot_u[i][2] * rot_b[2][c];
    // The flashlight hand keeps its animated orientation (the animation already points the beam
    // at the camera's target); the gun hand turns with the forearm (the animated wrist bend is
    // kept: a hanging gun comes up with the arm). Then each turns towards its aim.
    double rot_h[3][3], id3[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    memcpy(rot_h, s == 0 ? id3 : rot_f, sizeof rot_h);
    double (*rb)[3] = s == 0 ? id3 : rot_f;
    double weight = p.hands_aim * hw[s];
    if (weight > 0) {
      const double* at = a.attach;
      double fw[3], rel[3], pos[3], want[3], fwc[3], fws[3], dirc[3], ra[3][3];
      for (int i = 0; i < 3; i++) fw[i] = at[6] * rb[0][i] + at[7] * rb[1][i] + at[8] * rb[2][i];
      double ap[3] = {at[9], at[10], at[11]};
      v_sub(ap, a.W, rel);
      for (int i = 0; i < 3; i++) pos[i] = Wt[i] + rel[0] * rb[0][i] + rel[1] * rb[1][i] + rel[2] * rb[2][i];
      // where it should point: the aim point; the gun at rest at a "ready" point below it
      v_sub(aim, pos, want);
      normalize(want);
      dir_cam(want, dirc);
      if (s == 1) {
        double ready[3] = {dirc[0] * 0.7, dirc[1] - 0.2, dirc[2]};  // ~11 deg lower, a little inwards
        normalize(ready);
        for (int i = 0; i < 3; i++) dirc[i] = ready[i] + (dirc[i] - ready[i]) * aimf[1];
      }
      // turn the low-passed direction onto the wanted one, so fast kicks still show
      normalize(fw);
      dir_cam(fw, fwc);
      for (int i = 0; i < 3; i++) fws[i] = lp(g_vm.fwd[s][i], fwc[i], 0.12);
      rot_arc(fws, dirc, weight, (s == 0 ? 75 : 80) * M_PI / 180, ra);  // camera-space rotation
      // to world: R_w = C^T R_c C (row-vector: world dir -> cam -> rotate -> world)
      double cm[3][3] = {{r[0], u[0], f[0]}, {r[1], u[1], f[1]}, {r[2], u[2], f[2]}};  // world -> cam
      double tmp[3][3], rw[3][3];
      for (int i = 0; i < 3; i++)
        for (int c = 0; c < 3; c++) tmp[i][c] = cm[i][0] * ra[0][c] + cm[i][1] * ra[1][c] + cm[i][2] * ra[2][c];
      for (int i = 0; i < 3; i++)
        for (int c = 0; c < 3; c++) rw[i][c] = tmp[i][0] * r[c] + tmp[i][1] * u[c] + tmp[i][2] * f[c];
      double fsn[3] = {fws[0], fws[1], fws[2]};
      normalize(fsn);
      if (s == 0) g_vm.aim_err_deg = std::acos(std::fmin(1.0, v_dot(fsn, dirc))) * 180 / M_PI;
      for (int i = 0; i < 3; i++)
        for (int c = 0; c < 3; c++) rot_h[i][c] = rb[i][0] * rw[0][c] + rb[i][1] * rw[1][c] + rb[i][2] * rw[2][c];
    }
    // (the deltas below act on the shrugged arm: the clavicle's first)
    g_vm.d_world[8 + s] = dc;
    g_vm.d_world[s * 3 + 0] = xf_mul(dc, xf_pivot(a.S, rot_u, a.S));
    double rot_j[3][3];
    rot_scale(rot_u, p.hands_jacket, rot_j);
    g_vm.d_world[6 + s] = xf_mul(dc, xf_pivot(a.S, rot_j, a.S));
    g_vm.d_world[s * 3 + 1] = xf_mul(dc, xf_pivot(a.E, rot_f, Et));
    g_vm.d_world[s * 3 + 2] = xf_mul(dc, xf_pivot(a.W, rot_h, Wt));
  }
  g_vm.filt_ok = true;
  Xf mi = xf_inv(m);
  for (int i = 0; i < 10; i++) g_vm.d_model[i] = xf_mul(xf_mul(m, g_vm.d_world[i]), mi);
  g_vm.computes++;
  return true;
}

// 0x184080: the model-space pose exported for the render bones.
void __fastcall hk_export(void* anim, float* out, int n) {
  g_o_export(anim, out, n);
  void* ch = player();
  bool test = g_vm_test.load(std::memory_order_relaxed);
  if (!ch || anim != rd<void*>(ch, 0x1b8)) return;
  if (!(g_on.load(std::memory_order_relaxed) || test)) {
    // third person: nothing applied (the slot hook passes through once g_vm_ch is null)
    if (g_vm_ch.load(std::memory_order_relaxed)) {
      std::lock_guard lk(g_vm_mu);
      g_vm_ch = nullptr;
      g_vm.valid = g_vm.filt_ok = false;
    }
    return;
  }
  Params p = params();
  std::lock_guard lk(g_vm_mu);
  bool want = (p.framing_close || test) && g_vm_ok && (g_active.load(std::memory_order_relaxed) || test) &&
              g_vm.cam_ok && !game_hides(ch);
  g_vm.valid = want && vm_compute(ch, p);
  if (!g_vm.valid) g_vm.filt_ok = false;
  g_vm_ch = g_vm.valid ? ch : nullptr;
  if (!g_vm.valid) return;
  auto* map = rd<int*>(anim, kAnimRenderMap);
  int cnt = rd<int>(anim, kAnimRenderCount);
  for (int i = 0; map && i < cnt && i < 0x100; i++) {
    int j = map[i];
    if (j < 0 || j >= g_vs.n || g_vs.seg[j] == kSegNone) continue;
    xf_to(xf_mul(xf_from(out + size_t(i) * 12), g_vm.d_model[g_vs.seg[j]]), out + size_t(i) * 12);
  }
  g_vm.applies++;
}

// Character::slotTransform: the player's arm slots (held items, hands) get the same deltas.
void __fastcall hk_slot(void* ch, float* out, int slot) {
  g_o_slot(ch, out, slot);
  if (ch != g_vm_ch.load(std::memory_order_relaxed) || slot < 0 || slot >= 0x60) return;
  std::lock_guard lk(g_vm_mu);
  if (!g_vm.valid || ch != g_vm_ch.load() || !g_vs.ok) return;
  void* x = anim_x(ch);
  int j = x ? rd<int>(x, kXSlots + size_t(slot) * 4) : -1;
  if (j < 0 || j >= g_vs.n || g_vs.seg[j] == kSegNone) return;
  xf_to(xf_mul(xf_from(out), g_vm.d_world[g_vs.seg[j]]), out);
  g_vm.slot_applies++;
}

// From the filter: the camera just set (mono eye) and its aim point, kept in model space; `sway`
// low-passes the camera's orientation for placing the hands (they trail a fast turn slightly).
void vm_camera(void* ch, const double* mm, const double* aim, double dt, const Params& p) {
  std::lock_guard lk(g_vm_mu);
  if (!ch) {
    g_vm.cam_ok = g_vm.smooth_ok = g_vm.filt_ok = false;
    return;
  }
  Xf c;
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 3; k++) c.r[i][k] = mm[i * 3 + k];
  for (int k = 0; k < 3; k++) c.t[k] = mm[9 + k];
  Xf mi = xf_inv(char_model_to_world(ch));
  g_vm.cam_m = xf_mul(c, mi);
  xf_pt(mi, aim, g_vm.aim_m);
  g_vm.cam_ok = true;
  if (p.hands_sway > 0 && g_vm.smooth_ok) {
    Xf& s = g_vm.cam_smooth;
    if (dt > 0) {
      double a = 1.0 - std::exp(-dt / p.hands_sway);
      for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) s.r[i][k] += (g_vm.cam_m.r[i][k] - s.r[i][k]) * a;
      // re-orthonormalise (forward, then up)
      normalize(s.r[2]);
      cross(s.r[1], s.r[2], s.r[0]);  // right = up x forward (left-handed)
      normalize(s.r[0]);
      cross(s.r[2], s.r[0], s.r[1]);
    }
    memcpy(s.t, g_vm.cam_m.t, sizeof s.t);
  } else {
    g_vm.cam_smooth = g_vm.cam_m;
  }
  g_vm.smooth_ok = p.hands_sway > 0;
}

// core/camera.h filter: the main view's transform -> the player's eyes.
bool fp_filter(void* view, double* m) {
  if (g_job_pending.load(std::memory_order_relaxed) && G.ok && G.cameraman && view == *G.cameraman) job_run();
  if (g_slots_req.load(std::memory_order_relaxed) && G.ok && G.cameraman && view == *G.cameraman) slots_census();
  if (g_freecam_req.load(std::memory_order_relaxed) >= 0 && G.ok) freecam_apply();
  g_game_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  if (!g_on.load(std::memory_order_relaxed)) {
    if (g_vm_test.load(std::memory_order_relaxed) && g_vm_ok && G.ok && G.cameraman && view == *G.cameraman) {
      // test aid: the arms posed for a first-person eye (head slot, looking along the character's
      // facing) while the third-person view shows them (orbit the camera to look at them)
      void* ch = player();
      Params p = params();
      if (ch && *G.cam_mode == 0 && eye_position(ch, m, p)) {
        double mm[12];
        const float* ct = reinterpret_cast<const float*>(static_cast<uint8_t*>(ch) + kCharTransform);
        for (int i = 0; i < 9; i++) mm[i] = ct[i];
        for (int i = 0; i < 3; i++) mm[9 + i] = g_eye[i];
        double aim[3];
        for (int i = 0; i < 3; i++) aim[i] = g_eye[i] + mm[6 + i] * 15;
        vm_camera(ch, mm, aim, g_frame_dt, p);
      }
    }
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
    if (g_vm_ok) vm_camera(nullptr, nullptr, nullptr, 0, p);
    g_smooth_valid = false;
    g_cache_frame = ~0ull;
    hide_sync();
    return false;
  }
  for (int i = 0; i < 3; i++) g_camfwd[i] = m[6 + i];
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
  if (g_vm_ok) {
    // the viewmodel's camera and aim point (the point the view converges on)
    double dist = p.aim_distance > 0 ? p.aim_distance : 15, aim[3];
    for (int i = 0; i < 3; i++) aim[i] = g_cam[i] + g_camfwd[i] * dist;
    static uint64_t last_frame = ~0ull;
    uint64_t fr = present_frame();
    vm_camera(ch, m, aim, fr != last_frame ? g_frame_dt : 0, p);
    last_frame = fr;
  }
  hide_sync();
  return true;
}

// The rendered camera's near plane (PerspectiveView +0xcbc, "Camera:Near" 0.2 m): framing = close
// brings the arms within a few centimetres of the eye, and at 0.2 m the near plane cuts the
// sleeves open (the inside of a cut sleeve shows as a hollow ribbon, live). Lowered while first
// person is active, the game's value put back after.
float g_near_orig = 0;  // the game's near plane while ours is applied, else 0
void near_apply(void* view, double want) {
  auto* b = static_cast<uint8_t*>(view);
  float cur = rd<float>(view, kViewNear);
  float w = want > 0 ? float(want) : g_near_orig;
  if (want > 0 && g_near_orig == 0) g_near_orig = cur;
  if (want <= 0) g_near_orig = 0;
  if (w > 0 && cur != w) {
    memcpy(b + kViewNear, &w, sizeof w);
    b[kViewDirtyProj] = 0;
    b[kViewDirtyWorld] = 0;
  }
}

// core/camera.h pre-render: first-person FOV and near plane.
void fp_prerender(void* view) {
  if (G.cameraman && view == *G.cameraman && g_near_orig != 0 &&
      (!g_on.load(std::memory_order_relaxed) || !g_active.load(std::memory_order_relaxed)))
    near_apply(view, 0);  // first person stopped: the game's near plane back
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
  near_apply(view, p.near_m > 0 ? p.near_m : p.framing_close && g_vm_ok ? 0.05 : 0);
  float aspect = rd<float>(view, kViewAspect);
  if (!(aspect > 0.1f && aspect < 10.f)) return;
  if (g_vm_ok) {
    // the viewmodel's box is relative to the rendered frame
    float hf = rd<float>(view, kViewFov);
    double tv = std::tan(hf * 0.5) / aspect;
    if (tv > 0.05 && tv < 5) {
      std::lock_guard lk(g_vm_mu);
      g_vm.tan_v = tv;
    }
  }
  if (p.fov_deg <= 1 || p.fov_deg >= 170) return;
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
    if (id == "camera.near") g_p.near_m = n > 0.01 && n < 1 ? n : 0;
    if (id == "camera.smooth") g_p.smooth = n > 0 ? n : 0;
    if (id == "camera.aim_distance") g_p.aim_distance = n > 0 ? n : 0;
    if (id == "camera.framing") g_p.framing_close = v != "classic";
    if (id == "camera.hands_forward") g_p.hands_forward = n < 0.1 ? 0.1 : n > 0.8 ? 0.8 : n;
    if (id == "camera.hands_drop") g_p.hands_drop = n < 0 ? 0 : n > 1.5 ? 1.5 : n;
    if (id == "camera.hands_side") g_p.hands_side = n < 0 ? 0 : n > 1.5 ? 1.5 : n;
    if (id == "camera.hands_aim") g_p.hands_aim = n < 0 ? 0 : n > 1 ? 1 : n;
    if (id == "camera.hands_sight") g_p.hands_sight = n < -0.5 ? -0.5 : n > 1.5 ? 1.5 : n;
    if (id == "camera.hands_follow") g_p.hands_follow = n < 0 ? 0 : n > 2 ? 2 : n;
    if (id == "camera.hands_motion") g_p.hands_motion = n < 0 ? 0 : n > 1 ? 1 : n;
    if (id == "camera.hands_arms") g_p.hands_arms = v.empty() ? 3 : int(n);
    if (id == "camera.hands_jacket") g_p.hands_jacket = n < 0 ? 0 : n > 1 ? 1 : n;
    if (id == "camera.hands_shrug") g_p.hands_shrug = n < 0 ? 0 : n > 0.8 ? 0.8 : n;
    if (id == "camera.hands_sway") g_p.hands_sway = n < 0 ? 0 : n > 0.5 ? 0.5 : n;
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
    if (a[0] == "skel" && G.ok) {
      // RE aid: the player's animation skeleton (hkaSkeleton through the pose), slot table and
      // the render-bone map.
      return job_call([] {
        void* ch = player();
        if (!ch) return json_str("no player");
        void* anim = rd<void*>(ch, 0x1b8);
        void* x = anim ? rd<void*>(anim, 8) : nullptr;
        void* pose = x ? rd<void*>(x, 8) : nullptr;
        void* skel = pose ? rd<void*>(pose, 0) : nullptr;
        char b[256];
        snprintf(b, sizeof b, "{\"anim\":\"%p\",\"x\":\"%p\",\"pose\":\"%p\",\"skel\":\"%p\"", anim, x, pose, skel);
        std::string o = b;
        if (!readable(skel, 0x60)) return o + "}";
        // skeleton: +8 parent indices (int16), +0x10 count, +0x18 names (char*), +0x28 reference
        // pose (0x30 per joint), each array followed by its count (live)
        auto* par = rd<int16_t*>(skel, 0x8);
        int np = rd<int>(skel, 0x10);
        auto* bones = rd<uint8_t*>(skel, 0x18);
        int nb = rd<int>(skel, 0x20);
        snprintf(b, sizeof b, ",\"nparents\":%d,\"nbones\":%d,\"joints\":[", np, nb);
        o += b;
        const float* root = reinterpret_cast<const float*>(static_cast<uint8_t*>(ch) + kCharTransform);
        for (int i = 0; i < nb && i < 512; i++) {
          std::string name = readable(bones, size_t(nb) * 8) ? cstr_at(rd<void*>(bones + size_t(i) * 8, 0)) : "";
          float m[12];
          memcpy(m, root, sizeof m);
          reinterpret_cast<void(__fastcall*)(void*, float*, int)>(game_image().at(0x179e30))(ch, m, i);
          snprintf(b, sizeof b, "%s[%d,\"%s\",%d,[%.3f,%.3f,%.3f]]", i ? "," : "", i, name.c_str(),
                   i < np ? par[i] : -9, m[9] - root[9], m[10] - root[10], m[11] - root[11]);
          o += b;
        }
        o += "],\"slots\":[";
        for (int s = 0; s < 64; s++) {
          snprintf(b, sizeof b, "%s%d", s ? "," : "", rd<int>(x, 0x50 + size_t(s) * 4));
          o += b;
        }
        o += "],\"render_map\":[";
        auto* map = rd<int*>(anim, 0x48f8);
        int nm = rd<int>(anim, 0x4900);
        for (int i = 0; i < nm && i < 512 && readable(map, size_t(nm) * 4); i++) {
          snprintf(b, sizeof b, "%s%d", i ? "," : "", map[i]);
          o += b;
        }
        snprintf(b, sizeof b, "],\"render_count\":%d}", nm);
        return o + b;
      }, ok);
    }
    if (a[0] == "vmtest" && a.size() > 1) {
      g_vm_test = a[1] == "on";
      return json_str(g_vm_test ? "arms posed in third person too" : "arms posed in first person only");
    }
    if (a[0] == "vm") {
      // framing = close diagnostics: wrists (animated) and their targets in camera space
      // (x right, y up, z ahead, m), the flashlight's turn to the aim point, counters.
      std::lock_guard lk(g_vm_mu);
      char b[768];
      snprintf(b, sizeof b,
               "{\"ok\":%s,\"valid\":%s,\"cam\":%s,\"tan_v\":%.3f,\"left_wrist\":%s,\"left_target\":%s,"
               "\"right_wrist\":%s,\"right_target\":%s,\"flashlight_turn_deg\":%.1f,\"raise\":[%.2f,%.2f],"
               "\"aim\":[%.2f,%.2f],\"dt\":%.4f,\"computes\":%llu,\"applies\":%llu,\"slot_applies\":%llu}",
               g_vm_ok ? "true" : "false", g_vm.valid ? "true" : "false", g_vm.cam_ok ? "true" : "false", g_vm.tan_v,
               fmt3(g_vm.wrist_cam[0]).c_str(), fmt3(g_vm.target_cam[0]).c_str(), fmt3(g_vm.wrist_cam[1]).c_str(),
               fmt3(g_vm.target_cam[1]).c_str(), g_vm.aim_err_deg, g_vm.raise[0], g_vm.raise[1], g_vm.aim_out[0],
               g_vm.aim_out[1], game_dt(), (unsigned long long)g_vm.computes,
               (unsigned long long)g_vm.applies, (unsigned long long)g_vm.slot_applies);
      return std::string(b);
    }
    if (a[0] == "slotm" && G.ok) {
      // RE aid: world transforms (rows right, up, forward, position) of the given slots, and the
      // game camera's forward.
      std::vector<int> slots;
      for (size_t i = 1; i < a.size(); i++) slots.push_back(atoi(a[i].c_str()));
      return job_call([slots] {
        void* ch = player();
        if (!ch) return json_str("no player");
        std::string o = "{";
        char b[400];
        for (int s : slots) {
          float m[12];
          G.bone_slot(ch, m, s);
          snprintf(b, sizeof b, "\"%d\":[[%.3f,%.3f,%.3f],[%.3f,%.3f,%.3f],[%.3f,%.3f,%.3f],[%.3f,%.3f,%.3f]],", s, m[0],
                   m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
          o += b;
        }
        snprintf(b, sizeof b, "\"cam_fwd\":[%.3f,%.3f,%.3f],\"cam\":%s,\"eye\":%s}", g_camfwd[0], g_camfwd[1],
                 g_camfwd[2], fmt3(g_cam).c_str(), fmt3(g_eye).c_str());
        return o + b;
      }, ok);
    }
    if (a[0] == "peek" && a.size() > 1) {
      // RE aid: N bytes (default 64) at an RVA (< 0x10000000) or an absolute address, as hex.
      uintptr_t v = strtoull(a[1].c_str(), nullptr, 0);
      size_t n = a.size() > 2 ? strtoul(a[2].c_str(), nullptr, 0) : 64;
      if (n > 1024) n = 1024;
      auto* p = reinterpret_cast<const uint8_t*>(v < 0x10000000 ? game_image().at(v) : v);
      if (!readable(p, n)) {
        ok = false;
        return json_str("unreadable");
      }
      std::string o;
      char t[4];
      for (size_t i = 0; i < n; i++) snprintf(t, sizeof t, "%02x", p[i]), o += t;
      return json_str(o);
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
        "usables [mask] [radius]|hidetest on|off|vm|vmtest on|off|skel|slotm S...|peek ADDR [N]]");
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
  // framing = close: the viewmodel arms (without them, close keeps the classic arms).
  g_vm_ok = true;
  for (auto& s : kVmSigs)
    if (!sig_ok(s.rva, s.hex)) {
      TB_LOG("firstperson: signature mismatch at rva 0x%zx; framing = close keeps the animated arms (classic)",
             (size_t)s.rva);
      g_vm_ok = false;
      break;
    }
  if (g_vm_ok && !hook_fn(reinterpret_cast<void*>(game_image().at(RVA_POSE_EXPORT)),
                          reinterpret_cast<void*>(&hk_export), reinterpret_cast<void**>(&g_o_export),
                          "Animation pose export")) {
    g_vm_ok = false;
  }
  if (g_vm_ok) {
    if (hook_fn(reinterpret_cast<void*>(game_image().at(RVA_BONE_SLOT)), reinterpret_cast<void*>(&hk_slot),
                reinterpret_cast<void**>(&g_o_slot), "Character::slotTransform")) {
      G.bone_slot = g_o_slot;
    } else {
      // the export hook stays in but never applies (g_vm_ok false)
      g_vm_ok = false;
    }
  }
  if (!g_vm_ok) TB_LOG("firstperson: framing = close unavailable; the arms stay as animated");
  TB_LOG("firstperson: ready (rendered camera at %p)", (void*)G.cameraman);
}

}  // namespace tb
