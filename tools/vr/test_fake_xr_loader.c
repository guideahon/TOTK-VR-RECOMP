// fake_xr through a real OpenXR loader (XR_RUNTIME_JSON=build/vr/fake_xr.json), the way the game will reach it:
// the manifest is found and read, negotiation works, and a whole session runs from READY to EXITING. If this passes
// and test_fake_xr_session passes, a VR test that runs the game against fake_xr tests the game, not the plumbing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openxr/openxr.h>

#include "../../tests/vr/vr_test.h"
#include "fake_xr/fake_xr.h"

#define CHECK_OK(call)                                                                                          \
    do {                                                                                                        \
        XrResult r_ = (call);                                                                                   \
        vr_test_checks++;                                                                                       \
        if (XR_FAILED(r_)) {                                                                                    \
            vr_test_failures++;                                                                                 \
            fprintf(stderr, "%s:%d: [%s] %s failed: %d\n", __FILE__, __LINE__, vr_test_name, #call, (int)r_);    \
        }                                                                                                       \
    } while (0)

static XrSessionState next_state(XrInstance inst) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) return ((XrEventDataSessionStateChanged *)&ev)->state;
        ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
    return XR_SESSION_STATE_UNKNOWN;
}

static void wait_for(XrInstance inst, XrSessionState want) {
    XrSessionState st;
    while ((st = next_state(inst)) != XR_SESSION_STATE_UNKNOWN)
        if (st == want) return;
    CHECK_EQ_INT(st, want);
}

int main(void) {
    if (!getenv("XR_RUNTIME_JSON")) {
        fprintf(stderr, "set XR_RUNTIME_JSON to fake_xr's manifest (make test-vr-loader does)\n");
        return 2;
    }
    VR_TEST("loader instance");
    uint32_t n = 0;
    CHECK_OK(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL));
    CHECK(n >= 1);

    const char *ext[] = {XR_MND_HEADLESS_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "test_fake_xr_loader");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = ext;
    XrInstance inst = XR_NULL_HANDLE;
    CHECK_OK(xrCreateInstance(&ci, &inst));
    if (inst == XR_NULL_HANDLE) return VR_TEST_END("test_fake_xr_loader");
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    CHECK_OK(xrGetInstanceProperties(inst, &ip));
    CHECK(!strcmp(ip.runtimeName, FAKE_XR_RUNTIME_NAME));

    VR_TEST("loader session");
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    CHECK_OK(xrGetSystem(inst, &gi, &sys));
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, NULL, 0, sys};
    XrSession s = XR_NULL_HANDLE;
    CHECK_OK(xrCreateSession(inst, &sci, &s));
    wait_for(inst, XR_SESSION_STATE_READY);
    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    CHECK_OK(xrBeginSession(s, &bi));

    VR_TEST("loader frames");
    XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_STAGE,
                                      {{0, 0, 0, 1}, {0, 0, 0}}};
    XrSpace stage = XR_NULL_HANDLE;
    CHECK_OK(xrCreateReferenceSpace(s, &rci, &stage));
    XrTime prev = 0;
    for (int i = 0; i < 5; i++) {
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        CHECK_OK(xrWaitFrame(s, NULL, &fs));
        CHECK(fs.predictedDisplayTime > prev);
        prev = fs.predictedDisplayTime;
        CHECK_OK(xrBeginFrame(s, NULL));
        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                               fs.predictedDisplayTime, stage};
        XrViewState vs = {XR_TYPE_VIEW_STATE};
        XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        CHECK_OK(xrLocateViews(s, &li, &vs, 2, &n, v));
        CHECK_NEAR(v[0].pose.position.y, FAKE_XR_HEAD_HEIGHT, 1e-5);
        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO, NULL, fs.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                             0, NULL};
        CHECK_OK(xrEndFrame(s, &ei));
    }
    wait_for(inst, XR_SESSION_STATE_FOCUSED);

    VR_TEST("loader exit");
    CHECK_OK(xrRequestExitSession(s));
    wait_for(inst, XR_SESSION_STATE_STOPPING);
    CHECK_OK(xrEndSession(s));
    wait_for(inst, XR_SESSION_STATE_EXITING);
    CHECK_OK(xrDestroySpace(stage));
    CHECK_OK(xrDestroySession(s));
    CHECK_OK(xrDestroyInstance(inst));
    return VR_TEST_END("test_fake_xr_loader");
}
