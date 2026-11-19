// One DLSS feature per eye in sbs (dlss.h, docs/stereo.md section 5 "DLSS").
//
// Static (Ghidra), rend::DeviceD3D12PC (global kRvaDevice, vtable 0x790f58):
//   +0x38a0 NGX parameter block (shared: every evaluate sets all its inputs, 0x4a5800)
//   +0x38a8 DLSS feature handle             +0x38b0 old handle, released by slot 56 (0x4a6cd0)
//   +0x38b8 u8 NGX initialised              +0x38c0..+0x38cc u32 render w/h, output w/h it was
//   +0x38d8 u32 DLSS mode it was created for         created for (the "key")
// Slot 45 (0x4a6660), called at the start of every renderWorld (0x50d7a0): mode 0 -> handle to
// +0x38b0; mode or output size differs from the key -> optimal settings (0x4a73e0, sets the key
// and the render-size globals) and CreateFeature (0x4a7080) into +0x38a8, old one to +0x38b0;
// render size differs -> re-create with the key's size. The feature handle is the only per-feature
// state: the evaluate (slot 46, 0x4a67b0) passes every resource, jitter, MV scale, sharpness and
// exposure per call.
//
// So the second eye's feature is the device's own slot (handle + key + mode) swapped per eye: the
// right eye's slot is put into the device before its renderFrame and the engine's own slot 45 is
// run once on it (creating or re-creating it for the current mode / size exactly as the engine
// would, so the call inside renderWorld then finds nothing to do); after the render the left
// eye's slot goes back. An old right-eye handle is never handed to the engine's +0x38b0: it is
// released once a fence signalled on the game's queue after its last use has completed. (Not the
// engine's GPU idle 0x4f7be0 between the eyes: it submits the thread's open command list, and the
// next draw recorded into it crashed, live r1: 0x4f7640 with list index -1.)
#include "features/stereo/dlss.h"

#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <vector>
#include <cstdio>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "features/stereo/engine.h"

namespace tb::stereo {
namespace {

constexpr uintptr_t kRvaVtblDevice = 0x790f58;  // rend::DeviceD3D12PC (RTTI)
constexpr uintptr_t kRvaDlssUpdate = 0x4a6660;  // device vtable slot 45
constexpr size_t kSlotDlssUpdate = 45;
constexpr uintptr_t kRvaNgxRelease = 0x68ea80;  // NVSDK_NGX_D3D12_ReleaseFeature(handle)
constexpr size_t kOffHandle = 0x38a8, kOffOld = 0x38b0, kOffInit = 0x38b8, kOffKey = 0x38c0, kOffMode = 0x38d8;

struct DSig {
  uintptr_t rva;
  const char* name;
  uint8_t bytes[16];
};
constexpr DSig kDlssSigs[] = {
    // push rbx; sub rsp,0x20; mov eax,[rip -> 0xd75620]; mov rbx,rcx; test eax,eax
    {kRvaDlssUpdate, "DLSS update (device slot 45)",
     {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x05, 0xb4, 0xef, 0x8c, 0x00, 0x48, 0x8b, 0xd9, 0x85}},
    // mov [rsp+8],rbx; push rdi; sub rsp,0x20; mov rbx,[rip -> 0xe23d28 ReleaseFeature]
    {kRvaNgxRelease, "NGX ReleaseFeature",
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x1d, 0x97, 0x52, 0x79}},
    // CreateFeature (0x4a7080): mov rax,rsp; push rsi; sub rsp,0x170; cmp byte [rcx+0x38b8],0
    {0x4a7080, "DLSS create",
     {0x48, 0x8b, 0xc4, 0x56, 0x48, 0x81, 0xec, 0x70, 0x01, 0x00, 0x00, 0x80, 0xb9, 0xb8, 0x38, 0x00}},
};

using Char1_t = char(__fastcall*)(void*);
using Release_t = uint32_t(__fastcall*)(void*);

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

struct Slot {
  void* handle = nullptr;
  uint32_t key[4] = {};  // render w, h, output w, h
  uint32_t mode = 0;
};
Slot read_slot(void* d) {
  Slot s;
  s.handle = at<void*>(d, kOffHandle);
  memcpy(s.key, &at<uint8_t>(d, kOffKey), sizeof s.key);
  s.mode = at<uint32_t>(d, kOffMode);
  return s;
}
void write_slot(void* d, const Slot& s) {
  at<void*>(d, kOffHandle) = s.handle;
  memcpy(&at<uint8_t>(d, kOffKey), s.key, sizeof s.key);
  at<uint32_t>(d, kOffMode) = s.mode;
}
bool same_key(const Slot& a, const Slot& b) { return a.mode == b.mode && memcmp(a.key, b.key, sizeof a.key) == 0; }

bool g_ok = false;        // per-eye DLSS available (signatures)
bool g_reset_ok = false;  // the reset patch is available
std::atomic<bool> g_enabled{true};  // stereo.dlss_per_eye

// Game thread only.
void* g_dev = nullptr;  // the device the right eye's feature belongs to
Slot g_r;               // the right eye's slot while not swapped in
Slot g_l;               // the left eye's (engine's) slot while the right eye's is swapped in
bool g_swapped = false;
bool g_dual = false;
bool g_failed = false;  // creation failed for g_failed_key: reset fallback until the key changes
Slot g_failed_key;
bool g_reset = false;   // the evaluate's Reset immediate is patched to 1
std::atomic<uint64_t> g_creates{0}, g_releases{0}, g_swaps{0}, g_failures{0};

uint32_t dlss_mode() { return *reinterpret_cast<uint32_t*>(game_image().at(kRvaDlssMode)); }

void* device() {
  const auto& img = game_image();
  void* d = *reinterpret_cast<void**>(img.at(kRvaDevice));
  if (!d) return nullptr;
  void** vt = *reinterpret_cast<void***>(d);
  if (reinterpret_cast<uintptr_t>(vt) != img.at(kRvaVtblDevice) ||
      reinterpret_cast<uintptr_t>(vt[kSlotDlssUpdate]) != img.at(kRvaDlssUpdate))
    return nullptr;
  return d;
}

void set_reset(bool want) {
  if (!g_reset_ok || want == g_reset) return;
  auto* imm = reinterpret_cast<uint32_t*>(game_image().at(kRvaDlssResetImm));
  DWORD old;
  if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) return;
  *imm = want ? 1 : 0;
  VirtualProtect(imm, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), imm, 4);
  g_reset = want;
  TB_LOG("stereo dlss: reset every frame %s (DLSS mode %u)", want ? "on (one DLSS history for two eyes)" : "off",
         dlss_mode());
}

// Deferred NGX release: a fence on the game's queue (the one it presents on), signalled when the
// release is requested, i.e. after every submitted use of the handle (its last evaluate was
// recorded in an earlier, already submitted frame). Without a queue, after kFramesFallback presents.
constexpr uint64_t kFramesFallback = 8;
IDXGISwapChain* g_sc = nullptr;  // last presented (game thread)
ID3D12CommandQueue* g_queue = nullptr;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fence_value = 0;
struct Pending {
  void* handle;
  uint64_t fence;  // 0: use `frame`
  uint64_t frame;
};
std::vector<Pending> g_pending;

void release_later(void* h, const char* why) {
  if (!h) return;
  auto* q = g_sc ? static_cast<ID3D12CommandQueue*>(present_queue(g_sc)) : nullptr;
  if (q != g_queue) {
    if (g_fence) g_fence->Release();
    g_fence = nullptr;
    g_queue = q;
    ID3D12Device* dev = nullptr;
    if (q && SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) {
      if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence))) g_fence = nullptr;
      dev->Release();
    }
    g_fence_value = 0;
  }
  Pending p{h, 0, present_frame() + kFramesFallback};
  if (g_fence && SUCCEEDED(g_queue->Signal(g_fence, ++g_fence_value))) p.fence = g_fence_value;
  g_pending.push_back(p);
  TB_LOG("stereo dlss: right-eye feature %p to be released (%s; fence %llu)", h, why, (unsigned long long)p.fence);
}

void release_done() {
  for (size_t i = 0; i < g_pending.size();) {
    const Pending& p = g_pending[i];
    bool done = p.fence ? (g_fence && g_fence->GetCompletedValue() >= p.fence) : present_frame() >= p.frame;
    if (!done) {
      i++;
      continue;
    }
    uint32_t rc = reinterpret_cast<Release_t>(game_image().at(kRvaNgxRelease))(p.handle);
    g_releases++;
    TB_LOG("stereo dlss: right-eye feature %p released, NGX %#x", p.handle, rc);
    g_pending.erase(g_pending.begin() + i);
  }
}

void release_right(const char* why) {
  if (!g_r.handle) return;
  void* d = device();
  if (d && d == g_dev) release_later(g_r.handle, why);
  else TB_LOG("stereo dlss: right-eye feature %p dropped (device changed)", g_r.handle);
  g_r = Slot{};
}

bool active() { return g_ok && g_dual && g_enabled.load(std::memory_order_relaxed) && !g_failed; }

}  // namespace

bool dlss_start(bool reset_ok) {
  g_reset_ok = reset_ok;
  for (const auto& s : kDlssSigs) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(game_image().at(s.rva));
    if (memcmp(b, s.bytes, sizeof s.bytes) != 0) {
      TB_LOG("stereo dlss: signature mismatch for %s at RVA %#llx; DLSS in sbs falls back to a reset every frame",
             s.name, (unsigned long long)s.rva);
      return false;
    }
  }
  auto set = [](const std::string& v) { g_enabled = !(v == "0" || v == "false"); };
  set(cfg_str("stereo.dlss_per_eye", "1"));
  cfg_listen("stereo.dlss_per_eye", [set](const std::string&, const std::string& v) { set(v); });
  present_on([](IDXGISwapChain* sc, uint64_t) { g_sc = sc; });
  g_ok = true;
  TB_LOG("stereo dlss: per-eye DLSS ready");
  return true;
}

void dlss_update(bool dual) {
  if (!g_reset_ok && !g_ok) return;
  g_dual = dual;
  uint32_t mode = dlss_mode();
  void* d = g_ok ? device() : nullptr;
  if (g_ok) {
    if (d != g_dev) {
      if (g_r.handle) release_right("device changed");
      if (g_dev && !g_pending.empty()) {
        TB_LOG("stereo dlss: device changed; %zu pending right-eye features dropped", g_pending.size());
        g_pending.clear();
      }
      g_dev = d;
    }
    release_done();
    bool keep = dual && mode != 0 && d && at<uint8_t>(d, kOffInit) && g_enabled.load(std::memory_order_relaxed);
    if (!keep && g_r.handle) release_right(!dual ? "stereo off" : mode == 0 ? "DLSS off" : "per-eye DLSS off");
    // A failed creation is retried once the engine's own feature has a different key (mode or
    // size change), or when sbs starts again.
    if (g_failed && (!dual || (d && !same_key(read_slot(d), g_failed_key)))) g_failed = false;
  }
  set_reset(dual && mode != 0 && !active());
}

void dlss_eye_enter(int eye) {
  if (eye <= 0 || g_swapped || !active() || dlss_mode() == 0) return;
  void* d = device();
  if (!d || d != g_dev || !at<uint8_t>(d, kOffInit)) return;
  Slot l = read_slot(d);
  if (!l.handle) return;  // the engine has no feature (yet): nothing to pair with
  write_slot(d, g_r);
  void* pending = at<void*>(d, kOffOld);  // the engine's own old handle (normally already released)
  at<void*>(d, kOffOld) = nullptr;
  char changed = reinterpret_cast<Char1_t>(game_image().at(kRvaDlssUpdate))(d);
  void* old = at<void*>(d, kOffOld);
  at<void*>(d, kOffOld) = pending;
  Slot r = read_slot(d);
  if (old && r.handle == old) r.handle = nullptr;  // CreateFeature failed and left the old handle
  if (old) release_later(old, "mode or size changed");
  if (changed && r.handle) {
    g_creates++;
    TB_LOG("stereo dlss: right-eye feature %p created (mode %u, render %ux%u, output %ux%u; left %p)", r.handle,
           r.mode, r.key[0], r.key[1], r.key[2], r.key[3], l.handle);
  }
  if (!r.handle) {
    // No second feature: this eye shares the engine's, and every evaluate resets from now on.
    write_slot(d, l);
    g_r = Slot{};
    g_failed = true;
    g_failed_key = l;
    g_failures++;
    set_reset(true);
    TB_LOG("stereo dlss: right-eye feature creation failed (mode %u); reset every frame instead", dlss_mode());
    return;
  }
  g_r = r;
  g_l = l;
  g_swapped = true;
  g_swaps++;
}

void dlss_eye_leave(int eye) {
  if (eye <= 0 || !g_swapped) return;
  g_swapped = false;
  void* d = device();
  if (!d) return;
  // Normally unchanged; if the engine re-created the feature inside this render (the mode changed
  // in between), the new handle is the right eye's and the old one was released by the engine.
  g_r = read_slot(d);
  write_slot(d, g_l);
}

std::string dlss_status_json() {
  void* d = g_ok ? device() : nullptr;
  char b[512];
  snprintf(b, sizeof b,
           "{\"per_eye\":%s,\"enabled\":%s,\"mode\":%u,\"left\":\"%p\",\"right\":\"%p\",\"right_key\":[%u,%u,%u,%u],"
           "\"creates\":%llu,\"releases\":%llu,\"pending\":%zu,\"swaps\":%llu,\"failures\":%llu,\"failed\":%s,\"reset\":%s}",
           g_ok ? "true" : "false", g_enabled.load() ? "true" : "false", dlss_mode(),
           d && !g_swapped ? at<void*>(d, kOffHandle) : g_l.handle, g_r.handle, g_r.key[0], g_r.key[1], g_r.key[2],
           g_r.key[3], (unsigned long long)g_creates.load(), (unsigned long long)g_releases.load(), g_pending.size(),
           (unsigned long long)g_swaps.load(), (unsigned long long)g_failures.load(), g_failed ? "true" : "false",
           g_reset ? "true" : "false");
  return b;
}

}  // namespace tb::stereo
