#include "core/hook.h"

#include <windows.h>

#include <map>
#include <mutex>

#include "MinHook.h"
#include "core/log.h"

namespace tb {
namespace {
std::mutex g_mu;
std::map<void**, void*> g_vt_orig;  // patched slot -> original
int g_batch = 0;
int g_queued = 0;
}  // namespace

bool hook_init() {
  MH_STATUS s = MH_Initialize();
  if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
    TB_LOG("MinHook init failed: %s", MH_StatusToString(s));
    return false;
  }
  return true;
}

bool hook_fn(void* target, void* detour, void** original, const char* name) {
  MH_STATUS s = MH_CreateHook(target, detour, original);
  if (s != MH_OK) {
    TB_LOG("hook %s @%p: create failed: %s", name, target, MH_StatusToString(s));
    return false;
  }
  {
    std::lock_guard lk(g_mu);
    if (g_batch > 0) {
      s = MH_QueueEnableHook(target);
      if (s == MH_OK) g_queued++;
    } else {
      s = MH_EnableHook(target);
    }
  }
  if (s != MH_OK) {
    TB_LOG("hook %s @%p: enable failed: %s", name, target, MH_StatusToString(s));
    return false;
  }
  TB_LOG("hooked %s @%p", name, target);
  return true;
}

void hook_batch_begin() {
  std::lock_guard lk(g_mu);
  g_batch++;
}

void hook_batch_end() {
  std::lock_guard lk(g_mu);
  if (--g_batch > 0 || g_queued == 0) return;
  MH_STATUS s = MH_ApplyQueued();
  TB_LOG("enabled %d queued hooks: %s", g_queued, MH_StatusToString(s));
  g_queued = 0;
}

bool hook_enable_all() { return MH_EnableHook(MH_ALL_HOOKS) == MH_OK; }

void* hook_vtable(void* obj, size_t index, void* detour) {
  void** slot = *reinterpret_cast<void***>(obj) + index;
  std::lock_guard lk(g_mu);
  auto it = g_vt_orig.find(slot);
  if (it != g_vt_orig.end()) return it->second;
  void* orig = *slot;
  if (orig == detour) return orig;
  DWORD old;
  VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
  *slot = detour;
  VirtualProtect(slot, sizeof(void*), old, &old);
  g_vt_orig[slot] = orig;
  return orig;
}

}  // namespace tb
