#pragma once
#include "../../mh_common/participant_boundary.h"

namespace mh::participant_boundary::hosted {
bool       initialize();
authority &owner();
uint64_t   participant_body_boundary(uint32_t operation, uint64_t argument);
int32_t    participant_removal_admit(const void *request, uint32_t request_size);
void       begin_session();
void       observe_session_begin();
void       end_session();
void       prepare_frame();
void       invalidate_world();
void       poll();
bool       removal_ready(int32_t side);
void       remove_terminal(int32_t side);
bool       install_native();
} // namespace mh::participant_boundary::hosted

extern "C" void MH_Participant_PollGateway();
