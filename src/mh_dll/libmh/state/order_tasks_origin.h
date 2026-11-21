// Destination: src/mh_dll/libmh/state/order_tasks_origin.h
#pragma once
#include "include/mh_order_tasks_host.h"
#include "include/mh_order_tasks_reservations.h"
namespace mh::order_tasks::origin {
// Transient synchronous issue context only. Payload stamped into REAL records is the replay
// provenance; this pointer chain is never queried at apply, save, replay or presentation.
class primary_scope {
public:
    primary_scope(uint32_t player, uint32_t slot);
    ~primary_scope();
    primary_scope(const primary_scope &)            = delete;
    primary_scope &operator=(const primary_scope &) = delete;

private:
    friend class issue_guard;
    friend void    stamp(uint32_t, uint32_t, uint16_t, int16_t, int32_t (&)[13]);
    primary_scope *previous_;
    uint32_t       player_, slot_;
};
// Begins BEFORE the original native estimate/cache/tally mutation; finishes after its enqueue
// and statistic tail. The original estimator and issue-time overkill reservation run once.
class issue_guard {
public:
    issue_guard(uint32_t player, uint32_t slot, uint32_t target_ref, uint32_t target_slot, bool physical_only = false);
    ~issue_guard();
    bool allowed() const { return !denied_; }
    bool yielded_to_due_edit() const { return yielded_to_due_edit_; }
    issue_guard(const issue_guard &)            = delete;
    issue_guard &operator=(const issue_guard &) = delete;

private:
    friend void               stamp(uint32_t, uint32_t, uint16_t, int16_t, int32_t (&)[13]);
    friend void               note_source_sink(mh_order_tasks_callback, uint32_t, uint32_t, bool);
    issue_guard              *previous_;
    reservations::issue_scope scope_{};
    bool                      active_ = false, denied_ = false, recorded_ = false, physical_only_ = false, installed_ = false;
    bool                      source_sink_executed_ = false, source_sink_suppressed_ = false;
    bool                      yielded_to_due_edit_ = false;
};
void stamp(uint32_t player, uint32_t slot, uint16_t code, int16_t param, int32_t (&args)[13]);
void note_source_sink(mh_order_tasks_callback, uint32_t player, uint32_t slot, bool suppressed);
void published();
} // namespace mh::order_tasks::origin
