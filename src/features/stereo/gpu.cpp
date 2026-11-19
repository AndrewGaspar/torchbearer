#include "features/stereo/gpu.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "core/present.h"
#include "features/stereo/cmdlist.h"

namespace tb::stereo {
namespace {

struct Ctx {
  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* alloc[3] = {};
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  uint64_t fence_value = 0;
  uint64_t alloc_fence[3] = {};
  unsigned slot = 0;
  // readback
  ID3D12Resource* readback = nullptr;
  uint64_t readback_size = 0;
  bool failed = false;
};

std::mutex g_mu;
Ctx g;
// While set, barrier()/copy_rect() record into this list (one of the engine's open command lists)
// instead of ours.
ID3D12GraphicsCommandList* g_rec = nullptr;
ID3D12GraphicsCommandList* cur() { return g_rec ? g_rec : g.list; }
unsigned g_captures = 0, g_failures = 0;

bool ensure(IDXGISwapChain* sc) {
  if (g.failed) return false;
  if (g.list) return true;
  g.queue = static_cast<ID3D12CommandQueue*>(present_queue(sc));
  if (!g.queue) {
    TB_LOG("stereo gpu: no D3D12 command queue recorded for swapchain %p", (void*)sc);
    g.failed = true;
    return false;
  }
  if (FAILED(g.queue->GetDevice(__uuidof(ID3D12Device), (void**)&g.dev))) {
    TB_LOG("stereo gpu: queue has no device");
    g.failed = true;
    return false;
  }
  for (auto& a : g.alloc)
    if (FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                             (void**)&a))) {
      g.failed = true;
      return false;
    }
  if (FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                      __uuidof(ID3D12GraphicsCommandList), (void**)&g.list)) ||
      FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g.fence))) {
    TB_LOG("stereo gpu: command list / fence creation failed");
    g.failed = true;
    return false;
  }
  g.list->Close();
  g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  TB_LOG("stereo gpu: ready (queue %p, device %p)", (void*)g.queue, (void*)g.dev);
  return true;
}

// Open the command list on a free allocator.
void begin() {
  g.slot = (g.slot + 1) % 3;
  if (g.fence->GetCompletedValue() < g.alloc_fence[g.slot]) {
    g.fence->SetEventOnCompletion(g.alloc_fence[g.slot], g.event);
    WaitForSingleObject(g.event, 2000);
  }
  g.alloc[g.slot]->Reset();
  g.list->Reset(g.alloc[g.slot], nullptr);
}

uint64_t submit() {
  g.list->Close();
  ID3D12CommandList* l = g.list;
  g.queue->ExecuteCommandLists(1, &l);
  g.queue->Signal(g.fence, ++g.fence_value);
  g.alloc_fence[g.slot] = g.fence_value;
  return g.fence_value;
}

void wait(uint64_t v) {
  if (g.fence->GetCompletedValue() >= v) return;
  g.fence->SetEventOnCompletion(v, g.event);
  WaitForSingleObject(g.event, 5000);
}

void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  cur()->ResourceBarrier(1, &b);
}

ID3D12Resource* back_buffer(IDXGISwapChain* sc) {
  IDXGISwapChain3* sc3 = nullptr;
  if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3) return nullptr;
  UINT idx = sc3->GetCurrentBackBufferIndex();
  ID3D12Resource* bb = nullptr;
  sc3->GetBuffer(idx, __uuidof(ID3D12Resource), (void**)&bb);
  sc3->Release();
  return bb;
}

bool write_bmp(const std::string& path, const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch,
               DXGI_FORMAT fmt) {
  uint32_t row = (w * 3 + 3) & ~3u;
  std::vector<uint8_t> out(54 + size_t(row) * h);
  uint8_t* hd = out.data();
  auto u32 = [&](size_t o, uint32_t v) { memcpy(hd + o, &v, 4); };
  hd[0] = 'B';
  hd[1] = 'M';
  u32(2, uint32_t(out.size()));
  u32(10, 54);
  u32(14, 40);
  u32(18, w);
  u32(22, uint32_t(-int32_t(h)));  // top-down
  hd[26] = 1;
  hd[28] = 24;
  for (uint32_t y = 0; y < h; y++) {
    const uint32_t* s = reinterpret_cast<const uint32_t*>(src + size_t(y) * pitch);
    uint8_t* d = hd + 54 + size_t(y) * row;
    for (uint32_t x = 0; x < w; x++) {
      uint32_t p = s[x];
      uint8_t r, gg, b;
      if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
        r = uint8_t((p & 0x3ff) >> 2);
        gg = uint8_t(((p >> 10) & 0x3ff) >> 2);
        b = uint8_t(((p >> 20) & 0x3ff) >> 2);
      } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        b = uint8_t(p);
        gg = uint8_t(p >> 8);
        r = uint8_t(p >> 16);
      } else {  // R8G8B8A8
        r = uint8_t(p);
        gg = uint8_t(p >> 8);
        b = uint8_t(p >> 16);
      }
      d[3 * x] = b;
      d[3 * x + 1] = gg;
      d[3 * x + 2] = r;
    }
  }
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
  return true;
}

}  // namespace

bool gpu_capture(IDXGISwapChain* sc, const std::string& path) {
  std::lock_guard lk(g_mu);
  if (!ensure(sc)) return false;
  ID3D12Resource* bb = back_buffer(sc);
  if (!bb) {
    g_failures++;
    return false;
  }
  D3D12_RESOURCE_DESC d = bb->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  g.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
  if (!g.readback || g.readback_size < total) {
    if (g.readback) g.readback->Release();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, __uuidof(ID3D12Resource), (void**)&g.readback))) {
      TB_LOG("stereo gpu: readback buffer (%llu bytes) failed", (unsigned long long)total);
      bb->Release();
      g_failures++;
      return false;
    }
    g.readback_size = total;
  }
  begin();
  barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = g.readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = fp;
  src.pResource = bb;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  wait(submit());
  void* p = nullptr;
  D3D12_RANGE r{0, SIZE_T(total)};
  bool ok = false;
  if (SUCCEEDED(g.readback->Map(0, &r, &p))) {
    ok = write_bmp(path, static_cast<uint8_t*>(p) + fp.Offset, fp.Footprint.Width, fp.Footprint.Height,
                   fp.Footprint.RowPitch, d.Format);
    D3D12_RANGE none{0, 0};
    g.readback->Unmap(0, &none);
  }
  bb->Release();
  ok ? g_captures++ : g_failures++;
  return ok;
}

// --- side-by-side compositor --------------------------------------------------------------------
// Eye stash: one texture per eye, the size of one eye (the back buffer's width / width scale),
// kept in COPY_SOURCE between frames (COPY_DEST only while being written).
namespace {
ID3D12Resource* g_eye[2] = {};
uint32_t g_eye_w = 0, g_eye_h = 0;
DXGI_FORMAT g_eye_fmt = DXGI_FORMAT_UNKNOWN;
unsigned g_stored = 0, g_composed = 0, g_compose_fail = 0, g_duplicated = 0;

bool ensure_eyes(const D3D12_RESOURCE_DESC& bb, uint32_t w) {
  if (g_eye[0] && g_eye_w == w && g_eye_h == bb.Height && g_eye_fmt == bb.Format) return true;
  for (auto& e : g_eye)
    if (e) {
      e->Release();
      e = nullptr;
    }
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = bb.Height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = bb.Format;
  d.SampleDesc.Count = 1;
  for (auto& e : g_eye)
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                              __uuidof(ID3D12Resource), (void**)&e))) {
      TB_LOG("stereo gpu: eye texture %ux%u fmt %d failed", w, bb.Height, int(bb.Format));
      return false;
    }
  g_eye_w = w;
  g_eye_h = bb.Height;
  g_eye_fmt = bb.Format;
  TB_LOG("stereo gpu: eye textures %ux%u fmt %d", w, g_eye_h, int(g_eye_fmt));
  return true;
}

// Zero-filled (committed resources start zeroed) eye-sized texture: the black edge strips of a
// horizontally shifted eye.
ID3D12Resource* g_black = nullptr;
bool ensure_black() {
  if (g_black) {
    D3D12_RESOURCE_DESC d = g_black->GetDesc();
    if (d.Width == g_eye_w && d.Height == g_eye_h && d.Format == g_eye_fmt) return true;
    g_black->Release();
    g_black = nullptr;
  }
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = g_eye_w;
  d.Height = g_eye_h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = g_eye_fmt;
  d.SampleDesc.Count = 1;
  return SUCCEEDED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                  nullptr, __uuidof(ID3D12Resource), (void**)&g_black));
}

// Convergence as a horizontal image shift per eye, in pixels (content moves right when > 0).
std::atomic<int> g_hit[2] = {0, 0};

D3D12_TEXTURE_COPY_LOCATION loc(ID3D12Resource* r) {
  D3D12_TEXTURE_COPY_LOCATION l{};
  l.pResource = r;
  l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  l.SubresourceIndex = 0;
  return l;
}

// Copy rect [x0, x0+w) x [0, h) of src to (dx, 0) of dst.
void copy_rect(ID3D12Resource* dst, uint32_t dx, ID3D12Resource* src, uint32_t x0, uint32_t w, uint32_t h) {
  D3D12_TEXTURE_COPY_LOCATION d = loc(dst), s = loc(src);
  D3D12_BOX box{x0, 0, 0, x0 + w, h, 1};
  cur()->CopyTextureRegion(&d, dx, 0, 0, &s, &box);
}
// Eye texture `src` (w x h) into dst at x0, its content shifted by `shift` columns; the uncovered
// columns are black.
void place_eye(ID3D12Resource* dst, uint32_t x0, ID3D12Resource* src, uint32_t w, uint32_t h, int shift) {
  if (shift == 0 || !ensure_black()) {
    copy_rect(dst, x0, src, 0, w, h);
    return;
  }
  uint32_t s = uint32_t(shift < 0 ? -shift : shift);
  if (s >= w) s = w - 1;
  if (shift > 0) {
    copy_rect(dst, x0 + s, src, 0, w - s, h);
    copy_rect(dst, x0, g_black, 0, s, h);
  } else {
    copy_rect(dst, x0, src, s, w - s, h);
    copy_rect(dst, x0 + w - s, g_black, 0, s, h);
  }
}
}  // namespace

void gpu_set_hit(int left_half_px, int right_half_px) {
  g_hit[0] = left_half_px;
  g_hit[1] = right_half_px;
}

bool gpu_store_eye(IDXGISwapChain* sc, int eye) {
  std::lock_guard lk(g_mu);
  if (!ensure(sc)) return false;
  ID3D12Resource* bb = back_buffer(sc);
  if (!bb) return false;
  D3D12_RESOURCE_DESC d = bb->GetDesc();
  unsigned k = present_width_scale(sc);
  uint32_t w = uint32_t(d.Width) / k;
  if (!ensure_eyes(d, w)) {
    bb->Release();
    return false;
  }
  begin();
  barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(g_eye[eye], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_rect(g_eye[eye], 0, bb, 0, w, d.Height);
  barrier(g_eye[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  submit();
  bb->Release();
  g_stored++;
  return true;
}

// Full SBS (back buffer = 2 eyes wide): left eye stash -> [0, w), the frame just rendered (the right
// eye, drawn by the engine into [0, w)) -> [w, 2w). Both 1:1 copies: each eye keeps the engine's
// own 16:9 frustum and resolution. Half SBS (back buffer = 1 eye wide) would need a 2:1 filtered
// squeeze (a shader pass); this prototype instead shows the centre half of each eye (a crop, so
// geometry is right but each half shows only the middle 50% of the eye's field of view).
bool gpu_compose(IDXGISwapChain* sc) {
  std::lock_guard lk(g_mu);
  if (!ensure(sc) || !g_eye[0]) return false;
  ID3D12Resource* bb = back_buffer(sc);
  if (!bb) return false;
  D3D12_RESOURCE_DESC d = bb->GetDesc();
  unsigned k = present_width_scale(sc);
  uint32_t w = uint32_t(d.Width) / k, h = d.Height;
  if (w != g_eye_w || h != g_eye_h) {
    bb->Release();
    g_compose_fail++;
    return false;
  }
  begin();
  // right eye out of the back buffer first
  barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(g_eye[1], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_rect(g_eye[1], 0, bb, 0, w, h);
  barrier(g_eye[1], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  if (k >= 2) {
    place_eye(bb, 0, g_eye[0], w, h, g_hit[0]);
    place_eye(bb, w, g_eye[1], w, h, g_hit[1]);
  } else {
    copy_rect(bb, 0, g_eye[0], w / 4, w / 2, h);
    copy_rect(bb, w / 2, g_eye[1], w / 4, w / 2, h);
  }
  barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  submit();
  bb->Release();
  g_composed++;
  return true;
}

bool gpu_duplicate(IDXGISwapChain* sc) {
  std::lock_guard lk(g_mu);
  if (!ensure(sc)) return false;
  ID3D12Resource* bb = back_buffer(sc);
  if (!bb) return false;
  D3D12_RESOURCE_DESC d = bb->GetDesc();
  unsigned k = present_width_scale(sc);
  uint32_t w = uint32_t(d.Width) / k, h = d.Height;
  if (k < 2 || !ensure_eyes(d, w)) {
    bb->Release();
    return false;
  }
  // Through an eye texture: a resource can't be copy source and destination at once.
  begin();
  barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(g_eye[1], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_rect(g_eye[1], 0, bb, 0, w, h);
  barrier(g_eye[1], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  for (unsigned i = 1; i < k; i++) copy_rect(bb, w * i, g_eye[1], 0, w, h);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  submit();
  bb->Release();
  g_duplicated++;
  return true;
}

// --- both eyes inside one engine frame (sbs_path = view) --------------------------------------
// The copies happen in the middle of the engine's frame. The engine records a frame into a pool
// of command lists and submits whatever is closed at a few points (once inside renderFrame,
// twice in endFrame; live census), so the list that wrote the eye's final image (the one that
// bound the back buffer) may still be open when renderFrame returns, and a list of ours submitted
// then would run before it. So when that list is open the copy is recorded into it, right after
// the engine's own commands (GPU order = recording order); otherwise into our list, submitted now.
// The back buffer is in whatever state the engine's lists left it in (tracked from their
// ResourceBarrier calls by cmdlist.cpp); each copy transitions from and back to that state.
namespace {
unsigned g_mid_engine = 0, g_mid_own = 0;
void mid_begin() {
  g_rec = cmdlist_bb_list();
  if (g_rec) {
    g_mid_engine++;
  } else {
    g_mid_own++;
    begin();
  }
}
void mid_end() {
  if (!g_rec) submit();
  g_rec = nullptr;
}
}  // namespace
bool gpu_stash_mid(IDXGISwapChain* sc, int eye) {
  std::lock_guard lk(g_mu);
  if (!ensure(sc)) return false;
  ID3D12Resource* bb = back_buffer(sc);
  if (!bb) return false;
  D3D12_RESOURCE_DESC d = bb->GetDesc();
  unsigned k = present_width_scale(sc);
  uint32_t w = uint32_t(d.Width) / k;
  if (!ensure_eyes(d, w)) {
    bb->Release();
    return false;
  }
  auto st = D3D12_RESOURCE_STATES(cmdlist_bb_state(bb));
  mid_begin();
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) barrier(bb, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(g_eye[eye], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_rect(g_eye[eye], 0, bb, 0, w, d.Height);
  barrier(g_eye[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
  mid_end();
  bb->Release();
  g_stored++;
  return true;
}

// The second eye (the engine's image, in the first eye-width columns) goes to the second half
// and the stashed first eye back to the first half, before any overlay is drawn.
bool gpu_place_mid(IDXGISwapChain* sc, int stashed_eye) {
  std::lock_guard lk(g_mu);
  if (!ensure(sc) || !g_eye[0] || !g_eye[1]) return false;
  ID3D12Resource* bb = back_buffer(sc);
  if (!bb) return false;
  D3D12_RESOURCE_DESC d = bb->GetDesc();
  unsigned k = present_width_scale(sc);
  uint32_t w = uint32_t(d.Width) / k, h = d.Height;
  if (k < 2 || w != g_eye_w || h != g_eye_h) {
    bb->Release();
    g_compose_fail++;
    return false;
  }
  int other = 1 - stashed_eye;
  auto st = D3D12_RESOURCE_STATES(cmdlist_bb_state(bb));
  mid_begin();
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) barrier(bb, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(g_eye[other], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_rect(g_eye[other], 0, bb, 0, w, h);
  barrier(g_eye[other], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  place_eye(bb, 0, g_eye[0], w, h, g_hit[0]);  // left eye -> first half
  place_eye(bb, w, g_eye[1], w, h, g_hit[1]);  // right eye -> second half
  if (st != D3D12_RESOURCE_STATE_COPY_DEST) barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, st);
  mid_end();
  bb->Release();
  g_composed++;
  return true;
}

bool gpu_compose_view(IDXGISwapChain* sc, int stashed_eye) {
  std::lock_guard lk(g_mu);
  if (present_width_scale(sc) >= 2) return true;  // composed in the frame (gpu_place_mid)
  // Half output: the frame holds the other eye; centre crops of both, as gpu_compose.
  if (!ensure(sc) || !g_eye[0]) return false;
  ID3D12Resource* bb = back_buffer(sc);
  if (!bb) return false;
  D3D12_RESOURCE_DESC d = bb->GetDesc();
  uint32_t w = uint32_t(d.Width), h = d.Height;
  if (w != g_eye_w || h != g_eye_h) {
    bb->Release();
    g_compose_fail++;
    return false;
  }
  int other = 1 - stashed_eye;
  begin();
  barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(g_eye[other], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_rect(g_eye[other], 0, bb, 0, w, h);
  barrier(g_eye[other], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_rect(bb, 0, g_eye[0], w / 4, w / 2, h);
  copy_rect(bb, w / 2, g_eye[1], w / 4, w / 2, h);
  barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  submit();
  bb->Release();
  g_composed++;
  return true;
}

uint32_t gpu_eye_width() { return g_eye_w; }

std::string gpu_status() {
  char b[256];
  snprintf(b, sizeof b,
           "captures=%u failures=%u stored=%u composed=%u compose_fail=%u duplicated=%u eye=%ux%u mid_in_engine_list=%u "
           "mid_own=%u hit=%d/%d",
           g_captures, g_failures, g_stored, g_composed, g_compose_fail, g_duplicated, g_eye_w, g_eye_h, g_mid_engine,
           g_mid_own, g_hit[0].load(), g_hit[1].load());
  return b;
}

}  // namespace tb::stereo
