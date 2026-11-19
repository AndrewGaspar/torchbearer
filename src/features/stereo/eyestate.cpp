// Per-eye temporal state (eyestate.h, docs/stereo.md "Temporal state per eye").
#include "features/stereo/eyestate.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"
#include "features/stereo/dlss.h"
#include "features/stereo/engine.h"

namespace tb::stereo {
namespace {

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

// --- hooks -------------------------------------------------------------------------------------
// The RT pool allocator: 11 arguments, all register/stack slots of 8 bytes; only the name matters.
using PoolAlloc_t = void*(__fastcall*)(void* pool, uint64_t w, uint64_t h, uint64_t a4, uint64_t fmt, uint64_t flags,
                                      const char* name, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11);
using RenderWorld_t = void(__fastcall*)(void* r, void* view, void* a3, void* a4, void* a5);
PoolAlloc_t o_poolAlloc;
RenderWorld_t o_renderWorld;

bool g_ok = false;          // hooks installed
bool g_history_ok = false;  // the TAA history strings and index matched too

const char* g_str_hist[2];  // the game's literals (by address)
const char* g_str_mask[2];
// Right-eye names. None contains a left-eye name or is contained in one (the pool matches a
// case-insensitive substring), so the two eyes can never be handed each other's targets.
constexpr const char* kRightHist[2] = {"TB R TAAHistory0", "TB R TAAHistory1"};
constexpr const char* kRightMask[2] = {"TB R TAAMask0", "TB R TAAMask1"};

// Set by eyestate_enter for the duration of one eye's renderFrame (game thread only).
thread_local int t_remap = 0;  // 1: renderWorld is rendering the right eye with its own history
thread_local bool t_counter_held = false;
thread_local uint32_t t_counter_real = 0;
std::atomic<uint64_t> g_remapped{0};

uint32_t* frame_counter() { return reinterpret_cast<uint32_t*>(game_image().at(kRvaFrameCounter)); }

void* __fastcall hk_poolAlloc(void* pool, uint64_t w, uint64_t h, uint64_t a4, uint64_t fmt, uint64_t flags,
                              const char* name, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11) {
  if (t_remap && name) {
    for (int i = 0; i < 2; i++) {
      if (name == g_str_hist[i]) {
        name = kRightHist[i];
        g_remapped++;
      } else if (name == g_str_mask[i]) {
        name = kRightMask[i];
        g_remapped++;
      }
    }
  }
  return o_poolAlloc(pool, w, h, a4, fmt, flags, name, a8, a9, a10, a11);
}

// rend::Object::storePrevious (vtable slot 6): previous world (+0xb8) <- current world (+0x88).
// renderWorld calls it for every object it drew, at its very end (0x50b46e), after the velocity
// pass has used +0xb8 (live: ~330 calls per render, all from there). With two renders per game
// update the second eye would then find prev == current and lose all object motion. While the
// first eye of an update renders (another eye follows), the previous world is kept as it was, so
// both eyes see the previous UPDATE's transform; the rest of the function (flags, bookkeeping)
// still runs.
using Void1_t = void(__fastcall*)(void*);
Void1_t o_objStorePrev;
thread_local bool t_in_eye = false;
thread_local bool t_hold_prev = false;
std::atomic<uint64_t> g_store_in{0}, g_store_out{0}, g_store_held{0};
thread_local int t_eye_index = 2;  // 0 left, 1 right, 2 not in an eye render
std::atomic<uint64_t> g_moving[3];
void __fastcall hk_objStorePrev(void* o) {
  if (!t_in_eye) {  // not an eye render (mono, menus): untouched
    g_store_out++;
    o_objStorePrev(o);
    return;
  }
  g_store_in++;
  // Objects whose transform moved since their previous was stored, i.e. that drew with object
  // motion in this render (statistics per eye, in the status).
  if (o && memcmp(&at<uint8_t>(o, kObjectPrevWorld), &at<uint8_t>(o, kObjectPrevWorld - 0x30), 0x30) != 0)
    g_moving[t_eye_index]++;
  if (!t_hold_prev || !o) {
    o_objStorePrev(o);
    return;
  }
  uint8_t keep[kObjectPrevWorldEnd - kObjectPrevWorld];
  memcpy(keep, &at<uint8_t>(o, kObjectPrevWorld), sizeof keep);
  o_objStorePrev(o);
  memcpy(&at<uint8_t>(o, kObjectPrevWorld), keep, sizeof keep);
  g_store_held++;
}

// --- probe (debug) -----------------------------------------------------------------------------
std::atomic<int> g_probe{0};
void probe(const char* where, void* r, void* view, int eye) {
  if (g_probe.load(std::memory_order_relaxed) <= 0 || !view) return;
  const double* pv2c = &at<double>(view, view::kPrevViewToClip);
  const double* pw2v = &at<double>(view, view::kPrevWorldToView);
  const double* w2v = &at<double>(view, view::kWorldToView);
  const double* v2c = &at<double>(view, view::kViewToClip);
  const double* taa = r ? reinterpret_cast<const double*>(at<uint8_t*>(r, kRendererTaa) + kTaaPrevWorldToClip) : nullptr;
  TB_LOG("eyeprobe %-6s eye=%+d storePrev in=%llu out=%llu held=%llu ctr=%u idx=%d prevW2V.t=(%.4f %.4f %.4f) "
         "W2V.t=(%.4f %.4f %.4f) prevV2C[0]=%.5f prevV2C[3][0]=%.6f V2C[0]=%.5f V2C[3][0]=%.6f taaPrev[3][0]=%.5f",
         where, eye, (unsigned long long)g_store_in.load(), (unsigned long long)g_store_out.load(),
         (unsigned long long)g_store_held.load(), *frame_counter(), r ? at<int>(r, kRendererTaaIndex) : -1, pw2v[9], pw2v[10], pw2v[11], w2v[9],
         w2v[10], w2v[11], pv2c[0], pv2c[12], v2c[0], v2c[12], taa ? taa[12] : 0.0);
}

void __fastcall hk_renderWorld(void* r, void* view, void* a3, void* a4, void* a5) {
  // renderFrame has derived the jitter from the counter we lent it; everything renderWorld does
  // (skinning buffers, per-object stamps, GPU rings) sees the engine's own count again.
  if (t_counter_held) {
    *frame_counter() = t_counter_real;
    t_counter_held = false;
  }
  probe("world", r, view, 0);
  o_renderWorld(r, view, a3, a4, a5);
}

// --- per-eye copies ----------------------------------------------------------------------------
struct Eye {
  bool have = false;
  double taa_prev[16];   // TAA object +0x60: previous unjittered world-to-clip
  double vb_prev[16];    // vector-blur object +0x60: same, for its own reprojection
  int taa_index = 0;     // RendererX86+0x2e8
  bool have_view = false;
  uint8_t view_prev[view::kPrevEnd - view::kPrevViewToClip];  // previous view-to-clip, world-to-view
};
Eye g_eye[2];
uint32_t g_jitter_n = 0;  // TAA jitter phase, advanced once per game update
std::atomic<uint64_t> g_swaps{0};

bool check(const Sig& s) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(game_image().at(s.rva));
  if (memcmp(b, s.bytes, sizeof s.bytes) == 0) return true;
  TB_LOG("stereo eyestate: signature mismatch for %s at RVA %#llx (%02x %02x %02x %02x ...)", s.name,
         (unsigned long long)s.rva, b[0], b[1], b[2], b[3]);
  return false;
}

}  // namespace

bool eyestate_start() {
  for (const auto& s : kEyeSigs)
    if (!check(s)) {
      TB_LOG("stereo eyestate: per-eye temporal state unavailable (TAA / object velocity fall back to off in sbs)");
      return false;
    }
  const auto& img = game_image();
  g_str_hist[0] = reinterpret_cast<const char*>(img.at(kRvaStrTaaHistory0));
  g_str_hist[1] = reinterpret_cast<const char*>(img.at(kRvaStrTaaHistory1));
  g_str_mask[0] = reinterpret_cast<const char*>(img.at(kRvaStrTaaMask0));
  g_str_mask[1] = reinterpret_cast<const char*>(img.at(kRvaStrTaaMask1));
  g_history_ok = strcmp(g_str_hist[0], "pTAAHistoryRT0") == 0 && strcmp(g_str_hist[1], "pTAAHistoryRT1") == 0 &&
                 strcmp(g_str_mask[0], "TAA Mask 0") == 0 && strcmp(g_str_mask[1], "TAA Mask 1") == 0;
  if (!g_history_ok) TB_LOG("stereo eyestate: TAA history names not found; history stays shared");
  bool ok = hook_fn((void*)img.at(kRvaRenderWorld), (void*)hk_renderWorld, (void**)&o_renderWorld,
                    "RendererX86::renderWorld") &&
            hook_fn((void*)img.at(kRvaRtPoolAlloc), (void*)hk_poolAlloc, (void**)&o_poolAlloc, "RT pool alloc") &&
            hook_fn((void*)img.at(kRvaObjStorePrev), (void*)hk_objStorePrev, (void**)&o_objStorePrev,
                    "rend::Object::storePrevious");
  if (!ok) {
    TB_LOG("stereo eyestate: hooks failed; per-eye temporal state unavailable");
    g_history_ok = false;
    return false;
  }
  g_ok = true;
  dlss_start(true);  // the reset signature is part of kEyeSigs
  TB_LOG("stereo eyestate: ready (per-eye TAA history %s)", g_history_ok ? "on" : "unavailable");
  return true;
}

bool eyestate_available() { return g_ok; }
bool eyestate_history_available() { return g_ok && g_history_ok; }

void eyestate_reset() {
  for (auto& e : g_eye) {
    e.have = false;
    e.have_view = false;
  }
}

void eyestate_enter(void* r, void* view, int eye, bool first_of_update, const EyeOpts& o) {
  if (!g_ok || eye == 0 || !r) return;
  Eye& e = g_eye[eye < 0 ? 0 : 1];
  uint8_t* taa = at<uint8_t*>(r, kRendererTaa);
  uint8_t* vb = at<uint8_t*>(r, kRendererVectorBlur);
  if (e.have) {
    if (taa) memcpy(taa + kTaaPrevWorldToClip, e.taa_prev, sizeof e.taa_prev);
    if (vb) memcpy(vb + kVectorBlurPrevWorldToClip, e.vb_prev, sizeof e.vb_prev);
    if (o.taa_history && g_history_ok) at<int>(r, kRendererTaaIndex) = e.taa_index;
    g_swaps++;
  }
  if (o.view_prev && e.have_view && view) memcpy(&at<uint8_t>(view, view::kPrevViewToClip), e.view_prev, sizeof e.view_prev);
  t_remap = (o.taa_history && g_history_ok && eye > 0) ? 1 : 0;
  if (o.same_jitter) {
    if (first_of_update) g_jitter_n++;
    t_counter_real = *frame_counter();
    t_counter_held = true;
    *frame_counter() = g_jitter_n;
  }
  t_hold_prev = o.view_prev && o.another_eye_follows;
  t_in_eye = true;
  t_eye_index = eye < 0 ? 0 : 1;
  dlss_eye_enter(eye);
  probe("enter", r, view, eye);
}

void eyestate_leave(void* r, void* view, int eye) {
  if (!g_ok || eye == 0 || !r) return;
  if (t_counter_held) {  // renderWorld did not run: give the counter back anyway
    *frame_counter() = t_counter_real;
    t_counter_held = false;
  }
  t_remap = 0;
  t_in_eye = false;
  t_eye_index = 2;
  t_hold_prev = false;
  dlss_eye_leave(eye);
  probe("leave", r, view, eye);
  if (g_probe.load(std::memory_order_relaxed) > 0) g_probe--;
  Eye& e = g_eye[eye < 0 ? 0 : 1];
  uint8_t* taa = at<uint8_t*>(r, kRendererTaa);
  uint8_t* vb = at<uint8_t*>(r, kRendererVectorBlur);
  if (taa) memcpy(e.taa_prev, taa + kTaaPrevWorldToClip, sizeof e.taa_prev);
  if (vb) memcpy(e.vb_prev, vb + kVectorBlurPrevWorldToClip, sizeof e.vb_prev);
  e.taa_index = at<int>(r, kRendererTaaIndex);
  e.have = true;
  if (view) {
    memcpy(e.view_prev, &at<uint8_t>(view, view::kPrevViewToClip), sizeof e.view_prev);
    e.have_view = true;
  }
}

void eyestate_probe(int n) { g_probe = n; }

// DLSS: one feature per eye while both eyes of an update render (dlss.cpp), else the reset
// fallback.
void eyestate_update(bool dual) {
  if (!g_ok) return;
  dlss_update(dual);
}

std::string eyestate_status_json() {
  char b[1024];
  snprintf(b, sizeof b,
           "{\"available\":%s,\"history\":%s,\"swaps\":%llu,\"remapped\":%llu,\"jitter_n\":%u,"
           "\"obj_prev_stores\":%llu,\"obj_prev_held\":%llu,\"obj_moving\":[%llu,%llu,%llu],"
           "\"dlss\":%s}",
           g_ok ? "true" : "false", g_history_ok ? "true" : "false", (unsigned long long)g_swaps.load(),
           (unsigned long long)g_remapped.load(), g_jitter_n,
           (unsigned long long)(g_store_in.load() + g_store_out.load()), (unsigned long long)g_store_held.load(),
           (unsigned long long)g_moving[0].load(), (unsigned long long)g_moving[1].load(),
           (unsigned long long)g_moving[2].load(), dlss_status_json().c_str());
  return b;
}

}  // namespace tb::stereo
