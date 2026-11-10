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

// What a frame was drawn as (vr_camera.c): the head's view in play (world), for one eye (0 left, 1 right) or both
// (-1), with the pose (LOCAL space) and the frustum the camera was given; else the virtual screen.
typedef struct {
    bool world;
    int eye;
    XrPosef pose;
    XrFovf fov;
} vr_view;

bool vr_world_view(void);                     // vr_camera.c: in VR with head tracking on (the cameras follow the head)
void vr_present_mark(void);                   // the game presents a frame: what it was drawn as goes into the queue
bool vr_present_take(vr_view *out);           // the frame being copied to the headset, in the order presented
void vr_window_aspect(float width_over_height);
void vr_recenter(void);
void vr_turn(float radians);
uint64_t vr_camera_updates(void);

void vr_input_setup(XrSession session);       // vr_input.c: the controllers' actions on the session
void vr_stats(uint64_t *frames, uint64_t *rendered, int *state);
#endif
