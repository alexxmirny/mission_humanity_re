// Selected strategic intents, composited after ground and before native objects/shadows/bodies.
#pragma once

namespace mh::ui::selected_targets {
// Arms guarded ground-refresh and successful-effect observers without taking the renderer entry.
// The existing zoom call remains untouched.
bool install();
// Render-thread only. Read-only over gameplay state; presentation time drives the dashes.
void draw();
// Damage-only view change: preserve command provenance across resolution/zoom changes.
void invalidate_view();
// Full world replacement: invalidate pixels and forget local provenance after load/resync/import.
// Requires live dirty-map bindings before the next ground pass.
void reset();
} // namespace mh::ui::selected_targets
