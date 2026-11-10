// What runtime/vr*.c share (TOTK_VR builds only).
#pragma once
#ifdef TOTK_VR
#include <stdbool.h>
#include <stdint.h>
#include <openxr/openxr.h>

XrInstance vr_xr_instance(void);    // XR_NULL_HANDLE unless vr_enabled()
XrSession vr_xr_session(void);
XrSpace vr_xr_local(void);          // the room (LOCAL)
XrSpace vr_xr_view(void);           // the head (VIEW)
bool vr_session_running(void);
XrTime vr_next_display_time(void);  // when the frame being made now should be shown (0 before frames)

bool vr_xr_ok(XrResult r, const char *what);  // "vr: what: XR_ERROR_..." in the log; false when r failed
void vr_disable(const char *why);             // VR stops for the rest of the run; the game goes on flat

void vr_input_setup(XrSession session);       // vr_input.c: the controllers' actions on the session
void vr_stats(uint64_t *frames, uint64_t *rendered, int *state);
#endif
