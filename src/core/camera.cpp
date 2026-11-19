#include "core/camera.h"

#include <atomic>
#include <cstring>

namespace tb {
namespace {

struct Filter {
  int order;
  CameraFilter fn;
};
constexpr int kMax = 8;
Filter g_filters[kMax];
int g_nfilters = 0;
CameraPreRender g_pre[kMax];
int g_npre = 0;
std::atomic<bool> g_hooked{false};

}  // namespace

void camera_filter_add(int order, CameraFilter f) {
  if (g_nfilters >= kMax || !f) return;
  int i = g_nfilters++;
  while (i > 0 && g_filters[i - 1].order > order) {
    g_filters[i] = g_filters[i - 1];
    i--;
  }
  g_filters[i] = {order, f};
}

void camera_prerender_add(CameraPreRender f) {
  if (g_npre < kMax && f) g_pre[g_npre++] = f;
}

bool camera_filter_run(void* view, const double* in, double* out) {
  if (g_nfilters == 0) return false;
  memcpy(out, in, sizeof(double) * 12);
  bool changed = false;
  for (int i = 0; i < g_nfilters; i++) changed |= g_filters[i].fn(view, out);
  return changed;
}

void camera_prerender_run(void* view) {
  for (int i = 0; i < g_npre; i++) g_pre[i](view);
}

void camera_set_hooked(bool on) { g_hooked = on; }
bool camera_hooked() { return g_hooked.load(); }

}  // namespace tb
