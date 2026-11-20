#pragma once
#include <string>

namespace tb {

// Hyprland / HypXRland IPC from inside Wine: Wine's winsock bridges AF_UNIX sockets, so the
// mod talks to the compositor's request socket ($XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/
// .socket.sock, reached as Z:\...) like hyprctl does. Inert when not running under Hyprland.

// True when the environment names a Hyprland instance.
bool hypr_available();
// One request ("j/clients", "dispatch tagwindow +x address:0x…"); the reply, or "" on failure.
std::string hypr_request(const std::string& req);

// Keep the Hyprland window tag `tag` on this process's top-level game window (empty = none),
// removing the previously applied one. Applied from a background thread once the window is mapped
// (the compositor learns it a moment after CreateWindow); safe to call at any time.
void hypr_set_window_tag(const std::string& tag);
std::string hypr_status();

// Start the background poll (window tag, display state) if running under Hyprland. Idempotent.
void hypr_start();
// Whether the game's monitor (its window's, else the focused one) is a HypXRland side-by-side
// stereo output: 1 yes, 0 no, -1 unknown (not Hyprland, no answer yet). Updated each second;
// hypr_display_stereo_now() asks right away (for start-up decisions, before the window exists).
int hypr_display_stereo();
int hypr_display_stereo_now();

// An environment variable of this process: the Windows environment, else the unix one
// (/proc/self/environ: Wine doesn't copy every variable into the Windows environment).
std::string process_env(const char* name);

}  // namespace tb
