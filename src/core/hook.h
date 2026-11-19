#pragma once
#include <cstddef>

namespace tb {

// Inline hooks (MinHook) for game and system functions. Returns false (and logs) on failure.
bool hook_init();
bool hook_fn(void* target, void* detour, void** original, const char* name);
bool hook_enable_all();

// Patch one slot of a COM object's vtable; returns the previous slot value (the original), or
// the existing original if the slot was already patched by us.
void* hook_vtable(void* obj, size_t index, void* detour);

}  // namespace tb
