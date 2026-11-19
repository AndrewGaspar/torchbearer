#pragma once
// The second eye of a view-path sbs update (docs/stereo.md section 4): both eyes are rendered
// inside one engine frame (same frame counter 0xd744f8, same frame time 0xd74504), so render-side
// state that is stepped per renderWorld call would step twice per update, and work that does not
// depend on the eye is done twice. This module knows which eye of the update is being rendered
// and, for the second one:
//  - rewinds per-object temporal state that a render advances (lens flares' visibility average
//    and fade), so the second eye sees what the first saw and the state advances once per update;
//  - optionally skips eye-independent passes and reuses the first eye's results (shadow maps,
//    the sky cube probe).
// Outside the view path (slot -1) every hook is a pass-through.
#include <string>

namespace tb::stereo {

void pass2_register();  // config listeners (from register_feature)
void pass2_start();     // signature checks and hooks (from start)

// Which eye of this update renderFrame is about to render: 0 = first, 1 = second, -1 = not a
// view-path sbs update.
void pass2_eye(int slot);

std::string pass2_status();

}  // namespace tb::stereo
