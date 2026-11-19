#pragma once
#include <functional>
#include <string>

namespace tb {

// torchbearer.ini next to the game exe, ids are "section.key". Re-read whenever the file
// changes (polled every 250 ms), and also fed live by Quiver's mod SDK when the game runs under
// Quiver. Every effective change is broadcast to listeners, on the config thread.
//
// Values set through the SDK or the lab channel override the file until the file next changes
// that same key.

void config_init();

std::string cfg_str(const std::string& id, const std::string& def = "");
double cfg_num(const std::string& id, double def = 0);
bool cfg_bool(const std::string& id, bool def = false);

// Set a value at runtime (not written to the file). Fires listeners if it changed.
void cfg_set(const std::string& id, const std::string& value, const char* origin);

// Called with (id, value) for each changed key; `prefix` filters by id prefix ("" = all).
// Listeners registered before config_init() also receive the initial values.
void cfg_listen(const std::string& prefix,
                std::function<void(const std::string& id, const std::string& value)> fn);

// Monotonic generation, bumped on every applied change.
unsigned cfg_generation();

}  // namespace tb
