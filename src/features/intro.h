#pragma once

namespace tb {

// Called from DllMain (before the game's WinMain): reads [intro] from torchbearer.ini and
// rewrites the startup video / intro screen tables. Plain memory writes and a profile read only.
void intro_early();
// Logs what intro_early() did (call once the log is open).
void intro_report();

}  // namespace tb
