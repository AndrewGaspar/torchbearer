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

}  // namespace tb
