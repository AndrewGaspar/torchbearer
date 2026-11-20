// Per-eye SSAO and volumetric-light history (temporal.h, docs/stereo.md section 5).
//
// Static (Ghidra), both effects follow the same pattern:
//   rend::SSAO (RendererX86+0x130, render 0x536630 from the main pass at 0x5070ef):
//     +0x10 Texture2D* "Temporal buffer" (created by vtable slot 1, 0x5363c0: device createRT, half
//           or full resolution by SSAO quality 0x955674), +0x60 u8 reset (history ignored),
//     +0x64 float[4][3] the main camera's view-to-world at its last render (position at +0x88).
//     Per render: AO -> blur -> if "SSAO:Temporal Antialiasing": blend weight from the camera's
//     linear/angular speed since +0x64 (min 0.3 .. max 0.7, 0 on a jump or reset), resolve
//     (AO, +0x10) into a pool RT, copy it into +0x10, store the camera.
//   rend::VolumetricLight (RendererX86+0x170, 0x4e37a0 from renderWorld at 0x50a3f2; temporal
//     part 0x4e7120 when "Volumetric Light:Temporal Blur"):
//     +0x20 Texture2D* "VolumetricLight Temporal buffer" (created in 0x4e3020, half resolution,
//           format 0xb or 0x16 by device+0x198 == 0xb), +0x29 u8 reset,
//     +0x60 float[4][3] the camera at its last render (position at +0x84). Weight 0.35 .. 0.65.
// With two eyes per update both read the other eye's result, and the other eye's camera, 65 mm
// away, reads as 3.9 m/s of motion: the weight drops to its minimum and the eyes blend.
//
// What this does: the engine's buffer, camera and reset flag stay the left eye's (and mono's);
// around the right eye's renderFrame they are swapped for a second set, whose buffer is created
// with the engine's own device call (device vtable +0xc0, RVA 0x4f5410) and the same arguments
// the engine uses, re-created when the engine's buffer changes size (graphics settings).
#include "features/stereo/temporal.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "features/graphics.h"
#include "core/module.h"
#include "core/present.h"
#include "features/stereo/engine.h"

namespace tb::stereo {
namespace {

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

// Device (rend::DeviceD3D12PC, global kRvaDevice) createRenderTarget, vtable +0xc0 (static):
// Texture2D* (dev, w, h, mips, format, a6, a7, name, u8, u8, int, float, const float clear[4]).
constexpr size_t kDevCreateRtSlot = 0xc0 / 8;
constexpr uintptr_t kRvaDevCreateRt = 0x4f5410;
constexpr uintptr_t kRvaVtblDevice = 0x790f58;
// Device vtable +0x358 (0x4a7040): bool (dev, 1) = *(int*)(dev+0x198) == 0xb; the volumetric
// buffers use format 0xb when true, else 0x16 (0x4e3020).
constexpr size_t kDevFmtQuerySlot = 0x358 / 8;
constexpr uintptr_t kRvaDevFmtQuery = 0x4a7040;
constexpr uintptr_t kRvaFree = 0x3b9790;  // the engine's operator delete (after the virtual dtor)
// Texture2D: +0x00 vtable (slot 0 = destructor(this, flags)), +0x10 width, +0x14 height.
constexpr size_t kTexW = 0x10, kTexH = 0x14;
using CreateRt_t = void*(__fastcall*)(void*, int, int, int, int, int, int, const char*, uint8_t, uint8_t, int, float,
                                      const void*);
using FmtQuery_t = char(__fastcall*)(void*, char);
using Dtor_t = void(__fastcall*)(void*, int);
using Free_t = void(__fastcall*)(void*);

struct TSig {
  uintptr_t rva;
  const char* name;
  uint8_t n;
  uint8_t bytes[16];
};
constexpr TSig kTemporalSigs[] = {
    {kRvaDevCreateRt, "device createRT", 16,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48}},
    // test dl,dl; mov eax,0x198; mov r8d,0x1d0; cmove ...
    {kRvaDevFmtQuery, "device format query", 16,
     {0x84, 0xd2, 0xb8, 0x98, 0x01, 0x00, 0x00, 0x41, 0xb8, 0xd0, 0x01, 0x00, 0x00, 0x41, 0x0f, 0x44}},
    {kRvaFree, "operator delete", 16,
     {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x33, 0xc0, 0x48, 0x8b, 0xd9, 0x38, 0x05, 0xf7, 0x27, 0x95}},
    // SSAO: mov rcx,[rsi+0x130]; call 0x536630 (main pass)
    {0x5070ef, "SSAO render call", 12, {0x48, 0x8b, 0x8e, 0x30, 0x01, 0x00, 0x00, 0xe8, 0x35, 0xf5, 0x02, 0x00}},
    // SSAO create: mov rdi,[rbx+0x10]; mov [rbx+0x10],rax (Temporal buffer)
    {0x536469, "SSAO temporal buffer", 8, {0x48, 0x8b, 0x7b, 0x10, 0x48, 0x89, 0x43, 0x10}},
    // SSAO render: subss xmm1,[rsi+0x88] (camera position x); cmp byte [rsi+0x60],0; mov byte [rsi+0x60],0
    {0x536f10, "SSAO camera", 8, {0xf3, 0x0f, 0x5c, 0x8e, 0x88, 0x00, 0x00, 0x00}},
    {0x537100, "SSAO reset test", 4, {0x80, 0x7e, 0x60, 0x00}},
    {0x5371d8, "SSAO reset clear", 4, {0xc6, 0x46, 0x60, 0x00}},
    // VolumetricLight: mov rcx,[rsi+0x170]; call 0x4e37a0 (renderWorld)
    {0x50a3f2, "VolumetricLight call", 12, {0x48, 0x8b, 0x8e, 0x70, 0x01, 0x00, 0x00, 0xe8, 0xa2, 0x93, 0xfd, 0xff}},
    // temporal blur 0x4e7120: subss xmm1,[rcx+0x84]; cmp byte [r14+0x29],0; mov rdi,[r14+0x20];
    // mov byte [r14+0x29],0
    {0x4e7259, "VolumetricLight camera", 8, {0xf3, 0x0f, 0x5c, 0x89, 0x84, 0x00, 0x00, 0x00}},
    {0x4e747f, "VolumetricLight reset test", 5, {0x41, 0x80, 0x7e, 0x29, 0x00}},
    {0x4e749c, "VolumetricLight temporal buffer", 4, {0x49, 0x8b, 0x7e, 0x20}},
    {0x4e7719, "VolumetricLight reset clear", 5, {0x41, 0xc6, 0x46, 0x29, 0x00}},
    // ShadeEffect: mov rdx,[rsi+0x248]; mov rcx,[rsi+0x1b0]; call 0x4b6950 (renderWorld)
    {0x50a405, "ShadeEffect call", 15,
     {0x48, 0x8b, 0x96, 0x48, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x8e, 0xb0, 0x01, 0x00, 0x00, 0xe8}},
    // 0x4b6950: movss xmm0,[rcx+0x78] / movss [rcx+0x78],xmm1 (fade); mov rdx,[rcx+0x20] (buffer 1
    // array); mov rax,[rdi+0x80] (clear flags); mov rdx,[rdi+0x40] (buffer 2 array)
    {0x4b6987, "ShadeEffect fade read", 5, {0xf3, 0x0f, 0x10, 0x41, 0x78}},
    {0x4b69bd, "ShadeEffect fade write", 5, {0xf3, 0x0f, 0x11, 0x49, 0x78}},
    {0x4b69ea, "ShadeEffect buffer 1", 4, {0x48, 0x8b, 0x51, 0x20}},
    {0x4b6a0c, "ShadeEffect clear flags", 7, {0x48, 0x8b, 0x87, 0x80, 0x00, 0x00, 0x00}},
    {0x4b6bb3, "ShadeEffect buffer 2", 4, {0x48, 0x8b, 0x57, 0x40}},
};

// One effect's per-eye state.
struct Effect {
  const char* name;
  size_t renderer_off;  // RendererX86 field holding the effect object
  size_t rt_off, reset_off, cam_off;
  size_t pos_off;            // camera position (3 floats) inside the object
  const char* rt_name;       // our buffer's name (debug)
  uintptr_t clear_rva;       // the clear colour the engine passes (16 bytes)
  bool vol;                  // volumetric: format from the device query
  uintptr_t enable_rva;      // u8: the effect (and its temporal part) runs at all
  // the right eye's set
  void* rt = nullptr;
  int rt_w = 0, rt_h = 0;
  uint8_t reset = 1;
  uint8_t cam[0x30] = {};
  bool have_cam = false;
  // the engine's (left eye's) set while the right eye renders
  void* l_rt = nullptr;
  uint8_t l_reset = 0;
  uint8_t l_cam[0x30] = {};
  bool swapped = false;
  // measurement: camera jump the effect sees at the start of each eye's render (mm), sum / n
  double jump_mm[2] = {0, 0};
  uint64_t jump_n[2] = {0, 0};
  uint64_t creates = 0, swaps = 0;
};
Effect g_fx[2] = {
    // "Render SSAO" (0xd755e0, read at 0x5070e6): every graphics preset writes 0 (0x242a20; the game
    // renders HBAO instead, RendererX86+0x138, no history), so this one only runs when forced on.
    {"ssao", 0x130, 0x10, 0x60, 0x64, 0x88, "TB R SSAO Temporal buffer", 0xe10a08, false, 0xd755e0},
    // "Volumetric Light:Temporal Blur" tweakable value (0xdbb3b0, read in 0x4e37a0).
    {"volumetric", 0x170, 0x20, 0x29, 0x60, 0x84, "TB R VolumetricLight Temporal buffer", 0xdbb3b8, true, 0xdbb3b0},
};

// rend::ShadeEffect (RendererX86+0x1b0; render 0x4b6950 and its velocity pass 0x4b6720, both from
// renderWorld at 0x50a405, while "Render Shade Effect" 0x955662) draws the Taken darkness (the
// birds' and enemies' smoky shade) into ring slot ctr % N (N = 0x9554ac) of two half-resolution
// buffer arrays: buffer 1 (+0x20 data, +0x28 count) accumulates the shade mask (never cleared after
// its first use, flag array +0x80/+0x88), buffer 2 (+0x40/+0x48) gets the displaced, trailing result
// ("Shade Effect:Trail Factor" 0.85) that the composite reads (g_sShadeEffectBuffer); +0x78 is a
// fade stepped every render by +1/6 or -1/15. Both eyes of an update share the counter, so the
// second eye drew into the slot the first eye had just drawn: one more trail step, and the first
// eye's view of the shade in the second eye's buffer (the bird flock differing between the eyes).
// The right eye gets its own arrays (2N buffers created like 0x4b5f20 does, clear flags set) and
// its own fade, swapped in like the buffers above.
constexpr size_t kShadeObj = 0x1b0, kShadeBuf1 = 0x20, kShadeBuf2 = 0x40, kShadeFlags = 0x80, kShadeFade = 0x78;
constexpr uintptr_t kRvaShadeOn = 0x955662, kRvaShadeClear = 0xda8bc8;
constexpr int kShadeMaxN = 8;
struct Shade {
  void* buf1[kShadeMaxN] = {};
  void* buf2[kShadeMaxN] = {};
  uint8_t flags[kShadeMaxN] = {};
  int n = 0, w = 0, h = 0;
  float fade = 0;
  bool have = false;
  // the engine's (left eye's) while the right eye renders
  void* l_buf1 = nullptr;
  void* l_buf2 = nullptr;
  void* l_flags = nullptr;
  float l_fade = 0;
  bool swapped = false;
  uint64_t creates = 0, swaps = 0, resyncs = 0;
} g_shade;
bool g_shade_resync = false;  // start both eyes' shade buffers over before the next left eye

bool g_ok = false;
std::atomic<bool> g_on{true};  // stereo.temporal_per_eye
struct Grave {
  void* rt;
  uint64_t frame;
};
std::vector<Grave> g_graves;  // replaced right-eye buffers, destroyed a few presents later

void* device() {
  void* dev = *reinterpret_cast<void**>(game_image().at(kRvaDevice));
  if (!dev || *reinterpret_cast<uintptr_t*>(dev) != game_image().at(kRvaVtblDevice)) return nullptr;
  return dev;
}

void destroy(void* rt) {
  if (!rt) return;
  reinterpret_cast<Dtor_t>((*reinterpret_cast<void***>(rt))[0])(rt, 0);
  reinterpret_cast<Free_t>(game_image().at(kRvaFree))(rt);
}

void bury_due() {
  uint64_t now = present_frame();
  for (size_t i = 0; i < g_graves.size();) {
    if (now - g_graves[i].frame > 8) {
      destroy(g_graves[i].rt);
      g_graves.erase(g_graves.begin() + i);
    } else {
      i++;
    }
  }
}

// The right eye's buffer, matching the engine's in size (created like the engine does).
bool ensure_rt(Effect& f, void* engine_rt) {
  int w = at<int>(engine_rt, kTexW), h = at<int>(engine_rt, kTexH);
  if (f.rt && f.rt_w == w && f.rt_h == h) return true;
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
  void* dev = device();
  if (!dev) return false;
  void** vt = *reinterpret_cast<void***>(dev);
  if (reinterpret_cast<uintptr_t>(vt[kDevCreateRtSlot]) != game_image().at(kRvaDevCreateRt)) return false;
  int fmt = 1;  // SSAO (0x5363c0)
  if (f.vol) {
    if (reinterpret_cast<uintptr_t>(vt[kDevFmtQuerySlot]) != game_image().at(kRvaDevFmtQuery)) return false;
    fmt = reinterpret_cast<FmtQuery_t>(vt[kDevFmtQuerySlot])(dev, 1) ? 0xb : 0x16;
  }
  float clear[4];
  memcpy(clear, reinterpret_cast<const void*>(game_image().at(f.clear_rva)), sizeof clear);
  void* rt = reinterpret_cast<CreateRt_t>(vt[kDevCreateRtSlot])(dev, w, h, 1, fmt, 1, 1, f.rt_name, 0, 0, 0, 1.0f, clear);
  if (!rt) return false;
  if (f.rt) g_graves.push_back({f.rt, present_frame()});
  f.rt = rt;
  f.rt_w = w;
  f.rt_h = h;
  f.reset = 1;  // no history yet
  f.have_cam = false;
  f.creates++;
  TB_LOG("stereo temporal: right-eye %s buffer %dx%d format %#x created (%p)", f.name, w, h, fmt, rt);
  return true;
}

void* create_rt(int w, int h, int fmt, const char* name, uintptr_t clear_rva) {
  void* dev = device();
  if (!dev) return nullptr;
  void** vt = *reinterpret_cast<void***>(dev);
  if (reinterpret_cast<uintptr_t>(vt[kDevCreateRtSlot]) != game_image().at(kRvaDevCreateRt)) return nullptr;
  float clear[4];
  memcpy(clear, reinterpret_cast<const void*>(game_image().at(clear_rva)), sizeof clear);
  return reinterpret_cast<CreateRt_t>(vt[kDevCreateRtSlot])(dev, w, h, 1, fmt, 1, 1, name, 0, 0, 0, 1.0f, clear);
}

// The right eye's shade buffers, matching the engine's ring (count and size).
bool shade_ensure(void* obj) {
  size_t n = at<size_t>(obj, kShadeBuf1 + 8);
  if (n < 1 || n > kShadeMaxN || at<size_t>(obj, kShadeBuf2 + 8) != n || at<size_t>(obj, kShadeFlags + 8) != n) return false;
  void** e1 = at<void**>(obj, kShadeBuf1);
  if (!e1 || !e1[0] || !at<void**>(obj, kShadeBuf2) || !at<uint8_t*>(obj, kShadeFlags)) return false;
  int w = at<int>(e1[0], kTexW), h = at<int>(e1[0], kTexH);
  Shade& s = g_shade;
  if (s.have && s.n == int(n) && s.w == w && s.h == h) return true;
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
  Shade fresh;
  for (size_t i = 0; i < n; i++) {
    // as 0x4b5f20: (w/2, h/2, 1 mip, format 1, ..., clear colour 0xda8bc8), named
    fresh.buf1[i] = create_rt(w, h, 1, "TB R Shade effect buffer 1", kRvaShadeClear);
    fresh.buf2[i] = create_rt(w, h, 1, "TB R Shade effect buffer 2", kRvaShadeClear);
    fresh.flags[i] = 1;  // clear buffer 1 on first use, as the engine does
    if (!fresh.buf1[i] || !fresh.buf2[i]) {
      for (size_t j = 0; j <= i; j++) {
        if (fresh.buf1[j]) g_graves.push_back({fresh.buf1[j], present_frame()});
        if (fresh.buf2[j]) g_graves.push_back({fresh.buf2[j], present_frame()});
      }
      return false;
    }
  }
  for (int i = 0; i < s.n; i++) {
    g_graves.push_back({s.buf1[i], present_frame()});
    g_graves.push_back({s.buf2[i], present_frame()});
  }
  fresh.n = int(n);
  fresh.w = w;
  fresh.h = h;
  fresh.fade = at<float>(obj, kShadeFade);
  fresh.have = true;
  fresh.creates = s.creates + 1;
  fresh.swaps = s.swaps;
  fresh.resyncs = s.resyncs;
  s = fresh;
  g_shade_resync = true;
  TB_LOG("stereo temporal: right-eye shade effect buffers %d x 2 at %dx%d created", s.n, w, h);
  return true;
}

bool shade_on() { return *reinterpret_cast<const uint8_t*>(game_image().at(kRvaShadeOn)) != 0; }

// Before the first (left) eye: make sure the right eye's set exists, and when it is new (or after a
// reset) start BOTH eyes' buffers over in this same update: buffer 1 is only cleared on a slot's
// first use (flag), and live the right eye's fresh buffers left a ~1/255 offset wherever the shade
// is thin against the left eye's long-accumulated ones.
void shade_prepare(void* r) {
  void* obj = at<void*>(r, kShadeObj);
  if (!obj || !shade_on() || !shade_ensure(obj) || !g_shade_resync) return;
  uint8_t* ef = at<uint8_t*>(obj, kShadeFlags);
  for (int i = 0; i < g_shade.n; i++) {
    ef[i] = 1;
    g_shade.flags[i] = 1;
  }
  g_shade.fade = at<float>(obj, kShadeFade);
  g_shade_resync = false;
  g_shade.resyncs++;
}

void shade_enter(void* r) {
  Shade& s = g_shade;
  s.swapped = false;
  void* obj = at<void*>(r, kShadeObj);
  if (!obj || !shade_on() || !s.have || g_shade_resync) return;
  void** e1 = at<void**>(obj, kShadeBuf1);
  if (at<size_t>(obj, kShadeBuf1 + 8) != size_t(s.n) || !e1 || !e1[0] || at<int>(e1[0], kTexW) != s.w ||
      at<int>(e1[0], kTexH) != s.h)
    return;  // the engine's ring changed: the next left eye rebuilds ours
  s.l_buf1 = at<void*>(obj, kShadeBuf1);
  s.l_buf2 = at<void*>(obj, kShadeBuf2);
  s.l_flags = at<void*>(obj, kShadeFlags);
  s.l_fade = at<float>(obj, kShadeFade);
  at<void*>(obj, kShadeBuf1) = s.buf1;
  at<void*>(obj, kShadeBuf2) = s.buf2;
  at<void*>(obj, kShadeFlags) = s.flags;
  at<float>(obj, kShadeFade) = s.fade;
  s.swapped = true;
  s.swaps++;
}

void shade_leave(void* r) {
  Shade& s = g_shade;
  if (!s.swapped) return;
  s.swapped = false;
  void* obj = at<void*>(r, kShadeObj);
  if (!obj) return;
  if (at<void*>(obj, kShadeBuf1) != s.buf1) {  // the engine rebuilt its ring meanwhile: keep it
    TB_LOG("stereo temporal: engine rebuilt the shade ring during the right eye's render");
    s = Shade{};
    return;
  }
  s.fade = at<float>(obj, kShadeFade);
  at<void*>(obj, kShadeBuf1) = s.l_buf1;
  at<void*>(obj, kShadeBuf2) = s.l_buf2;
  at<void*>(obj, kShadeFlags) = s.l_flags;
  at<float>(obj, kShadeFade) = s.l_fade;
}

void measure(Effect& f, void* obj, void* view, int idx) {
  if (!view) return;
  const float* p = &at<float>(obj, f.pos_off);
  const double* v = &at<double>(view, view::kViewToWorld + 9 * sizeof(double));
  double d = std::sqrt((v[0] - p[0]) * (v[0] - p[0]) + (v[1] - p[1]) * (v[1] - p[1]) + (v[2] - p[2]) * (v[2] - p[2]));
  if (d < 10.0) {  // ignore teleports / first frames
    f.jump_mm[idx] += d * 1000.0;
    f.jump_n[idx]++;
  }
}

}  // namespace

void temporal_register() {
  cfg_listen("stereo.", [](const std::string& id, const std::string& v) {
    if (id == "stereo.temporal_per_eye") g_on = !(v == "0" || v == "false");
  });
}

bool temporal_start() {
  for (const auto& s : kTemporalSigs) {
    const auto* b = reinterpret_cast<const uint8_t*>(game_image().at(s.rva));
    if (memcmp(b, s.bytes, s.n) != 0) {
      TB_LOG("stereo temporal: signature mismatch for %s at RVA %#llx (%02x %02x %02x %02x ...); SSAO, "
             "volumetric and shade histories stay shared between eyes",
             s.name, (unsigned long long)s.rva, b[0], b[1], b[2], b[3]);
      return false;
    }
  }
  g_ok = true;
  TB_LOG("stereo temporal: ready (per-eye SSAO, volumetric-light and shade-effect history)");
  return true;
}

void temporal_reset() {
  for (auto& f : g_fx) {
    f.reset = 1;
    f.have_cam = false;
  }
  g_shade_resync = true;  // both eyes' shade buffers start over together
}

void temporal_enter(void* r, void* view, int eye) {
  if (!g_ok || !r || eye == 0) return;
  bury_due();
  int idx = eye < 0 ? 0 : 1;
  bool on = g_on.load(std::memory_order_relaxed);
  static bool was_on = true;
  if (on && !was_on) temporal_reset();  // the right eye's buffers are stale after a pause
  was_on = on;
  for (auto& f : g_fx) {
    f.swapped = false;
    void* obj = at<void*>(r, f.renderer_off);
    if (!obj) continue;
    void* ert = at<void*>(obj, f.rt_off);
    bool runs = *reinterpret_cast<const uint8_t*>(game_image().at(f.enable_rva)) != 0 ||
                (f.enable_rva == 0xd755e0 && graphics_ssao_on());  // [graphics] ssao patches the branch
    if (eye > 0 && on && runs && ert && ensure_rt(f, ert)) {
      f.l_rt = ert;
      f.l_reset = at<uint8_t>(obj, f.reset_off);
      memcpy(f.l_cam, &at<uint8_t>(obj, f.cam_off), sizeof f.l_cam);
      at<void*>(obj, f.rt_off) = f.rt;
      at<uint8_t>(obj, f.reset_off) = f.reset;
      if (f.have_cam) memcpy(&at<uint8_t>(obj, f.cam_off), f.cam, sizeof f.cam);
      f.swapped = true;
      f.swaps++;
    }
    measure(f, obj, view, idx);
  }
  if (eye < 0 && on) shade_prepare(r);
  if (eye > 0 && on) shade_enter(r);
}

void temporal_leave(void* r, int eye) {
  if (!g_ok || !r || eye <= 0) return;
  shade_leave(r);
  for (auto& f : g_fx) {
    if (!f.swapped) continue;
    f.swapped = false;
    void* obj = at<void*>(r, f.renderer_off);
    if (!obj) continue;
    if (at<void*>(obj, f.rt_off) != f.rt) {
      // The engine replaced its buffer during this render (its re-create destroys the one it held,
      // ours): keep the new one, start the right eye over with a buffer of its own.
      TB_LOG("stereo temporal: engine replaced the %s buffer during the right eye's render", f.name);
      f.rt = nullptr;
      f.reset = 1;
      f.have_cam = false;
      continue;
    }
    f.reset = at<uint8_t>(obj, f.reset_off);
    memcpy(f.cam, &at<uint8_t>(obj, f.cam_off), sizeof f.cam);
    f.have_cam = true;
    at<void*>(obj, f.rt_off) = f.l_rt;
    at<uint8_t>(obj, f.reset_off) = f.l_reset;
    memcpy(&at<uint8_t>(obj, f.cam_off), f.l_cam, sizeof f.l_cam);
  }
}

std::string temporal_status() {
  std::string s = "{\"available\":" + std::string(g_ok ? "true" : "false") +
                  ",\"on\":" + (g_on.load() ? "true" : "false");
  for (auto& f : g_fx) {
    char b[320];
    snprintf(b, sizeof b,
             ",\"%s\":{\"rt\":\"%p\",\"w\":%d,\"h\":%d,\"creates\":%llu,\"swaps\":%llu,"
             "\"jump_mm\":[%.2f,%.2f],\"jump_n\":[%llu,%llu]}",
             f.name, f.rt, f.rt_w, f.rt_h, (unsigned long long)f.creates, (unsigned long long)f.swaps,
             f.jump_n[0] ? f.jump_mm[0] / f.jump_n[0] : 0.0, f.jump_n[1] ? f.jump_mm[1] / f.jump_n[1] : 0.0,
             (unsigned long long)f.jump_n[0], (unsigned long long)f.jump_n[1]);
    s += b;
  }
  char b[200];
  snprintf(b, sizeof b, ",\"shade\":{\"n\":%d,\"w\":%d,\"h\":%d,\"creates\":%llu,\"swaps\":%llu,\"resyncs\":%llu,\"fade_r\":%.3f}",
           g_shade.n, g_shade.w, g_shade.h, (unsigned long long)g_shade.creates, (unsigned long long)g_shade.swaps,
           (unsigned long long)g_shade.resyncs, g_shade.fade);
  s += b;
  return s + "}";
}

}  // namespace tb::stereo
