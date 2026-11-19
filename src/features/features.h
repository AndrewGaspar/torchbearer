#pragma once

namespace tb {

// Each feature registers its config listeners and lab verbs in features_register() (before the
// config is loaded, so listeners see the initial values) and installs hooks in features_start().
void features_register();
void features_start();

}  // namespace tb
