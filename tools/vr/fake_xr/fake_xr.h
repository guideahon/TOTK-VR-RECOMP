// fake_xr: a scripted OpenXR runtime for tests (docs/VR.md "Tests"). The OpenXR loader loads it like any runtime
// (XR_RUNTIME_JSON=build/vr/fake_xr.json); tests that skip the loader call xrNegotiateLoaderRuntimeInterface themselves.
//
// It reads, at xrCreateInstance:
//   FAKE_XR_SCRIPT=<file>  head poses and events over time, one per line ('#' starts a comment):
//                            <t> head <x> <y> <z> <yaw> <pitch> <roll>   the head (VIEW) in STAGE space: metres and
//                                                                        degrees, q = yaw(+Y) * pitch(+X) * roll(+Z):
//                                                                        yaw 90 looks along -X, pitch 30 looks up
//                            <t> unfocus | focus                        the session loses / gets back input focus
//                            <t> exit                                   the runtime asks the app to stop (STOPPING)
//                            <t> lose                                   the session is lost (LOSS_PENDING)
//                          <t> is seconds from the first frame's display time; a line applies from the first frame
//                          whose display time reaches it. Without a script the head stands still 1.7 m up, looking
//                          along -Z.
//   FAKE_XR_LOG=<file>     what the app does: one line per state change and per xrEndFrame (layers, their kind)
//   FAKE_XR_HZ=<n>         the display rate (default 90)
//   FAKE_XR_IPD=<m>        the distance between the eyes (default 0.063)
//
// Phase 0 has no graphics: sessions need XR_MND_headless. XR_KHR_vulkan_enable2 and swapchains come in phase 1.
#pragma once
#include <stdint.h>

#ifdef _WIN32
#define FAKE_XR_EXPORT __declspec(dllexport)
#else
#define FAKE_XR_EXPORT __attribute__((visibility("default")))
#endif

#define FAKE_XR_RUNTIME_NAME "Savage Wind fake_xr"
#define FAKE_XR_SYSTEM_ID 1
#define FAKE_XR_DEFAULT_HZ 90
#define FAKE_XR_DEFAULT_IPD 0.063f
#define FAKE_XR_HEAD_HEIGHT 1.7f
#define FAKE_XR_VIEW_W 1832  // recommended per eye (a Quest 2's)
#define FAKE_XR_VIEW_H 1920
#define FAKE_XR_MAX_LAYERS 16
// the left eye's field (radians); the right eye's is its mirror image
#define FAKE_XR_FOV_LEFT (-0.942f)
#define FAKE_XR_FOV_RIGHT 0.698f
#define FAKE_XR_FOV_UP 0.768f
#define FAKE_XR_FOV_DOWN (-0.890f)

// Counters for tests that load the library themselves (GetProcAddress / dlsym "fake_xr_stats").
typedef struct {
    uint32_t instances;         // alive now
    uint32_t sessions;          // alive now
    uint64_t frames_waited;     // xrWaitFrame calls that succeeded, all sessions
    uint64_t frames_ended;      // xrEndFrame calls that succeeded
    uint64_t frames_discarded;  // xrBeginFrame that discarded a begun frame
    uint32_t last_layer_count;  // layers of the last xrEndFrame
    int32_t state;              // the last session's state as the app has seen it (XrSessionState)
} fake_xr_stats_t;

typedef void (*PFN_fake_xr_stats)(fake_xr_stats_t *out);
