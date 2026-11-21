#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Harness-only host seam. The profile bytes are frozen before input starts. No per-frame IO.
// Query size by passing bytes==0 and capacity==0; returns1 and writes required bytes.
int MH_StrategicBindings_Profile(void *bytes, unsigned capacity, unsigned *written);
// UI-journal preflight only. Returns0 after the first normalized input event or on any invalid bytes.
// error_code receives mh::strategic_bindings::error_code; never falls back to local assignments.
int MH_StrategicBindings_AdoptProfile(const void *bytes, unsigned size, unsigned *error_code);
#ifdef __cplusplus
}
#endif
