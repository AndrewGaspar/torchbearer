#define QUIVER_MOD_VERSION "0.1"
#define QUIVER_MOD_IMPLEMENTATION
#include "quiver_mod.h"

#include "core/quiver.h"

#include <string>

#include "core/config.h"
#include "core/log.h"

namespace tb {
namespace {
quiver_mod* g_q = nullptr;
std::string g_layout;  // last stereo layout, for before quiver_init

int on_values(void*, const quiver_value* v, int n) {
  for (int i = 0; i < n; i++) cfg_set(v[i].id, v[i].text ? v[i].text : "", "quiver");
  return 0;
}
}  // namespace

void quiver_init() {
  g_q = quiver_open("torchbearer", nullptr, 0);
  quiver_on_values_cb(g_q, on_values, nullptr);
  TB_LOG("quiver SDK: %s", quiver_connected(g_q) ? "connected" : "opened (connects in background)");
  if (g_q && !g_layout.empty()) quiver_set_stereo_layout(g_q, g_layout.c_str());
}

void quiver_live(const char* id) { quiver_handle_option(g_q, id); }

void quiver_stereo_layout(const char* layout) {
  if (g_layout == layout) return;
  g_layout = layout;
  if (g_q) quiver_set_stereo_layout(g_q, layout);
  TB_LOG("quiver SDK: stereo layout %s", layout);
}

void quiver_report(const std::string& id, const std::string& text, bool persist) {
  quiver_changed(g_q, id.c_str(), text.c_str(), persist ? 1 : 0);
}

void quiver_text(const std::string& metric, const std::string& text) {
  quiver_readout(g_q, metric.c_str(), text.c_str());
}

}  // namespace tb
