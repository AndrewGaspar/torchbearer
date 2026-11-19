// Stereoscopic 3D for Alan Wake Remastered (docs/stereo.md).
//
// The lever: rend::View::setViewToWorld is where every camera's transform enters the renderer, and
// everything the GPU sees (g_mWorldToView, g_mViewToWorld, g_mWorldToClip, g_vViewPoint, culling)
// is derived from it. RendererX86::renderFrame tells us which View is the main scene camera (it is
// the one passed in), so the eye offset is applied to that View only, as a translation along its
// own right axis. Shadow, reflection and light views keep their own transforms.
//
// Modes ([stereo] mode):
//   off        pass-through
//   left/right one fixed eye on every frame (mono-frame bias; measurement and sanity)
//   alternate  eyes alternate per presented frame (frame-sequential, like Ishimura)
//   sbs        both eyes from the same simulated frame: the scene is rendered twice per game update
//              (renderFrame re-invoked for the second eye) and composed side by side
#include "features/stereo/stereo.h"

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
#include "core/hyprland.h"
#include "core/quiver.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "features/stereo/cmdlist.h"
#include "features/stereo/engine.h"
#include "features/stereo/eyestate.h"
#include "features/stereo/gpu.h"
#include "features/stereo/pass2.h"
#include "features/stereo/temporal.h"
#include "features/stereo/window.h"

namespace tb::stereo {
namespace {

using RenderFrame_t = void(__fastcall*)(void* r, void* view, void* a3, void* a4);
using SetViewToWorld_t = void(__fastcall*)(void* view, const double* m43);
using SetPostProjection_t = void(__fastcall*)(void* view, const double* m44);
using Void1_t = void(__fastcall*)(void*);
using Bool1_t = char(__fastcall*)(void*);

RenderFrame_t o_renderFrame;
SetViewToWorld_t o_setViewToWorld;
SetPostProjection_t o_setPostProjection;
Void1_t o_modeSwitchRender;
Bool1_t o_beginFrame;
Void1_t o_endFrame;

enum class Mode { Off, Left, Right, Alternate, Sbs };
// sbs: how the two eyes of one game update are rendered.
//   Frame  ModeSwitch::render twice (two whole engine frames; the first Present swallowed)
//   View   renderFrame twice inside one ModeSwitch::render (one engine frame: beginFrame/endFrame,
//          HUD and Present once; the HUD duplicated into both halves)
enum class SbsPath { Frame, View };

struct Params {
  Mode mode = Mode::Off;
  double separation_mm = 32.5;  // per eye
  double convergence_m = 0.0;   // 0 = parallel
  bool swap = false;
  // sbs/alternate: keep the game's TAA and object motion vectors as the user set them (each eye
  // then gets its own history and previous matrices); false = force them off while sbs runs.
  bool taa_in_sbs = true;
  bool objvel_in_sbs = true;
  // dev (stereo.per_eye): 1 all per-eye state, 0 none (shared history / previous view: the old
  // prototype), 2 all but holding the objects' previous transforms for the second eye.
  int per_eye = 1;
  SbsPath path = SbsPath::View;
  // output = hsbs: each eye is rendered anamorphic (16:9 field of view squeezed into a half-width
  // image), for compositors that un-squeeze a half side-by-side window (HypXRland `stereo hsbs`).
  bool anamorphic = false;
};
std::mutex g_pmu;
Params g_p;

Params params() {
  std::lock_guard lk(g_pmu);
  return g_p;
}

Mode parse_mode(const std::string& s) {
  if (s == "left") return Mode::Left;
  if (s == "right") return Mode::Right;
  if (s == "alternate") return Mode::Alternate;
  if (s == "sbs") return Mode::Sbs;
  return Mode::Off;
}
const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Left: return "left";
    case Mode::Right: return "right";
    case Mode::Alternate: return "alternate";
    case Mode::Sbs: return "sbs";
    default: return "off";
  }
}

uintptr_t rva(const void* p) {
  auto a = reinterpret_cast<uintptr_t>(p);
  return game_image().contains(a) ? a - game_image().base : a;
}
template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

// --- census ------------------------------------------------------------------------------------
std::atomic<int> g_census{0};  // presents left to trace
std::atomic<uint64_t> g_census_end{0};
bool census_on() { return g_census.load(std::memory_order_relaxed) > 0; }
void census(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void census(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  TB_LOG("census f=%llu tid=%lu %s", (unsigned long long)present_frame(), GetCurrentThreadId(), buf);
}

// --- eye state ---------------------------------------------------------------------------------
// All of it lives on the thread that calls renderFrame (the game thread, measured).
void* g_main_view = nullptr;   // the View last passed to renderFrame
std::atomic<void*> g_renderer{nullptr};  // RendererX86 (renderFrame's this), for the peek verb
double g_game_v2w[12];         // the game's own (unbiased) transform for g_main_view
bool g_have_game_v2w = false;
double g_applied = 0;          // metres along the view's right axis currently applied
thread_local bool t_in_frame = false;
thread_local double t_conv_shift = 0;  // NDC x shift added to the post-projection (convergence)
std::atomic<int> g_eye_now{0};  // eye of the frame being rendered: -1 left, +1 right, 0 mono
std::atomic<uint64_t> g_frames_biased{0};
std::atomic<uint64_t> g_render_frames{0};

// Signed offset (metres, along view right) for an eye (-1 left, +1 right).
double eye_offset(const Params& p, int eye) {
  if (eye == 0) return 0;
  double e = p.separation_mm / 1000.0 * eye;
  return p.swap ? -e : e;
}

// Measurement lever (stereo.test_pan, degrees per game update): a synthetic yaw about world up,
// advanced once per ModeSwitch::render, so camera motion is exactly known. In alternate mode the
// two eyes of a pair are one update apart and see the pan between them; in sbs they must not.
double g_pan_rad = 0;
std::atomic<double> g_pan_rate{0};

void biased(const double* in, double off, double* out) {
  memcpy(out, in, sizeof(double) * 12);
  if (g_pan_rad != 0) {  // rotate the basis rows about world Y
    double c = std::cos(g_pan_rad), sn = std::sin(g_pan_rad);
    for (int r = 0; r < 3; r++) {
      double x = in[3 * r], z = in[3 * r + 2];
      out[3 * r] = c * x + sn * z;
      out[3 * r + 2] = -sn * x + c * z;
    }
    in = out;
  }
  double rx = in[0], ry = in[1], rz = in[2];
  out[9] += rx * off;  // position += right * off
  out[10] += ry * off;
  out[11] += rz * off;
}

// Apply `off` to the main view now, from the game's own transform.
void apply_offset(void* view, double off) {
  if (!g_have_game_v2w || view != g_main_view) {
    memcpy(g_game_v2w, &at<double>(view, view::kViewToWorld), sizeof g_game_v2w);
    if (g_applied != 0) {  // undo our own bias if the game has not set it since
      for (int i = 0; i < 3; i++) g_game_v2w[9 + i] -= g_game_v2w[i] * g_applied;
    }
    g_have_game_v2w = true;
  }
  double m[12];
  biased(g_game_v2w, off, m);
  o_setViewToWorld(view, m);
  g_applied = off;
}

// --- hooks -------------------------------------------------------------------------------------
void __fastcall hk_setViewToWorld(void* view, const double* m) {
  if (census_on()) {
    census("setViewToWorld ret=%#llx view=%p vt=%#llx pos=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f) right=(%.3f %.3f %.3f)%s",
           (unsigned long long)rva(__builtin_return_address(0)), view,
           (unsigned long long)rva(at<void*>(view, 0)), m[9], m[10], m[11], m[6], m[7], m[8], m[0], m[1],
           m[2], view == g_main_view ? " MAIN" : "");
  }
  if (view == g_main_view && view) {
    memcpy(g_game_v2w, m, sizeof g_game_v2w);
    g_have_game_v2w = true;
    if (g_applied != 0) {
      double b[12];
      biased(m, g_applied, b);
      o_setViewToWorld(view, b);
      return;
    }
  }
  o_setViewToWorld(view, m);
}

void __fastcall hk_setPostProjection(void* view, const double* m) {
  if (census_on() && view == g_main_view) {
    census("setPostProjection ret=%#llx view=%p jitter=(%.6f %.6f)", (unsigned long long)rva(__builtin_return_address(0)),
           view, m[12], m[13]);
  }
  if (t_in_frame && view == g_main_view && t_conv_shift != 0) {
    double b[16];
    memcpy(b, m, sizeof b);
    b[12] += t_conv_shift;
    o_setPostProjection(view, b);
    return;
  }
  o_setPostProjection(view, m);
}

// CPU time on the game thread (QueryPerformanceCounter), accumulated for the status verb: whole
// updates (ModeSwitch::render, including its 240 fps pacing spin), the scene renders of the first
// and second eye of an update (mono: first), endFrame (submission + Present).
std::atomic<uint64_t> g_us_update{0}, g_us_eye[2] = {}, g_us_end{0}, g_n_timed{0};
uint64_t now_us() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return uint64_t(c.QuadPart * 1000000 / f.QuadPart);
}

// sbs: which of the two engine frames of this game update is being rendered (0 = not in sbs).
std::atomic<int> g_pass{0};
std::atomic<uint64_t> g_last_render_present{0};
std::atomic<uint64_t> g_sbs_updates{0};
// sbs_path = view: this ModeSwitch::render renders both eyes inside its one renderFrame call.
bool g_view_update = false;  // game thread only
bool g_view_done = false;    // both eyes rendered in this update
std::atomic<bool> g_view_present{false};  // the next Present is a view-path sbs frame
std::atomic<IDXGISwapChain*> g_sc{nullptr};  // the game's swapchain (last presented)
int pick_eye(const Params& p) {
  switch (p.mode) {
    case Mode::Sbs: return g_pass == 1 ? -1 : g_pass == 2 ? 1 : 0;
    case Mode::Left: return -1;
    case Mode::Right: return 1;
    case Mode::Alternate: return (present_frame() & 1) ? 1 : -1;
    default: return 0;
  }
}

void render_eye(void* r, void* view, void* a3, void* a4, const Params& p, int eye) {
  double off = eye_offset(p, eye);
  if (off != g_applied || view != g_main_view || g_pan_rad != 0) {
    g_main_view = view;
    apply_offset(view, off);
  }
  // Convergence: shift NDC x by +off*m00/Zc so the disparity is zero at Zc metres.
  t_conv_shift = 0;
  // The main view is a BaseCamera (vtable RVA 0x752ad0), a PerspectiveView subclass: the FOV/zoom
  // fields are PerspectiveView's. Sanity-check them instead of the exact vtable.
  float fov = at<float>(view, view::kFov), zoom = at<float>(view, view::kZoom);
  if (off != 0 && p.convergence_m > 0 && fov > 0.1f && fov < 3.0f && zoom > 0.01f && zoom < 100.f) {
    double m00 = 1.0 / std::tan(double(fov) * 0.5) / double(zoom ? zoom : 1.0f);
    double ndc = off * m00 / p.convergence_m;
    // Full side-by-side: shift the finished eye images instead (gpu_set_hit). A shift in the
    // projection is largely undone by the TAA resolve (+5..6.5 px measured for 18.9 expected).
    if (p.mode == Mode::Sbs && present_width_scale_pending() > 1) {
      // Copies move whole pixels: round the total (both eyes) and split it, so the screen plane
      // lands within half a pixel of Zc (rounding each eye lost 0.86 px at 4 m: measured 17.98
      // for 18.86 px).
      uint32_t w = gpu_eye_width();
      int total = int(std::lround(std::fabs(ndc) * (w ? w : 1920)));
      int a = total / 2, b = total - a;
      int px = eye < 0 ? a : b;
      static int hit[2];
      hit[eye < 0 ? 0 : 1] = ndc < 0 ? -px : px;
      gpu_set_hit(hit[0], hit[1]);
    } else {
      t_conv_shift = ndc;
    }
  } else if (p.convergence_m <= 0) {
    gpu_set_hit(0, 0);
  }
  g_eye_now = eye;
  // Per-eye temporal state (eyestate.cpp): TAA history and ping-pong index, the previous camera
  // matrices behind camera/object motion vectors, the jitter phase. Without it, "last frame" for
  // one eye is the other eye and the eye offset reads as motion.
  bool per_eye = eye != 0 && (p.mode == Mode::Sbs || p.mode == Mode::Alternate);
  if (per_eye) {
    EyeOpts o;
    o.taa_history = p.per_eye != 0;
    o.view_prev = p.per_eye != 0;
    o.same_jitter = p.per_eye != 0 && p.mode == Mode::Sbs;
    o.another_eye_follows = p.per_eye == 1 && p.mode == Mode::Sbs && eye < 0;
    // The left eye is rendered first in every sbs update; in alternate mode every frame is its
    // own update.
    eyestate_enter(r, view, eye, p.mode == Mode::Alternate || eye < 0, o);
    temporal_enter(r, view, eye);  // SSAO / volumetric-light history (temporal.cpp)
  }
  t_in_frame = true;
  o_renderFrame(r, view, a3, a4);
  g_render_frames++;  // one scene render (one eye)
  t_in_frame = false;
  if (per_eye) {
    temporal_leave(r, eye);
    eyestate_leave(r, view, eye);
  }
  if (off != 0) g_frames_biased++;
}

// sbs_path = view: both eyes inside this one renderFrame call, i.e. inside one engine frame
// (beginFrame .. endFrame, HUD, Present all once per update). The right eye renders first; once
// renderFrame returns its lists are submitted (live census: the scene's ExecuteCommandLists is
// inside renderFrame) and its final image is in the back buffer's first eye-width columns, so a
// copy queued now on the game's queue lands after the right eye's work and before the left eye's.
// The left eye then renders into the same columns, the stash goes into the second half, and
// everything drawn into the back buffer after this (HUD, overlays) is duplicated into the second
// half by cmdlist.cpp until Present.
std::atomic<bool> g_view_kick{true};  // stereo.view_kick
std::atomic<uint64_t> g_kicks{0};
void engine_kick() {
  const auto& img = game_image();
  void* dev = *reinterpret_cast<void**>(img.at(kRvaDevice));
  if (!dev) return;
  void* fn = (*reinterpret_cast<void***>(dev))[kDeviceKickSlot];
  if (reinterpret_cast<uintptr_t>(fn) != img.at(kRvaDeviceKick)) return;  // not the device we know
  reinterpret_cast<Void1_t>(fn)(dev);
  g_kicks++;
}

void render_view_sbs(void* r, void* view, void* a3, void* a4, const Params& p) {
  g_view_done = true;
  IDXGISwapChain* sc = g_sc.load();
  // Left eye first, as in every sbs update (eyestate.cpp: the first eye of an update advances the
  // jitter phase and holds the objects' previous transforms for the eye that follows).
  pass2_eye(0, r);
  uint64_t t0 = now_us();
  render_eye(r, view, a3, a4, p, -1);
  uint64_t t1 = now_us();
  g_us_eye[0] += t1 - t0;
  bool stashed = sc && gpu_stash_mid(sc, 0);
  // The engine submits a render's lists in two batches: at its kick inside renderWorld and in
  // endFrame. Kick now, so the first eye's second batch (lighting, post and the stash copy just
  // recorded into it) reaches the GPU before the second eye is recorded, not halfway through it.
  if (g_view_kick.load()) engine_kick();
  if (census_on()) census("view sbs: left eye rendered, stashed=%d", stashed);
  pass2_eye(1, r);
  t0 = now_us();
  render_eye(r, view, a3, a4, p, 1);
  g_us_eye[1] += now_us() - t0;
  pass2_eye(-1);
  bool placed = false;
  if (stashed && present_width_scale(sc) >= 2 && gpu_place_mid(sc, 0)) {
    placed = true;
    cmdlist_duplicate(GetCurrentThreadId(), gpu_eye_width());
  }
  g_view_present = stashed;
  if (census_on()) census("view sbs: right eye rendered, eyes placed=%d (overlay duplication armed)", placed);
}

// Half side-by-side (output = hsbs): the engine renders one eye at half the window's width
// (borderless: half the desktop, window.cpp), so its own camera would have a half-width aspect.
// The game keeps the vertical FOV and derives the horizontal one from the aspect (live: 1920x1080
// fov 1.3823 rad, 960x1080 fov 0.7844 = 2*atan(tan(1.3823/2) * 0.8889/1.7778)). Here the main
// camera gets twice its aspect and the matching horizontal FOV before each render, so every eye
// image holds the full-width view squeezed 2:1, which the compositor stretches back. Applied from
// the game's own values each update (the game rewrites them; ours are recognised and not
// compounded).
// Preferred route (live-verified, docs/stereo-output.md): the game's one aspect getter,
// DisplaySettings::aspect (RVA 0x1e05d0, `+0x20 / +0x24` of the display object), feeds the 3D camera
// AND the 2D UI context (its +0x70, which scales every glyph and sprite horizontally). Reporting
// twice the real aspect makes the game itself lay the UI out at 16:9 and pick the 16:9 field of
// view, both squeezed into the half-width eye. anamorphic_apply below is the fallback when that
// hook is unavailable (signature mismatch).
constexpr uintptr_t kRvaAspect = 0x1e05d0;
constexpr uint8_t kSigAspect[21] = {0x66, 0x0f, 0x6e, 0x41, 0x20, 0x66, 0x0f, 0x6e, 0x49, 0x24, 0x0f,
                                    0x5b, 0xc0, 0x0f, 0x5b, 0xc9, 0xf3, 0x0f, 0x5e, 0xc1, 0xc3};
using Aspect_t = float(__fastcall*)(void*);
Aspect_t o_aspect;
std::atomic<bool> g_aspect_hooked{false}, g_ana_on{false};
float __fastcall hk_aspect(void* display) {
  float a = o_aspect(display);
  return g_ana_on.load(std::memory_order_relaxed) ? a * 2.0f : a;
}

// The pixel-sized UI (menu logo, button prompts…) uses DisplaySettings::invResolution (RVA
// 0x1e0650, out = {1/w, 1/h}, letterbox-adjusted): "1024 px" becomes 1024/w of the screen. With
// the engine at half width that doubles; report 1/(2w) so it matches the 16:9 layout above.
constexpr uintptr_t kRvaInvRes = 0x1e0650;
constexpr uint8_t kSigInvRes[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x44, 0x8b, 0x51,
                                    0x20, 0x45, 0x33, 0xc0, 0x44, 0x8b, 0x59, 0x24};
using InvRes_t = float*(__fastcall*)(void*, float*);
InvRes_t o_invres;
float* __fastcall hk_invres(void* display, float* out) {
  float* r = o_invres(display, out);
  if (out && g_ana_on.load(std::memory_order_relaxed)) out[0] *= 0.5f;
  return r;
}

// Bink videos (main-menu backdrop, cutscenes): the 2D renderer's video quad (command type 5,
// executed in 0x466af0) fits the video by comparing the video's aspect with the REAL viewport's
// (the renderer's screen-size globals 0xd745f8/fc, also the shaders' g_vScreenRes, so not something
// to fake): a 16:9 video on the half-width eye (0.889) was letterboxed to half height, i.e. 32:9
// after the compositor's stretch (live, in the glasses). The quad is recorded by one function only
// (0x464c00, from the video draw 0x338b10) with the video's aspect and an aspect bound as its 8th
// and 9th arguments; in hsbs both are halved, so the fit sees the squeezed eye's own shape and the
// video fills it.
constexpr uintptr_t kRvaVideoQuad = 0x464c00;
constexpr uint8_t kSigVideoQuad[16] = {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x08, 0x49,
                                       0x89, 0x73, 0x10, 0x57, 0x48, 0x83, 0xec, 0x70};
using VideoQuad_t = void(__fastcall*)(void*, void*, void*, void*, void*, uint32_t, uint8_t, float, float, float,
                                      float, float);
VideoQuad_t o_videoQuad;
std::atomic<uint64_t> g_video_quads{0};
void __fastcall hk_videoQuad(void* ctx, void* uv, void* cb, void* user, void* tex, uint32_t a6, uint8_t a7,
                             float aspect, float bound, float a10, float a11, float a12) {
  if (g_ana_on.load(std::memory_order_relaxed)) {
    aspect *= 0.5f;
    bound *= 0.5f;
    g_video_quads++;
  }
  o_videoQuad(ctx, uv, cb, user, tex, a6, a7, aspect, bound, a10, a11, a12);
}

// The 2D context's aspect (+0x70) is written from the getter only at boot (0x2ab8f0) and when some
// menus open (0x249690, 0x24b4e0). The game boots at its saved resolution (often 1440x900) and only
// then switches to borderless, so screens that never rewrite it (the title screen) keep the boot
// value: with the doubled getter 2 x 1.6 = 3.2, text half as wide as it should be (live). In hsbs
// the mod rewrites it every frame from the game's own getter (Game::aspect, 0x1d23d0), i.e. the
// value those menus would write.
constexpr uintptr_t kRvaGameAspect = 0x1d23d0;
constexpr uintptr_t kRva2DContext = 0xd6b598;
constexpr uint8_t kSigGameAspect[9] = {0x48, 0x83, 0xec, 0x28, 0xe8, 0x57, 0xe2, 0x00, 0x00};
bool g_ui_aspect_ok = false;
void refresh_ui_aspect() {
  if (!g_ui_aspect_ok || !g_ana_on.load(std::memory_order_relaxed) || !g_aspect_hooked.load()) return;
  auto* ctx = *reinterpret_cast<uint8_t**>(game_image().at(kRva2DContext));
  if (!ctx) return;
  float a = reinterpret_cast<float(__fastcall*)()>(game_image().at(kRvaGameAspect))();
  if (a > 0.1f && a < 10.f) *reinterpret_cast<float*>(ctx + 0x70) = a;
}

float g_ana_orig_fov = 0, g_ana_orig_aspect = 0, g_ana_set_fov = -1, g_ana_set_aspect = -1;
std::atomic<uint64_t> g_ana_applied{0};
void anamorphic_apply(void* view) {
  if (g_aspect_hooked.load(std::memory_order_relaxed)) return;  // the game does it (hk_aspect)
  float& fov = at<float>(view, view::kFov);
  float& aspect = at<float>(view, view::kAspect);
  if (!(fov == g_ana_set_fov && aspect == g_ana_set_aspect)) {
    g_ana_orig_fov = fov;
    g_ana_orig_aspect = aspect;
  }
  if (!(g_ana_orig_fov > 0.05f && g_ana_orig_fov < 3.0f && g_ana_orig_aspect > 0.1f && g_ana_orig_aspect < 10.f))
    return;
  float want_aspect = g_ana_orig_aspect * 2.0f;
  float want_fov = float(2.0 * std::atan(std::tan(double(g_ana_orig_fov) * 0.5) * 2.0));
  if (fov != want_fov || aspect != want_aspect) {
    fov = want_fov;
    aspect = want_aspect;
    at<uint8_t>(view, view::kDirtyProj) = 0;   // view-to-clip is rebuilt from these
    at<uint8_t>(view, view::kDirtyWorld) = 0;  // and world-to-clip from that
  }
  g_ana_set_fov = want_fov;
  g_ana_set_aspect = want_aspect;
  g_ana_applied++;
}

void __fastcall hk_renderFrame(void* r, void* view, void* a3, void* a4) {
  if (census_on()) {
    const double* m = &at<double>(view, view::kViewToWorld);
    census("renderFrame ret=%#llx r=%p view=%p vt=%#llx a3=%p a4=%p fov=%.4f zoom=%.3f aspect=%.4f near=%.3f far=%.1f pos=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f)",
           (unsigned long long)rva(__builtin_return_address(0)), r, view,
           (unsigned long long)rva(at<void*>(view, 0)), a3, a4, at<float>(view, view::kFov),
           at<float>(view, view::kZoom), at<float>(view, view::kAspect), at<float>(view, view::kNear),
           at<float>(view, view::kFar), m[9], m[10], m[11], m[6], m[7], m[8]);
  }
  g_last_render_present = present_frame();
  g_renderer = r;
  if (g_render_frames.load() == 0) {
    TB_LOG("stereo: first renderFrame (view %p)", view);
  }
  Params p = params();
  if (p.anamorphic) anamorphic_apply(view);
  if (p.mode == Mode::Off && g_applied == 0) {
    g_main_view = view;
    uint64_t t0 = now_us();
    o_renderFrame(r, view, a3, a4);
    g_us_eye[0] += now_us() - t0;
    g_render_frames++;
    if (census_on()) census("renderFrame leave");
    return;
  }
  if (g_view_update && !g_view_done) {
    render_view_sbs(r, view, a3, a4, p);
    return;
  }
  uint64_t t0 = now_us();
  render_eye(r, view, a3, a4, p, pick_eye(p));
  g_us_eye[g_pass == 2 ? 1 : 0] += now_us() - t0;
  if (census_on()) census("renderFrame leave");
}

// The game's TAA switch (Options > Anti-aliasing; also forced off by DLSS): with it on, the second
// eye would resolve against the first eye's history. Off for the duration of sbs unless asked.
constexpr uintptr_t kRvaTaaEnable = 0x955636;   // u8, read by renderFrame (jitter) and renderWorld
constexpr uintptr_t kRvaTaaEnable2 = 0x955694;  // u8, set together with it by the options code
bool g_taa_forced = false;
uint8_t g_taa_saved[2];

// Tweakable lookup by name in the engine's registry (engine.h). nullptr if not registered (yet).
uint8_t* tweak_find(const char* name) {
  const auto& img = game_image();
  auto** arr = reinterpret_cast<uint8_t**>(img.at(kRvaTweakArray));
  int n = *reinterpret_cast<int*>(img.at(kRvaTweakCount));
  for (int i = 0; i < n && i < 0x500; i++) {
    const char* nm = arr[i] ? *reinterpret_cast<const char**>(arr[i]) : nullptr;
    if (nm && strcmp(nm, name) == 0) return arr[i];
  }
  return nullptr;
}

// Object motion vectors: skinned/moving objects keep last frame's transform from the previous
// *render*, which in sbs is the other eye, so the eye offset reads as object velocity (the player
// smears horizontally in both eyes; measured). Off while sbs runs.
bool g_objvel_forced = false;
uint8_t g_objvel_saved = 1;
void objvel_force_off(bool off) {
  if (off == g_objvel_forced) return;  // nothing to change (no registry scan per frame)
  uint8_t* t = tweak_find("TAA:Enable Object Velocity");
  if (!t) return;
  if (off && !g_objvel_forced) {
    g_objvel_saved = t[kTweakValue];
    t[kTweakValue] = 0;
    g_objvel_forced = true;
    TB_LOG("stereo: object velocity forced off for sbs (was %u)", g_objvel_saved);
  } else if (!off && g_objvel_forced) {
    t[kTweakValue] = g_objvel_saved;
    g_objvel_forced = false;
  }
}

void taa_force_off(bool off) {
  uint8_t* a = reinterpret_cast<uint8_t*>(game_image().at(kRvaTaaEnable));
  uint8_t* b = reinterpret_cast<uint8_t*>(game_image().at(kRvaTaaEnable2));
  if (off && !g_taa_forced) {
    g_taa_saved[0] = *a;
    g_taa_saved[1] = *b;
    *a = 0;
    *b = 0;
    g_taa_forced = true;
    TB_LOG("stereo: TAA forced off for sbs (was %u/%u)", g_taa_saved[0], g_taa_saved[1]);
  } else if (!off && g_taa_forced) {
    *a = g_taa_saved[0];
    *b = g_taa_saved[1];
    g_taa_forced = false;
    TB_LOG("stereo: TAA restored (%u/%u)", g_taa_saved[0], g_taa_saved[1]);
  }
}

bool g_was_sbs = false;

// One game update = one ModeSwitch::render. In sbs mode (and only while the world is being
// rendered) it runs twice: left eye, then right eye, from the same simulated state; the first
// Present is swallowed after its back buffer is stashed, the second is composed and presented.
void __fastcall hk_modeSwitchRender(void* self) {
  if (census_on()) census("ModeSwitch::render enter ret=%#llx", (unsigned long long)rva(__builtin_return_address(0)));
  Params p = params();
  double rate = g_pan_rate.load();
  g_pan_rad = rate != 0 ? g_pan_rad + rate * M_PI / 180.0 : 0;
  bool world = present_frame() - g_last_render_present.load() < 8;
  // Side-by-side needs a packed output (output = full | hsbs: a 2x-wide swapchain); without one
  // there is nowhere to put the second eye, so the game renders one (mono) image.
  IDXGISwapChain* psc = g_sc.load();
  bool packed = psc && present_width_scale(psc) > 1;
  bool sbs = p.mode == Mode::Sbs && world && packed;
  // Without per-eye state (signature mismatch) TAA and object velocity would blend / smear across
  // eyes, so they are switched off while sbs runs, as they are when the user asks for it.
  taa_force_off(sbs && (!p.taa_in_sbs || !eyestate_history_available()));
  objvel_force_off(sbs && (!p.objvel_in_sbs || !eyestate_available()));
  if (sbs != g_was_sbs) {
    eyestate_reset();
    temporal_reset();
    g_was_sbs = sbs;
  }
  eyestate_update(sbs && p.per_eye != 0);  // DLSS: one history, two eyes -> reset while sbs runs
  IDXGISwapChain* sc = g_sc.load();
  bool view = sbs && p.path == SbsPath::View && sc && cmdlist_install(sc);
  cmdlist_track(view);
  uint64_t t0 = now_us();
  if (!sbs) {
    o_modeSwitchRender(self);
  } else if (view) {
    g_view_update = true;
    g_view_done = false;
    o_modeSwitchRender(self);
    g_view_update = false;
    cmdlist_duplicate(0, 0);  // normally disarmed at Present; also if this update had none
    if (g_view_done) g_sbs_updates++;
  } else {
    g_pass = 1;
    o_modeSwitchRender(self);
    g_pass = 2;
    o_modeSwitchRender(self);
    g_pass = 0;
    g_sbs_updates++;
  }
  g_us_update += now_us() - t0;
  g_n_timed++;
  if (census_on()) census("ModeSwitch::render leave");
}
char __fastcall hk_beginFrame(void* self) {
  char c = o_beginFrame(self);
  if (census_on()) census("Renderer::beginFrame -> %d ret=%#llx", c, (unsigned long long)rva(__builtin_return_address(0)));
  return c;
}
void __fastcall hk_endFrame(void* self) {
  if (census_on()) census("Renderer::endFrame enter ret=%#llx", (unsigned long long)rva(__builtin_return_address(0)));
  uint64_t t0 = now_us();
  o_endFrame(self);
  g_us_end += now_us() - t0;
  if (census_on()) census("Renderer::endFrame leave");
}

// --- command queue trace -------------------------------------------------------------------
using ExecuteCommandLists_t = void(STDMETHODCALLTYPE*)(void* q, UINT n, void* const* lists);
ExecuteCommandLists_t o_executeCommandLists;
void STDMETHODCALLTYPE hk_executeCommandLists(void* q, UINT n, void* const* lists) {
  if (census_on()) {
    std::string ls;
    for (UINT i = 0; i < n && i < 32; i++) {
      char b[24];
      snprintf(b, sizeof b, " %p", lists[i]);
      ls += b;
    }
    census("ExecuteCommandLists q=%p n=%u ret=%#llx%s lists:%s", q, n, (unsigned long long)rva(__builtin_return_address(0)),
           t_in_frame ? " IN-RENDERFRAME" : "", ls.c_str());
  }
  o_executeCommandLists(q, n, lists);
}

// Does the engine ask the swapchain which buffer is current (so a swallowed Present is safe)?
using GetCurrentBackBufferIndex_t = UINT(STDMETHODCALLTYPE*)(void* sc);
GetCurrentBackBufferIndex_t o_getCurrentBackBufferIndex;
UINT STDMETHODCALLTYPE hk_getCurrentBackBufferIndex(void* sc) {
  UINT i = o_getCurrentBackBufferIndex(sc);
  if (census_on())
    census("GetCurrentBackBufferIndex -> %u ret=%#llx", i, (unsigned long long)rva(__builtin_return_address(0)));
  return i;
}

// --- captures ----------------------------------------------------------------------------------
std::mutex g_cmu;
int g_shots_left = 0;
int g_shot_skip = 0;
std::string g_shot_tag;
std::string g_shot_dir;
std::vector<std::string> g_shot_files;

void on_present(IDXGISwapChain* sc, uint64_t frame) {
  refresh_ui_aspect();  // game thread, before the next frame's 2D drawing
  g_sc = sc;
  static bool ecl_hooked = false;
  if (!ecl_hooked) {
    ecl_hooked = true;
    if (void* q = present_queue(sc)) {
      o_executeCommandLists = reinterpret_cast<ExecuteCommandLists_t>(hook_vtable(q, 10, (void*)hk_executeCommandLists));
      TB_LOG("stereo: traced ExecuteCommandLists on queue %p", q);
    }
    o_getCurrentBackBufferIndex =
        reinterpret_cast<GetCurrentBackBufferIndex_t>(hook_vtable(sc, 36, (void*)hk_getCurrentBackBufferIndex));
  }
  if (g_census.load() > 0) {
    census("Present");
    if (--g_census == 0) {
      cmdlist_trace(false);
      TB_LOG("census done");
    }
  }
}

void capture_if_armed(IDXGISwapChain* sc, uint64_t frame, const char* eye_tag) {
  std::lock_guard lk(g_cmu);
  if (g_shot_skip > 0) {
    g_shot_skip--;
    return;
  }
  if (g_shots_left > 0) {
    char name[512];
    snprintf(name, sizeof name, "%s\\frame_%08llu_%s_%s.bmp", g_shot_dir.c_str(), (unsigned long long)frame,
             g_shot_tag.c_str(), eye_tag);
    if (gpu_capture(sc, name)) g_shot_files.push_back(name);
    g_shots_left--;
  }
}

// After the listeners, just before the real Present.
bool present_gate(IDXGISwapChain* sc, uint64_t frame) {
  if (g_view_present.exchange(false)) {  // both eyes of one update in this one frame
    cmdlist_duplicate(0, 0);
    gpu_compose_view(sc, 0);
    capture_if_armed(sc, frame, "SBS");
    return true;
  }
  int pass = g_pass.load();
  if (pass == 1) {  // left eye of an sbs update: keep it, don't show it
    capture_if_armed(sc, frame, "L");
    gpu_store_eye(sc, 0);
    return false;
  }
  if (pass == 2) {
    gpu_compose(sc);
    capture_if_armed(sc, frame, "SBS");
    return true;
  }
  int eye = g_eye_now.load();
  // Full-SBS output without a stereo pair this frame (mode off/left/right/alternate, menus,
  // videos): show the engine's single image to both eyes (2D in the glasses) rather than one
  // eye and an undefined half.
  if (present_width_scale(sc) > 1) gpu_duplicate(sc);
  capture_if_armed(sc, frame, eye < 0 ? "L" : eye > 0 ? "R" : "M");
  return true;
}

bool check_sigs() {
  const auto& img = game_image();
  for (const auto& s : kSigs) {
    if (memcmp(reinterpret_cast<const void*>(img.at(s.rva)), s.bytes, sizeof s.bytes) != 0) {
      const uint8_t* b = reinterpret_cast<const uint8_t*>(img.at(s.rva));
      TB_LOG("stereo: signature mismatch for %s at RVA %#llx (%02x %02x %02x %02x ...); feature disabled",
             s.name, (unsigned long long)s.rva, b[0], b[1], b[2], b[3]);
      return false;
    }
  }
  return true;
}

std::string status_json() {
  Params p = params();
  char b[4096];
  snprintf(b, sizeof b,
           "{\"mode\":\"%s\",\"sbs_path\":\"%s\",\"separation_mm\":%.3f,\"convergence_m\":%.3f,\"swap\":%s,"
           "\"main_view\":\"%p\",\"applied_m\":%.5f,\"frames_biased\":%llu,\"render_frames\":%llu,\"sbs_updates\":%llu,"
           "\"present\":%llu,\"taa_forced_off\":%s,\"taa_game\":%u,\"objvel_forced_off\":%s,\"eyestate\":%s,"
           "\"temporal\":%s,\"gpu\":\"%s\",\"cl\":\"%s\",\"pass2\":\"%s\",\"kicks\":%llu,\"timed_updates\":%llu,\"us_update\":%llu,"
           "\"us_eye1\":%llu,\"us_eye2\":%llu,\"us_endframe\":%llu}",
           mode_name(p.mode), p.path == SbsPath::View ? "view" : "frame", p.separation_mm, p.convergence_m,
           p.swap ? "true" : "false", g_main_view, g_applied, (unsigned long long)g_frames_biased.load(),
           (unsigned long long)g_render_frames.load(), (unsigned long long)g_sbs_updates.load(),
           (unsigned long long)present_frame(), g_taa_forced ? "true" : "false",
           *reinterpret_cast<uint8_t*>(game_image().at(kRvaTaaEnable)), g_objvel_forced ? "true" : "false",
           eyestate_status_json().c_str(), temporal_status().c_str(), gpu_status().c_str(), cmdlist_status().c_str(), pass2_status().c_str(),
           (unsigned long long)g_kicks.load(), (unsigned long long)g_n_timed.load(),
           (unsigned long long)g_us_update.load(), (unsigned long long)g_us_eye[0].load(),
           (unsigned long long)g_us_eye[1].load(), (unsigned long long)g_us_end.load());
  return b;
}

bool g_hooked = false;

// The packed output declares itself to HypXRland: a Hyprland window tag `stereo-<layout>` on the
// game's window, which the compositor's config maps to `stereo <layout> always` (core/hyprland.cpp,
// docs/stereo-output.md). Kept whenever the packed output is on: with stereo off or in menus both
// halves carry the same image, which is still a valid packed frame (2D in the glasses).
std::string g_tag_layout;
bool g_tag_on = true;
void update_window_tag() {
  hypr_set_window_tag(g_tag_on && !g_tag_layout.empty() ? "stereo-" + g_tag_layout : "");
  // Quiver's Sight HUD/panel: the packing, whether or not stereo rendering is on.
  quiver_stereo_layout(g_tag_layout.empty() ? "mono" : g_tag_layout.c_str());
}

}  // namespace

void register_feature() {
  cfg_listen("stereo.", [](const std::string& id, const std::string& v) {
    std::lock_guard lk(g_pmu);
    if (id == "stereo.mode") g_p.mode = parse_mode(v);
    else if (id == "stereo.separation") g_p.separation_mm = atof(v.c_str());
    else if (id == "stereo.convergence") g_p.convergence_m = atof(v.c_str());
    else if (id == "stereo.swap_eyes") g_p.swap = (v == "1" || v == "true");
    else if (id == "stereo.test_pan") g_pan_rate = atof(v.c_str());
    else if (id == "stereo.taa") g_p.taa_in_sbs = (v == "1" || v == "true");
    else if (id == "stereo.object_velocity") g_p.objvel_in_sbs = (v == "1" || v == "true");
    else if (id == "stereo.per_eye") g_p.per_eye = (v == "0" || v == "false") ? 0 : v == "2" ? 2 : 1;
    else if (id == "stereo.hyprland_tag") {
      g_tag_on = v != "0" && v != "false";
      update_window_tag();
    } else if (id == "stereo.output") {
      // full / hsbs: the swapchain (and window) are twice the engine's width, read at swapchain
      // creation; hsbs additionally squeezes each eye (anamorphic, above). half: legacy crop.
      present_set_width_scale(v == "full" || v == "hsbs" ? 2 : 1);
      g_p.anamorphic = v == "hsbs";
      g_ana_on = g_p.anamorphic;
      window_set_stretched(g_p.anamorphic);
      g_tag_layout = v == "full" ? "sbs" : v == "hsbs" ? "hsbs" : "";
      update_window_tag();
    }
    else if (id == "stereo.sbs_path") g_p.path = v == "frame" ? SbsPath::Frame : SbsPath::View;
    else if (id == "stereo.view_kick") g_view_kick = (v == "1" || v == "true");
  });
  // Back buffers and their RTVs (bookkeeping only; cmdlist.cpp needs them to recognise the back
  // buffer in command lists when sbs_path = view is used).
  resize_on([](IDXGISwapChain* sc, unsigned, unsigned) { cmdlist_on_swapchain(sc); });
  pass2_register();
  temporal_register();
  // quiver lab call awr tweak list [SUBSTR] | tweak NAME [VALUE]: the engine's tweakable registry.
  lab_verb("tweak", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    if (a.empty()) {
      ok = false;
      return json_str("usage: tweak list [SUBSTR] | tweak NAME [VALUE]");
    }
    auto describe = [](uint8_t* t) {
      char b[256];
      unsigned type = *reinterpret_cast<unsigned*>(t + kTweakType);
      const char* nm = *reinterpret_cast<const char**>(t);
      if (type == 0) snprintf(b, sizeof b, "%s = %u (bool)", nm, t[kTweakValue]);
      else if (type == 1)
        snprintf(b, sizeof b, "%s = %g (float %g..%g)", nm, *reinterpret_cast<float*>(t + kTweakValue),
                 *reinterpret_cast<float*>(t + kTweakValue + 4), *reinterpret_cast<float*>(t + kTweakValue + 8));
      else snprintf(b, sizeof b, "%s (type %u, raw %08x)", nm, type, *reinterpret_cast<unsigned*>(t + kTweakValue));
      return std::string(b);
    };
    if (a[0] == "list") {
      const auto& img = game_image();
      auto** arr = reinterpret_cast<uint8_t**>(img.at(kRvaTweakArray));
      int n = *reinterpret_cast<int*>(img.at(kRvaTweakCount));
      std::string r = "[";
      bool first = true;
      for (int i = 0; i < n && i < 0x500; i++) {
        if (!arr[i]) continue;
        std::string d = describe(arr[i]);
        if (a.size() > 1 && d.find(a[1]) == std::string::npos) continue;
        r += (first ? "" : ",") + json_str(d);
        first = false;
      }
      return r + "]";
    }
    uint8_t* t = tweak_find(a[0].c_str());
    if (!t) {
      ok = false;
      return json_str("no tweakable named " + a[0] + " (not registered yet?)");
    }
    if (a.size() > 1) {
      unsigned type = *reinterpret_cast<unsigned*>(t + kTweakType);
      if (type == 0) t[kTweakValue] = uint8_t(atoi(a[1].c_str()) != 0);
      else if (type == 1) *reinterpret_cast<float*>(t + kTweakValue) = float(atof(a[1].c_str()));
      else {
        ok = false;
        return json_str("only bool and float tweakables can be set");
      }
      TB_LOG("tweak %s set by lab", describe(t).c_str());
    }
    return json_str(describe(t));
  });
  // quiver lab call awr stereo [status | census N | shots N TAG [SKIP] | files]
  lab_verb("stereo", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    std::string sub = a.empty() ? "status" : a[0];
    if (sub == "status") return status_json();
    if (sub == "window") return window_status();
    if (sub == "hyprland") return hypr_status();
    if (!g_hooked) {
      ok = false;
      return json_str("stereo hooks not installed (see torchbearer.log)");
    }
    if (sub == "census") {
      int n = a.size() > 1 ? atoi(a[1].c_str()) : 2;
      // "census N cl": also trace command-list calls touching the back buffer.
      if (a.size() > 2 && a[2] == "cl" && g_sc.load() && cmdlist_install(g_sc.load())) cmdlist_trace(true);
      g_census = n;
      return json_str("census armed for " + std::to_string(n) + " presents");
    }
    if (sub == "shots") {
      std::lock_guard lk(g_cmu);
      g_shots_left = a.size() > 1 ? atoi(a[1].c_str()) : 2;
      g_shot_tag = a.size() > 2 ? a[2] : "shot";
      g_shot_skip = a.size() > 3 ? atoi(a[3].c_str()) : 0;
      const char* d = getenv("QUIVER_LAB_RUN_DIR");
      g_shot_dir = d ? d : ".";
      g_shot_files.clear();
      return json_str("capturing " + std::to_string(g_shots_left) + " frames to " + g_shot_dir);
    }
    if (sub == "probe") {
      eyestate_probe(a.size() > 1 ? atoi(a[1].c_str()) : 4);
      return json_str("probing the next eye renders into torchbearer.log");
    }
    if (sub == "peek") {  // peek BASE [+OFF | * ]...: BASE = r (RendererX86), img, or an address
      if (a.size() < 2) {
        ok = false;
        return json_str("usage: stereo peek r|img|ADDR [+OFF|*]...");
      }
      uintptr_t v = a[1] == "r" ? reinterpret_cast<uintptr_t>(g_renderer.load())
                    : a[1] == "img" ? game_image().base
                                    : uintptr_t(strtoull(a[1].c_str(), nullptr, 16));
      std::string trail;
      for (size_t i = 2; i < a.size() && v; i++) {
        if (a[i] == "*") {
          if (IsBadReadPtr(reinterpret_cast<void*>(v), 8)) return json_str("bad pointer after " + trail);
          v = *reinterpret_cast<uintptr_t*>(v);
        } else {
          v += uintptr_t(strtoll(a[i].c_str(), nullptr, 16));
        }
        trail += " " + a[i];
      }
      char b[96];
      snprintf(b, sizeof b, "%#llx%s%#llx", (unsigned long long)v, game_image().contains(v) ? " rva " : "",
               game_image().contains(v) ? (unsigned long long)(v - game_image().base) : 0ull);
      return json_str(b);
    }
    if (sub == "rand") return json_str(pass2_rand_sites());
    if (sub == "files") {
      std::lock_guard lk(g_cmu);
      std::string r = "[";
      for (size_t i = 0; i < g_shot_files.size(); i++) r += (i ? "," : "") + json_str(g_shot_files[i]);
      return r + "]";
    }
    ok = false;
    return json_str("usage: stereo [status|census N|shots N TAG [SKIP]|files|probe N]");
  });
}

void start() {
  if (!check_sigs()) return;
  const auto& img = game_image();
  bool ok = hook_fn((void*)img.at(kRvaRenderFrame), (void*)hk_renderFrame, (void**)&o_renderFrame,
                    "RendererX86::renderFrame") &&
            hook_fn((void*)img.at(kRvaSetViewToWorld), (void*)hk_setViewToWorld, (void**)&o_setViewToWorld,
                    "View::setViewToWorld") &&
            hook_fn((void*)img.at(kRvaSetPostProjection), (void*)hk_setPostProjection,
                    (void**)&o_setPostProjection, "View::setPostProjection") &&
            hook_fn((void*)img.at(kRvaModeSwitchRender), (void*)hk_modeSwitchRender, (void**)&o_modeSwitchRender,
                    "ModeSwitch::render") &&
            hook_fn((void*)img.at(kRvaBeginFrame), (void*)hk_beginFrame, (void**)&o_beginFrame,
                    "Renderer::beginFrame") &&
            hook_fn((void*)img.at(kRvaEndFrame), (void*)hk_endFrame, (void**)&o_endFrame, "Renderer::endFrame");
  if (!ok) {
    TB_LOG("stereo: hooks failed; feature disabled");
    return;
  }
  eyestate_start();  // optional: without it sbs falls back to TAA / object velocity off
  pass2_start();
  temporal_start();  // optional: without it SSAO / volumetric history stays shared
  present_on(on_present);
  present_filter(present_gate);
  window_start();
  if (memcmp(reinterpret_cast<const void*>(img.at(kRvaAspect)), kSigAspect, sizeof kSigAspect) == 0 &&
      hook_fn((void*)img.at(kRvaAspect), (void*)hk_aspect, (void**)&o_aspect, "DisplaySettings::aspect"))
    g_aspect_hooked = true;
  else
    TB_LOG("stereo: aspect getter not hooked; hsbs falls back to the camera-only anamorphic override");
  if (!(memcmp(reinterpret_cast<const void*>(img.at(kRvaInvRes)), kSigInvRes, sizeof kSigInvRes) == 0 &&
        hook_fn((void*)img.at(kRvaInvRes), (void*)hk_invres, (void**)&o_invres, "DisplaySettings::invResolution")))
    TB_LOG("stereo: inverse-resolution getter not hooked; pixel-sized UI stays twice as wide in hsbs");
  if (!(memcmp(reinterpret_cast<const void*>(img.at(kRvaVideoQuad)), kSigVideoQuad, sizeof kSigVideoQuad) == 0 &&
        hook_fn((void*)img.at(kRvaVideoQuad), (void*)hk_videoQuad, (void**)&o_videoQuad, "Draw2D::videoQuad")))
    TB_LOG("stereo: video quad not hooked; videos stay letterboxed to half height in hsbs");
  g_ui_aspect_ok = memcmp(reinterpret_cast<const void*>(img.at(kRvaGameAspect)), kSigGameAspect,
                          sizeof kSigGameAspect) == 0;
  if (!g_ui_aspect_ok) TB_LOG("stereo: Game::aspect signature mismatch; the UI aspect is not refreshed per frame");
  g_hooked = true;
  TB_LOG("stereo: hooks installed");
}

}  // namespace tb::stereo
