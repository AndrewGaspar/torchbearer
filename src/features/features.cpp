#include "features/features.h"

#include "core/config.h"
#include "core/lab.h"
#include "features/levels.h"

namespace tb {

void features_register() {
  // `quiver lab call <i> cfg <id> [value]`: read or set (in memory) one config value.
  lab_verb("cfg", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    if (a.empty()) {
      ok = false;
      return json_str("usage: cfg <id> [value]");
    }
    if (a.size() >= 2) cfg_set(a[0], a[1], "lab");
    return json_str(cfg_str(a[0]));
  });
  levels_register();
}

void features_start() { levels_start(); }

}  // namespace tb
