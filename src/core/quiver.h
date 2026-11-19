#pragma once
#include <string>

namespace tb {

// Quiver mod SDK (third_party/quiver_mod.h): surface "torchbearer". Values Quiver sends live go
// into the config (cfg_set), so features only ever listen to the config.
void quiver_init();

// Tell Quiver about a value the mod changed itself (persist: save it to the profile).
void quiver_report(const std::string& id, const std::string& text, bool persist);
void quiver_text(const std::string& metric, const std::string& text);

// Option ids the mod applies live (sent as `set` while the game runs).
void quiver_live(const char* id);

}  // namespace tb
