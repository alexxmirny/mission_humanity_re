#pragma once
#include "strategic_input.h"
#include <cstdint>
namespace mh::seams::strategic_input_native {
// Local presentation/input state only. Never publishes a queue edit or writes a world region.
// Native strategic pump, including the interactive mode3 tutorial (not its dialogs).
bool main_context_active();
bool initialize();
bool install();
bool prepare_mouse();
void on_present();
void focus_lost();
void cancel_pointer_gesture();
void cancel_modal_gesture(bool reset_provenance = false);
void wndproc_record(void *hwnd, uint32_t message, uint32_t wparam, int32_t lparam);
void discarded_key_release(uint32_t scan, uint32_t type);
// Pause's existing raw-ring erase, with its exact producer/discard proofs.
uint32_t erase_key_record(uint32_t slot);
// ONLY after successful wholesale snapshot replacement of the native ring.
void key_ring_replaced();
// Existing U48 pop observer calls this BEFORE its caller-range early return. No second entry hook.
void begin_main_pop();
bool observing();
void observe_dequeue(const uint32_t *event, uintptr_t caller);
// Called AFTER the single native pop into its strategic caller's 0x38-byte local record.
void                   main_record(uint32_t *event);
extern const uintptr_t modal_widget_return;
int                    filter_modal_pump(int result);
// H1 calls only for its private modal latch, before modal E0 reconstruction/ASCII translation.
// True neutralizes a record; false continues real H1 translation for mapped
// native edges and untouched text/WAIT/non-strategic records.
bool     modal_record(uint32_t *event, uintptr_t outer_return);
uint8_t  modifiers();
void     capture_pointer_modifiers();
bool     held(mh::strategic_bindings::role_index);
unsigned button_mask(mh::strategic_bindings::role_index);
// Actual world/minimap local-edge bridge at 00442da4; no physical mouse-state rewrite.
// Replaces only the per-iteration native button-get CALL; returns its unchanged physical value.
uint32_t physical_buttons();
void     begin_mouse_pump();
void     mouse_frame(uint8_t *native_ebp);
// Shared command surface used by expanded map, with the same role/double-click/target classifier.
// Pointer coordinates preserve its existing submission-log geometry, never alter the world target.
void mouse_world_order(uint8_t *native_ebp);
void command_tile(mh::strategic_bindings::surface, uint32_t x, uint32_t y, int pointer_x, int pointer_y);
// Publication boundary, also invoked for exact imported snapshot/save rebinds even in same world.
void reset_pending_commands();
// Canonical due admission calls feedback only for this local player, never local capture.
void refused(unsigned reason, unsigned count);
void blocked(unsigned reason, unsigned count);
} // namespace mh::seams::strategic_input_native
