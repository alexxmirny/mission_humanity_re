// Native presentation-only attack provenance observers.
#pragma once

namespace mh::ui::selected_targets::detail {
// Install once, after gameplay promotions and world-region binding, at boot's quiescent point.
// Promoted owners emit the corresponding translated presentation events instead.
bool install_attack_observers();
} // namespace mh::ui::selected_targets::detail
