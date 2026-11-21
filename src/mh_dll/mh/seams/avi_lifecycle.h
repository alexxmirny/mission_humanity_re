#pragma once

namespace mh::avi_lifecycle {
// Required before the first native movie. Failure must abort arming, not leave partial ownership.
bool install();
// Implemented beside the existing display-mode paint guard; main-thread nesting is shared.
void suppress_frames(bool enter);
} // namespace mh::avi_lifecycle
