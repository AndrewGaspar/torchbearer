#include "features/stereo/pass2.h"

#include <windows.h>

#include <algorithm>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"

namespace tb::stereo {
namespace {

// --- engine entry points (RVAs; docs/stereo.md section 4) ----------------------------------------
// rend::Flare vtable slot 8 (vtable RVA 0x78f768): per render, reads the flare's occlusion query,
// pushes the visible fraction into a 10-sample ring (+0xd0 index, +0xd4 count, +0xd8 capacity,
// +0xe0 float*), steps a fade (+0xe8 target, +0xec value, by the frame time 0xd74504) and resets
// the ring unless it last ran on the previous frame (+0x130 = frame counter 0xd744f8 of its last
// run). Twice in one frame the second call resets the ring: no flare in the second eye. (static)
constexpr uintptr_t kRvaFlareRender = 0x492bb0;
// RendererX86 shadow pass (called once per renderWorld: early from 0x5090b0 or late from the main
// pass 0x506b20, by the "Late Shadow Pass" switch): shadow-filter globals, the shadow manager's
// begin (RendererX86+0xf0 vtbl+0x18), each visible light's shadow render (vtbl+0x40). (static)
constexpr uintptr_t kRvaShadowPass = 0x50dba0;
// The sky cube probe (from the sky/atmosphere update 0x4a85d0 at the start of renderWorld): the
// 6-face sky cube, its filtering, the SH terms, g_sAmbientSHTerms / g_sEnvironmentReflectionCube.
// Static PerspectiveView 0xdc44f0 (the 6 setViewToWorld per frame in the census). (static, live)
constexpr uintptr_t kRvaSkyProbe = 0x512220;

struct Sig {
  uintptr_t rva;
  const char* name;
  uint8_t bytes[16];
};
constexpr Sig kFlareSig = {kRvaFlareRender, "Flare::render",
                           {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x41, 0x56, 0x48, 0x8d, 0x68, 0x88, 0x48}};
constexpr Sig kShadowSig = {kRvaShadowPass, "RendererX86 shadow pass",
                            {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x10, 0x49, 0x89, 0x6b, 0x18, 0x49, 0x89, 0x73, 0x20, 0x57}};
constexpr Sig kProbeSig = {kRvaSkyProbe, "sky cube probe",
                           {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54}};

bool sig_ok(const Sig& s) {
  const auto* b = reinterpret_cast<const uint8_t*>(game_image().at(s.rva));
  if (memcmp(b, s.bytes, sizeof s.bytes) == 0) return true;
  TB_LOG("stereo pass2: signature mismatch for %s at RVA %#llx (%02x %02x %02x %02x ...); that part disabled", s.name,
         (unsigned long long)s.rva, b[0], b[1], b[2], b[3]);
  return false;
}

std::atomic<int> g_slot{-1};
std::atomic<bool> g_skip_shadows{false}, g_skip_probe{true}, g_flare_rewind{true};
std::atomic<uint64_t> g_n_shadow_skip{0}, g_n_probe_skip{0}, g_n_flare_rewind{0}, g_n_flare_first{0};

// --- flares ------------------------------------------------------------------------------------
namespace flare {
constexpr size_t kIdx = 0xd0, kCount = 0xd4, kCap = 0xd8, kRing = 0xe0, kTarget = 0xe8, kFade = 0xec, kLast = 0x130;
constexpr int kMaxRing = 32;
struct Snap {
  int idx, count, target, last;
  float fade;
  int cap;
  float ring[kMaxRing];
};
template <typename T>
T& f(void* o, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(o) + off);
}
bool take(void* o, Snap& s) {
  s.cap = f<int>(o, kCap);
  float* ring = f<float*>(o, kRing);
  if (s.cap < 0 || s.cap > kMaxRing || !ring) return false;
  s.idx = f<int>(o, kIdx);
  s.count = f<int>(o, kCount);
  s.target = f<int>(o, kTarget);
  s.last = f<int>(o, kLast);
  s.fade = f<float>(o, kFade);
  memcpy(s.ring, ring, sizeof(float) * s.cap);
  return true;
}
void put(void* o, const Snap& s) {
  if (f<int>(o, kCap) != s.cap || !f<float*>(o, kRing)) return;
  f<int>(o, kIdx) = s.idx;
  f<int>(o, kCount) = s.count;
  f<int>(o, kTarget) = s.target;
  f<int>(o, kLast) = s.last;
  f<float>(o, kFade) = s.fade;
  memcpy(f<float*>(o, kRing), s.ring, sizeof(float) * s.cap);
}
std::unordered_map<void*, Snap> g_before;  // state before the first eye's call (game thread only)
}  // namespace flare

using Fn2_t = void(__fastcall*)(void*, void*);
using Fn1_t = void(__fastcall*)(void*);
using Fn3_t = void(__fastcall*)(void*, void*, void*);
Fn2_t o_flareRender;
Fn3_t o_shadowPass;
Fn2_t o_skyProbe;

void __fastcall hk_flareRender(void* self, void* ctx) {
  int slot = g_slot.load(std::memory_order_relaxed);
  if (slot < 0 || !g_flare_rewind.load(std::memory_order_relaxed)) {
    o_flareRender(self, ctx);
    return;
  }
  if (slot == 0) {  // first eye: remember the state it starts from
    flare::Snap s;
    if (flare::take(self, s)) flare::g_before[self] = s;
    g_n_flare_first++;
    o_flareRender(self, ctx);
    return;
  }
  // Second eye: start from the same state as the first eye (same query result, same frame time
  // -> the same visibility and fade), then keep the state the first eye left (advanced once).
  auto it = flare::g_before.find(self);
  flare::Snap after;
  if (it == flare::g_before.end() || !flare::take(self, after)) {
    o_flareRender(self, ctx);
    return;
  }
  flare::put(self, it->second);
  o_flareRender(self, ctx);
  flare::put(self, after);
  g_n_flare_rewind++;
}

// The pass: shadow manager begin (RendererX86+0xf0 vtbl+0x18), then each light in `lights`
// ({void** data, size_t count}) renders its shadow (vtbl+0x40), then an optional end (+0x28)
// (static). The second eye may reuse the first eye's shadows only if it sees no light the first
// eye did not render: the eyes' frusta differ slightly, and a light visible only to the second
// eye would be lit with shadow state nobody prepared this frame. Skipping unconditionally
// crashed the game (clean exit, no exception) ~9 s into level 15 when the player's flashlight
// came on after the opening cinematic (live, 3 of 3 runs; probe reuse alone: no exit).
std::vector<void*> g_eye1_lights;
std::atomic<uint64_t> g_n_shadow_fallback{0};

struct LightList {
  void** data;
  size_t count;
};

void __fastcall hk_shadowPass(void* r, void* lights, void* objs) {
  int slot = g_slot.load(std::memory_order_relaxed);
  auto* ll = static_cast<LightList*>(lights);
  if (slot == 0) {
    g_eye1_lights.assign(ll && ll->data ? ll->data : nullptr, ll && ll->data ? ll->data + ll->count : nullptr);
  } else if (slot == 1 && g_skip_shadows.load(std::memory_order_relaxed)) {
    bool subset = ll != nullptr;
    for (size_t i = 0; subset && ll->data && i < ll->count; i++)
      subset = std::find(g_eye1_lights.begin(), g_eye1_lights.end(), ll->data[i]) != g_eye1_lights.end();
    if (subset) {
      g_n_shadow_skip++;
      return;  // the first eye's shadow maps (and the lights' shadow matrices) are still current
    }
    g_n_shadow_fallback++;
  }
  o_shadowPass(r, lights, objs);
}

void __fastcall hk_skyProbe(void* self, void* view) {
  if (g_slot.load(std::memory_order_relaxed) == 1 && g_skip_probe.load(std::memory_order_relaxed)) {
    g_n_probe_skip++;
    return;  // the first eye's sky cube / SH (an infinitely distant sky: eye-independent)
  }
  o_skyProbe(self, view);
}

bool on(const std::string& v) { return v == "1" || v == "true"; }

// --- rewound memory ------------------------------------------------------------------------------
// Render-side state that each renderWorld call advances and that the second eye must see exactly
// as the first eye saw it: snapshotted when the first eye starts, put back when the second starts
// (the second eye then advances it identically, so it ends as the first eye left it). (static)
//  - the post-process object's Mersenne Twister (RendererX86+0x118, +0xa8, 624 words + index at
//    +0x9c0; 0x532030 draws g_vRandomOffset, the film-grain offset, from it every render);
//  - the motion-blur frame-time ring (0x470750: globals idx 0x955618, count 0x95561c, capacity
//    0x955620, float* 0x955628) and the TAA one (0x49a8c0: 0x955850 / 0x955854 / 0x955858 /
//    0x955860): each render pushes the frame time, so the second eye's average differs.
constexpr size_t kRendererPost = 0x118, kPostRng = 0xa8, kRngSize = 0x9c4;
constexpr uintptr_t kRvaBlurRing = 0x955618, kRvaTaaRing = 0x955850;  // {int idx, count, cap; float* ring}
std::atomic<bool> g_rewind{true};
std::atomic<uint64_t> g_n_rewind{0};
struct Region {
  uint8_t* p = nullptr;
  size_t n = 0;
  void** guard = nullptr;  // restored only while *guard still equals guard_val
  void* guard_val = nullptr;
  uint8_t buf[0xa00];
};
Region g_regions[5];
int g_nregions = 0;
void save_region(void* p, size_t n, void** guard = nullptr) {
  if (!p || n > sizeof g_regions[0].buf || g_nregions >= 5 || IsBadReadPtr(p, n)) return;
  auto& r = g_regions[g_nregions++];
  r.p = static_cast<uint8_t*>(p);
  r.n = n;
  r.guard = guard;
  r.guard_val = guard ? *guard : nullptr;
  memcpy(r.buf, p, n);
}
// A frame-time ring {int idx, count, capacity; float* ring}: only the index, the count and the
// samples are put back, and only while the ring is still the one that was saved (it is allocated
// lazily on first use, which can happen during the first eye: live crash when motion blur was
// switched on with the whole header rewound).
void save_ring(uintptr_t rva) {
  auto* hdr = reinterpret_cast<uint8_t*>(game_image().at(rva));
  int cap = *reinterpret_cast<int*>(hdr + 8);
  void** ptr = reinterpret_cast<void**>(hdr + 0x10);
  if (!*ptr || cap <= 0 || cap > 256) return;
  save_region(hdr, 8, ptr);
  save_region(*ptr, sizeof(float) * size_t(cap), ptr);
}
void rewind_save(void* renderer) {
  g_nregions = 0;
  if (!g_rewind.load(std::memory_order_relaxed)) return;
  if (renderer && !IsBadReadPtr(static_cast<uint8_t*>(renderer) + kRendererPost, 8)) {
    auto* post = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(renderer) + kRendererPost);
    if (post) save_region(post + kPostRng, kRngSize);
  }
  save_ring(kRvaBlurRing);
  save_ring(kRvaTaaRing);
}
void rewind_restore() {
  for (int i = 0; i < g_nregions; i++) {
    auto& r = g_regions[i];
    if (r.guard && *r.guard != r.guard_val) continue;
    memcpy(r.p, r.buf, r.n);
  }
  if (g_nregions) g_n_rewind++;
}

}  // namespace

void pass2_register() {
  cfg_listen("stereo.", [](const std::string& id, const std::string& v) {
    if (id == "stereo.skip_shadows") g_skip_shadows = on(v);
    else if (id == "stereo.skip_probe") g_skip_probe = on(v);
    else if (id == "stereo.flare_rewind") g_flare_rewind = on(v);
    else if (id == "stereo.rewind") g_rewind = on(v);
  });
}

void pass2_start() {
  const auto& img = game_image();
  if (sig_ok(kFlareSig))
    hook_fn((void*)img.at(kRvaFlareRender), (void*)hk_flareRender, (void**)&o_flareRender, "Flare::render");
  if (sig_ok(kShadowSig))
    hook_fn((void*)img.at(kRvaShadowPass), (void*)hk_shadowPass, (void**)&o_shadowPass, "RendererX86 shadow pass");
  if (sig_ok(kProbeSig))
    hook_fn((void*)img.at(kRvaSkyProbe), (void*)hk_skyProbe, (void**)&o_skyProbe, "sky cube probe");
}

void pass2_eye(int slot, void* renderer) {
  if (slot == 0) {
    flare::g_before.clear();
    rewind_save(renderer);
  } else if (slot == 1) {
    rewind_restore();
  }
  g_slot = slot;
}

std::string pass2_status() {
  char b[320];
  snprintf(b, sizeof b,
           "skip_shadows=%d skip_probe=%d flare_rewind=%d rewind=%d shadow_skips=%llu shadow_fallbacks=%llu probe_skips=%llu "
           "flare_first=%llu flare_rewinds=%llu rewinds=%llu regions=%d",
           g_skip_shadows.load() ? 1 : 0, g_skip_probe.load() ? 1 : 0, g_flare_rewind.load() ? 1 : 0,
           g_rewind.load() ? 1 : 0, (unsigned long long)g_n_shadow_skip.load(), (unsigned long long)g_n_shadow_fallback.load(),
           (unsigned long long)g_n_probe_skip.load(), (unsigned long long)g_n_flare_first.load(),
           (unsigned long long)g_n_flare_rewind.load(), (unsigned long long)g_n_rewind.load(), g_nregions);
  return b;
}

}  // namespace tb::stereo
