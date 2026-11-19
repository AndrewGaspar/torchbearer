#pragma once
#include <cstddef>

namespace tb {

// Inline hooks (MinHook) for game and system functions. Returns false (and logs) on failure.
bool hook_init();
bool hook_fn(void* target, void* detour, void** original, const char* name);
bool hook_enable_all();

// Between hook_batch_begin and hook_batch_end, hook_fn only queues: enabling a MinHook hook
// suspends every thread, ~12 ms each under Proton, so start-up enables them all in one go.
void hook_batch_begin();
void hook_batch_end();

// Patch one slot of a COM object's vtable; returns the previous slot value (the original), or
// the existing original if the slot was already patched by us.
void* hook_vtable(void* obj, size_t index, void* detour);

}  // namespace tb
