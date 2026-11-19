#include "features/stereo/cmdlist.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/hook.h"
#include "core/log.h"
#include "core/present.h"

namespace tb::stereo {
namespace {

// ID3D12Device / ID3D12GraphicsCommandList vtable slots (d3d12.h declaration order).
constexpr size_t kDevCreateRenderTargetView = 20;
constexpr size_t kClClose = 9;
constexpr size_t kClReset = 10;
constexpr size_t kClDrawInstanced = 12;
constexpr size_t kClDrawIndexedInstanced = 13;
constexpr size_t kClDispatch = 14;
constexpr size_t kClCopyTextureRegion = 16;
constexpr size_t kClCopyResource = 17;
constexpr size_t kClRSSetViewports = 21;
constexpr size_t kClRSSetScissorRects = 22;
constexpr size_t kClResourceBarrier = 26;
constexpr size_t kClOMSetRenderTargets = 46;
constexpr size_t kClClearRenderTargetView = 48;

using CreateRTV_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
using Close_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using Dispatch_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using CopyTextureRegion_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT,
                                                     UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyResource_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using RSSetViewports_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using RSSetScissorRects_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using ResourceBarrier_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                      BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearRTV_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT,
                                            const D3D12_RECT*);

CreateRTV_t o_createRTV;
Close_t o_close;
Reset_t o_reset;
DrawInstanced_t o_drawInstanced;
DrawIndexedInstanced_t o_drawIndexedInstanced;
Dispatch_t o_dispatch;
CopyTextureRegion_t o_copyTextureRegion;
CopyResource_t o_copyResource;
RSSetViewports_t o_rsSetViewports;
RSSetScissorRects_t o_rsSetScissorRects;
ResourceBarrier_t o_resourceBarrier;
OMSetRenderTargets_t o_omSetRenderTargets;
ClearRTV_t o_clearRTV;

std::mutex g_mu;
std::vector<ID3D12Resource*> g_bbs;                     // current swapchain buffers (not AddRef'd)
std::unordered_map<SIZE_T, int> g_rtv;                  // RTV handle -> back buffer index
std::unordered_map<ID3D12Resource*, unsigned> g_state;  // record-time state of each back buffer
UINT g_rtv_inc = 0;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_trace{false};
std::atomic<bool> g_track{false};
std::atomic<uint32_t> g_dx{0};
std::atomic<unsigned long> g_tid{0};
std::atomic<uint64_t> g_dup_draws{0}, g_dup_lists{0};
ID3D12GraphicsCommandList* g_bb_list = nullptr;  // open list that last bound a back buffer (under g_mu)

// Per command list (only maintained while tracing or duplicating).
struct ListState {
  int bb = -1;  // back buffer index bound as render target 0..7, or -1
  // The bound targets, as passed (only kept while a back buffer is bound).
  UINT nrt = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE rt[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
  bool has_ds = false;
  D3D12_CPU_DESCRIPTOR_HANDLE ds{};
  UINT nvp = 0, nsc = 0;
  D3D12_VIEWPORT vp[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  D3D12_RECT sc[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  unsigned draws = 0, bb_draws = 0, dispatches = 0, dups = 0;
};
std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_lists;

bool active() { return g_trace.load(std::memory_order_relaxed) || g_track.load(std::memory_order_relaxed); }

int bb_index(ID3D12Resource* r) {
  for (size_t i = 0; i < g_bbs.size(); i++)
    if (g_bbs[i] == r) return int(i);
  return -1;
}

void STDMETHODCALLTYPE hk_createRTV(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE h) {
  o_createRTV(d, r, desc, h);
  std::lock_guard lk(g_mu);
  int i = r ? bb_index(r) : -1;
  if (i >= 0) {
    if (g_rtv.size() < 64) TB_LOG("stereo cl: RTV %#llx on back buffer %d", (unsigned long long)h.ptr, i);
    g_rtv[h.ptr] = i;
  } else {
    g_rtv.erase(h.ptr);  // a descriptor slot reused for something else
  }
}

HRESULT STDMETHODCALLTYPE hk_reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
  if (active()) {
    std::lock_guard lk(g_mu);
    g_lists.erase(l);
    if (g_trace) TB_LOG("stereo cl: reset list=%p tid=%lu", (void*)l, GetCurrentThreadId());
  }
  return o_reset(l, a, p);
}

HRESULT STDMETHODCALLTYPE hk_close(ID3D12GraphicsCommandList* l) {
  if (active()) {
    std::lock_guard lk(g_mu);
    if (g_bb_list == l) g_bb_list = nullptr;
    auto it = g_lists.find(l);
    if (it != g_lists.end()) {
      auto& s = it->second;
      if (g_trace && (s.draws || s.dispatches))
        TB_LOG("stereo cl: close list=%p tid=%lu draws=%u bb_draws=%u dispatches=%u dups=%u", (void*)l,
               GetCurrentThreadId(), s.draws, s.bb_draws, s.dispatches, s.dups);
      if (s.dups) g_dup_lists++;
      g_lists.erase(it);
    }
  }
  return o_close(l);
}

void STDMETHODCALLTYPE hk_omSetRenderTargets(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* h,
                                             BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
  o_omSetRenderTargets(l, n, h, single, ds);
  if (!active()) return;
  std::lock_guard lk(g_mu);
  int bb = -1;
  for (UINT i = 0; i < n && h && bb < 0; i++) {
    SIZE_T p = single ? h[0].ptr + SIZE_T(i) * g_rtv_inc : h[i].ptr;
    auto it = g_rtv.find(p);
    if (it != g_rtv.end()) bb = it->second;
  }
  auto& s = g_lists[l];
  if (g_trace && (bb != s.bb || bb >= 0))
    TB_LOG("stereo cl: list=%p tid=%lu OMSetRenderTargets n=%u rtv0=%#llx ds=%#llx -> bb=%d", (void*)l,
           GetCurrentThreadId(), n, (unsigned long long)(n && h ? h[0].ptr : 0), (unsigned long long)(ds ? ds->ptr : 0),
           bb);
  s.bb = bb;
  if (bb >= 0) {
    s.nrt = n < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ? n : D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
    for (UINT i = 0; i < s.nrt; i++) s.rt[i].ptr = single ? h[0].ptr + SIZE_T(i) * g_rtv_inc : h[i].ptr;
    s.has_ds = ds != nullptr;
    if (ds) s.ds = *ds;
  }
  if (bb >= 0) g_bb_list = l;
}

void STDMETHODCALLTYPE hk_rsSetViewports(ID3D12GraphicsCommandList* l, UINT n, const D3D12_VIEWPORT* v) {
  o_rsSetViewports(l, n, v);
  if (!active() || !v) return;
  std::lock_guard lk(g_mu);
  auto& s = g_lists[l];
  s.nvp = n < D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ? n : D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  memcpy(s.vp, v, sizeof(*v) * s.nvp);
}

void STDMETHODCALLTYPE hk_rsSetScissorRects(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RECT* r) {
  o_rsSetScissorRects(l, n, r);
  if (!active() || !r) return;
  std::lock_guard lk(g_mu);
  auto& s = g_lists[l];
  s.nsc = n < D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ? n : D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  memcpy(s.sc, r, sizeof(*r) * s.nsc);
}

// Called after the original draw. Returns true if the caller should draw again (state shifted).
// On true, `restore` gets the list state to put back after the second draw.
bool before_dup(ID3D12GraphicsCommandList* l, ListState& restore) {
  std::lock_guard lk(g_mu);
  auto it = g_lists.find(l);
  if (it == g_lists.end()) return false;
  auto& s = it->second;
  s.draws++;
  if (s.bb < 0) return false;
  s.bb_draws++;
  uint32_t dx = g_dx.load(std::memory_order_relaxed);
  if (!dx || GetCurrentThreadId() != g_tid.load(std::memory_order_relaxed) || !s.nvp) return false;
  restore = s;
  s.dups++;
  return true;
}

void shift_and(ID3D12GraphicsCommandList* l, const ListState& s, uint32_t dx) {
  // Rasterization is limited to the smallest bound target: the engine's depth buffer is one eye
  // wide, so the duplicate is drawn without it (overlays don't depth-test against the scene).
  if (s.has_ds) o_omSetRenderTargets(l, s.nrt, s.rt, FALSE, nullptr);
  D3D12_VIEWPORT vp[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  D3D12_RECT sc[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  for (UINT i = 0; i < s.nvp; i++) {
    vp[i] = s.vp[i];
    vp[i].TopLeftX += float(dx);
  }
  o_rsSetViewports(l, s.nvp, vp);
  if (s.nsc) {
    for (UINT i = 0; i < s.nsc; i++) {
      sc[i] = s.sc[i];
      sc[i].left += LONG(dx);
      sc[i].right += LONG(dx);
    }
    o_rsSetScissorRects(l, s.nsc, sc);
  }
}
void unshift(ID3D12GraphicsCommandList* l, const ListState& s) {
  if (s.has_ds) o_omSetRenderTargets(l, s.nrt, s.rt, FALSE, &s.ds);
  o_rsSetViewports(l, s.nvp, s.vp);
  if (s.nsc) o_rsSetScissorRects(l, s.nsc, s.sc);
}

void STDMETHODCALLTYPE hk_drawInstanced(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, UINT d) {
  o_drawInstanced(l, a, b, c, d);
  if (!active()) return;
  ListState s;
  if (!before_dup(l, s)) return;
  shift_and(l, s, g_dx);
  o_drawInstanced(l, a, b, c, d);
  unshift(l, s);
  g_dup_draws++;
}

void STDMETHODCALLTYPE hk_drawIndexedInstanced(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, INT d, UINT e) {
  o_drawIndexedInstanced(l, a, b, c, d, e);
  if (!active()) return;
  ListState s;
  if (!before_dup(l, s)) return;
  shift_and(l, s, g_dx);
  o_drawIndexedInstanced(l, a, b, c, d, e);
  unshift(l, s);
  g_dup_draws++;
}

void STDMETHODCALLTYPE hk_dispatch(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z) {
  o_dispatch(l, x, y, z);
  if (!g_trace.load(std::memory_order_relaxed)) return;
  std::lock_guard lk(g_mu);
  g_lists[l].dispatches++;
}

void STDMETHODCALLTYPE hk_copyTextureRegion(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x,
                                            UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* box) {
  o_copyTextureRegion(l, d, x, y, z, s, box);
  if (!g_trace.load(std::memory_order_relaxed)) return;
  std::lock_guard lk(g_mu);
  int di = d ? bb_index(d->pResource) : -1, si = s ? bb_index(s->pResource) : -1;
  if (di >= 0 || si >= 0)
    TB_LOG("stereo cl: list=%p tid=%lu CopyTextureRegion dst=%p(bb%d) at %u,%u src=%p(bb%d)", (void*)l,
           GetCurrentThreadId(), d ? (void*)d->pResource : nullptr, di, x, y, s ? (void*)s->pResource : nullptr, si);
}

void STDMETHODCALLTYPE hk_copyResource(ID3D12GraphicsCommandList* l, ID3D12Resource* d, ID3D12Resource* s) {
  o_copyResource(l, d, s);
  if (!g_trace.load(std::memory_order_relaxed)) return;
  std::lock_guard lk(g_mu);
  int di = bb_index(d), si = bb_index(s);
  if (di >= 0 || si >= 0)
    TB_LOG("stereo cl: list=%p tid=%lu CopyResource dst=%p(bb%d) src=%p(bb%d)", (void*)l, GetCurrentThreadId(), (void*)d,
           di, (void*)s, si);
}

void STDMETHODCALLTYPE hk_resourceBarrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
  o_resourceBarrier(l, n, b);
  if (!g_installed.load(std::memory_order_relaxed) || !b) return;
  // Back-buffer state tracking (cheap: a pointer compare per barrier against 2-4 buffers).
  std::lock_guard lk(g_mu);
  for (UINT i = 0; i < n; i++) {
    if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
    int bi = bb_index(b[i].Transition.pResource);
    if (bi < 0) continue;
    g_state[b[i].Transition.pResource] = b[i].Transition.StateAfter;
    if (g_trace)
      TB_LOG("stereo cl: list=%p tid=%lu barrier bb%d %#x -> %#x", (void*)l, GetCurrentThreadId(), bi,
             b[i].Transition.StateBefore, b[i].Transition.StateAfter);
  }
}

void STDMETHODCALLTYPE hk_clearRTV(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* c, UINT n,
                                   const D3D12_RECT* r) {
  o_clearRTV(l, h, c, n, r);
  if (!g_trace.load(std::memory_order_relaxed)) return;
  std::lock_guard lk(g_mu);
  auto it = g_rtv.find(h.ptr);
  if (it != g_rtv.end())
    TB_LOG("stereo cl: list=%p tid=%lu ClearRenderTargetView bb%d rects=%u", (void*)l, GetCurrentThreadId(), it->second,
           n);
}

}  // namespace

void cmdlist_on_swapchain(IDXGISwapChain* sc) {
  auto* q = static_cast<ID3D12CommandQueue*>(present_queue(sc));
  IDXGISwapChain3* sc3 = nullptr;
  if (!q || FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3) return;
  DXGI_SWAP_CHAIN_DESC d{};
  sc3->GetDesc(&d);
  std::vector<ID3D12Resource*> bbs;
  for (UINT i = 0; i < d.BufferCount && i < 8; i++) {
    ID3D12Resource* r = nullptr;
    if (SUCCEEDED(sc3->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&r)) && r) {
      bbs.push_back(r);
      r->Release();  // the swapchain keeps it alive
    }
  }
  sc3->Release();
  ID3D12Device* dev = nullptr;
  if (FAILED(q->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return;
  {
    std::lock_guard lk(g_mu);
    g_bbs = bbs;
    g_rtv.clear();
    g_state.clear();
    g_rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  }
  if (!o_createRTV) {
    o_createRTV = reinterpret_cast<CreateRTV_t>(hook_vtable(dev, kDevCreateRenderTargetView, (void*)hk_createRTV));
  }
  dev->Release();
  TB_LOG("stereo cl: swapchain %p has %zu back buffers", (void*)sc, bbs.size());
}

bool cmdlist_install(IDXGISwapChain* sc) {
  if (g_installed) return true;
  auto* q = static_cast<ID3D12CommandQueue*>(present_queue(sc));
  ID3D12Device* dev = nullptr;
  if (!q || FAILED(q->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return false;
  ID3D12CommandAllocator* a = nullptr;
  ID3D12GraphicsCommandList* l = nullptr;
  bool ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                  (void**)&a)) &&
            SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), (void**)&l));
  if (ok) {
    o_close = reinterpret_cast<Close_t>(hook_vtable(l, kClClose, (void*)hk_close));
    o_reset = reinterpret_cast<Reset_t>(hook_vtable(l, kClReset, (void*)hk_reset));
    o_drawInstanced = reinterpret_cast<DrawInstanced_t>(hook_vtable(l, kClDrawInstanced, (void*)hk_drawInstanced));
    o_drawIndexedInstanced =
        reinterpret_cast<DrawIndexedInstanced_t>(hook_vtable(l, kClDrawIndexedInstanced, (void*)hk_drawIndexedInstanced));
    o_dispatch = reinterpret_cast<Dispatch_t>(hook_vtable(l, kClDispatch, (void*)hk_dispatch));
    o_copyTextureRegion =
        reinterpret_cast<CopyTextureRegion_t>(hook_vtable(l, kClCopyTextureRegion, (void*)hk_copyTextureRegion));
    o_copyResource = reinterpret_cast<CopyResource_t>(hook_vtable(l, kClCopyResource, (void*)hk_copyResource));
    o_rsSetViewports = reinterpret_cast<RSSetViewports_t>(hook_vtable(l, kClRSSetViewports, (void*)hk_rsSetViewports));
    o_rsSetScissorRects =
        reinterpret_cast<RSSetScissorRects_t>(hook_vtable(l, kClRSSetScissorRects, (void*)hk_rsSetScissorRects));
    o_resourceBarrier = reinterpret_cast<ResourceBarrier_t>(hook_vtable(l, kClResourceBarrier, (void*)hk_resourceBarrier));
    o_omSetRenderTargets =
        reinterpret_cast<OMSetRenderTargets_t>(hook_vtable(l, kClOMSetRenderTargets, (void*)hk_omSetRenderTargets));
    o_clearRTV = reinterpret_cast<ClearRTV_t>(hook_vtable(l, kClClearRenderTargetView, (void*)hk_clearRTV));
    l->Close();
    g_installed = true;
    TB_LOG("stereo cl: command-list hooks installed");
  } else {
    TB_LOG("stereo cl: could not create a command list to hook");
  }
  if (l) l->Release();
  if (a) a->Release();
  dev->Release();
  return ok;
}

bool cmdlist_installed() { return g_installed; }

void cmdlist_track(bool on) {
  g_track = on;
  if (!on && !g_trace) {
    std::lock_guard lk(g_mu);
    g_lists.clear();
  }
}

void cmdlist_trace(bool on) {
  g_trace = on;
  if (!on && !g_track) {
    std::lock_guard lk(g_mu);
    g_lists.clear();
  }
}

void cmdlist_duplicate(unsigned long tid, uint32_t dx) {
  g_tid = tid;
  g_dx = dx;
}

ID3D12GraphicsCommandList* cmdlist_bb_list() {
  std::lock_guard lk(g_mu);
  return g_bb_list;
}

unsigned cmdlist_bb_state(ID3D12Resource* bb) {
  std::lock_guard lk(g_mu);
  auto it = g_state.find(bb);
  return it == g_state.end() ? 0u : it->second;
}

std::string cmdlist_status() {
  char b[160];
  std::lock_guard lk(g_mu);
  snprintf(b, sizeof b, "installed=%d bbs=%zu rtvs=%zu dup_draws=%llu dup_lists=%llu", g_installed.load() ? 1 : 0,
           g_bbs.size(), g_rtv.size(), (unsigned long long)g_dup_draws.load(), (unsigned long long)g_dup_lists.load());
  return b;
}

}  // namespace tb::stereo
