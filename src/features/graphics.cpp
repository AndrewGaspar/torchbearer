// [graphics] options (graphics.h). docs/raytracing.md has how each address was established.
#include "features/graphics.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "features/rt/rt.h"

namespace tb {
namespace {

// Main pass 0x506b20 (static, Ghidra): the SSAO render call and, after the shadow-buffer pass, its
// apply are each guarded by `cmp byte [Render SSAO 0xd755e0], ...; jz +0x0c`; the HBAO ones by the
// same test of "Render HBAO" 0x955659 (default 1):
//   0x5070e6  44 38 2d f3 e4 86 00   cmp [0xd755e0], r13b     0x5070ed  74 0c  (render 0x536630)
//   0x50713e  80 3d 9b e4 86 00 00   cmp [0xd755e0], 0        0x507145  74 0c  (apply 0x536580)
//   0x5070fb  44 38 2d 57 e5 44 00   cmp [0x955659], r13b     0x507102  74 0c  (render 0x538160)
//   0x507153  80 3d ff e4 44 00 00   cmp [0x955659], 0        0x50715a  74 0c  (apply 0x537ef0)
// Forcing a branch rewrites its jz to 90 90 (always run) or eb 0c (always skip), as one 16-bit
// store, at a frame boundary (present, game thread) so render and apply always agree.
struct Branch {
  uintptr_t jz;
  uint8_t sig[7];
};
const Branch kSsao[2] = {{0x5070ed, {0x44, 0x38, 0x2d, 0xf3, 0xe4, 0x86, 0x00}},
                         {0x507145, {0x80, 0x3d, 0x9b, 0xe4, 0x86, 0x00, 0x00}}};
const Branch kHbao[2] = {{0x507102, {0x44, 0x38, 0x2d, 0x57, 0xe5, 0x44, 0x00}},
                         {0x50715a, {0x80, 0x3d, 0xff, 0xe4, 0x44, 0x00, 0x00}}};
constexpr uint16_t kJz = 0x0c74, kNops = 0x9090, kJmp = 0x0ceb;

bool sig_ok(const Branch (&br)[2]) {
  for (auto& b : br) {
    auto* p = reinterpret_cast<const uint8_t*>(game_image().at(b.jz - 7));
    uint16_t cur;
    memcpy(&cur, p + 7, 2);
    if (memcmp(p, b.sig, 7) || (cur != kJz && cur != kNops && cur != kJmp)) return false;
  }
  return true;
}

void write(const Branch (&br)[2], uint16_t v) {
  for (auto& b : br) {
    auto* p = reinterpret_cast<uint16_t*>(game_image().at(b.jz));
    if (*p == v) continue;
    DWORD old;
    VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old);
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);  // one store: never a half-patched jz
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
  }
}

bool g_ok_ssao = false, g_ok_hbao = false;
std::atomic<bool> g_ssao_cfg{false};
// [graphics] ao: -1 unset (then the older `ssao` key decides), 0 game, 1 ssao, 2 rtao.
std::atomic<int> g_ao_cfg{-1};
int ao_mode() {
  int a = g_ao_cfg.load();
  return a >= 0 ? a : g_ssao_cfg.load() ? 1 : 0;
}
// Wanted branch modes (-1 game, 1 run, 0 skip), applied on the game thread at present.
std::atomic<int> g_dev_ssao{-2}, g_dev_hbao{-2};  // dev overrides from rt flag (-2: none)
int g_have_ssao = -1, g_have_hbao = -1;

uint16_t code(int mode) { return mode < 0 ? kJz : mode ? kNops : kJmp; }

void on_present(IDXGISwapChain*, uint64_t) {
  int mode = ao_mode();
  // RTAO (rt/scene.cpp, rt/trace.cpp) replaces HBAO's result just before HBAO's apply, so the
  // game's branches stay as they are.
  static int last_mode = -1;  // on changes only: `rt scene on/off` (lab) may switch it meanwhile
  if (mode != last_mode) rt::set_rtao(mode == 2);
  last_mode = mode;
  int ws = g_dev_ssao.load() != -2 ? g_dev_ssao.load() : (mode == 1 ? 1 : -1);
  if (g_ok_ssao && ws != g_have_ssao) {
    write(kSsao, code(ws));
    g_have_ssao = ws;
    TB_LOG("graphics: SSAO branch %s", ws < 0 ? "the game's" : ws ? "forced on" : "forced off");
  }
  // HBAO's apply overwrites the shadow buffer's AO channel after SSAO's (measured), so SSAO only
  // shows with HBAO skipped: the option replaces one with the other.
  int wh = g_dev_hbao.load() != -2 ? g_dev_hbao.load() : (mode == 1 ? 0 : -1);
  if (g_ok_hbao && wh != g_have_hbao) {
    write(kHbao, code(wh));
    g_have_hbao = wh;
    TB_LOG("graphics: HBAO branch %s", wh < 0 ? "the game's" : wh ? "forced on" : "forced off");
  }
}

}  // namespace

void graphics_register() {
  cfg_listen("graphics.", [](const std::string& id, const std::string& v) {
    if (id == "graphics.ssao") g_ssao_cfg = (v == "1" || v == "on" || v == "true");
    if (id == "graphics.ao") {
      g_ao_cfg = v == "game" ? 0 : v == "ssao" ? 1 : v == "rtao" ? 2 : -1;
      if (!v.empty() && g_ao_cfg.load() < 0) TB_LOG("graphics: unknown ao = %s (game, ssao or rtao)", v.c_str());
    }
  });
}

void graphics_start() {
  g_ok_ssao = sig_ok(kSsao);
  g_ok_hbao = sig_ok(kHbao);
  if (!g_ok_ssao) TB_LOG("graphics: SSAO branch signature mismatch; [graphics] ssao is off");
  if (!g_ok_hbao) TB_LOG("graphics: HBAO branch signature mismatch");
  present_on(on_present);
}

bool graphics_ssao_on() {
  return g_ok_ssao && g_have_ssao == 1;
}

bool graphics_branch(const std::string& which, int mode) {
  if (which == "ssao") {
    if (!g_ok_ssao) return false;
    g_dev_ssao = mode < 0 ? -2 : mode;  // "game" hands the branch back to the option
    return true;
  }
  if (which == "hbao") {
    if (!g_ok_hbao) return false;
    g_dev_hbao = mode < 0 ? -2 : mode;
    return true;
  }
  return false;
}

std::string graphics_branch_state(const std::string& which) {
  int h = which == "ssao" ? g_have_ssao : g_have_hbao;
  return h < 0 ? "game" : h ? "forced on" : "forced off";
}

}  // namespace tb
