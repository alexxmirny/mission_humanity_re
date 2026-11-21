#pragma once

#include <cstdint>
#include "include/mh_order_witness.h"

namespace mh::ui::selected_targets::detail {
struct order_metadata_edit {
    mh::order_witness::metadata_binding binding{};
    mh::order_witness::canonical_order  before{};
    uint32_t                            lane = 0, slot = 0;
};
// Optional presentation-only amendment. Capture immediately before the raw metadata
// stamp, finish immediately after, with no destructive witness operation in between.
order_metadata_edit begin_order_metadata_edit(uint32_t lane, uint32_t index);
void                finish_order_metadata_edit(const order_metadata_edit &);

// Install after promotions, caps geometry setup and witness-store allocation.
// Each native owner is skipped independently when promoted.
bool install_order_mutation_observers();

// Called by the EXISTING successful-admission adapter, after its INC.
void native_order_admitted(uint32_t container);

// A rejected tagged record is dequeued without a successful presentation application.
void native_order_dequeued(uint32_t queue_index, bool applied);

// Called after a successful caps order-geometry toggle.
// Indices remain stable across stock/wide copies; only truncated tails expire.
void native_order_geometry_changed();

} // namespace mh::ui::selected_targets::detail
