// Drives tools/vr/fake_xr the way the OpenXR loader and an app do, without the loader: negotiation, the session
// lifecycle with its errors, the frame loop's timing and order rules, scripted head poses, focus changes, an exit the
// runtime asks for, and a lost session. The later VR tests run the game against fake_xr, so it has to be right first.
//   test_fake_xr_session <fake_xr library> <scratch folder>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include "../../tests/vr/vr_test.h"
#include "fake_xr/fake_xr.h"

#define CHECK_XR(call, expected)                                                                             \
    do {                                                                                                     \
        XrResult r_ = (call);                                                                                \
        vr_test_checks++;                                                                                    \
        if (r_ != (expected)) {                                                                              \
            vr_test_failures++;                                                                              \
            fprintf(stderr, "%s:%d: [%s] %s = %d, expected %s (%d)\n", __FILE__, __LINE__, vr_test_name, #call, \
                    (int)r_, #expected, (int)(expected));                                                    \
        }                                                                                                    \
    } while (0)

static char scratch[1024];
static PFN_xrNegotiateLoaderRuntimeInterface negotiate;
static PFN_fake_xr_stats get_stats;

#define XR_FUNCS(X)                                                                                                    \
    X(EnumerateInstanceExtensionProperties) X(CreateInstance) X(DestroyInstance) X(GetInstanceProperties)            \
    X(GetSystem) X(GetSystemProperties) X(EnumerateViewConfigurationViews) X(EnumerateEnvironmentBlendModes)         \
    X(CreateSession) X(DestroySession) X(PollEvent) X(BeginSession) X(RequestExitSession) X(EndSession) X(WaitFrame) \
    X(BeginFrame) X(EndFrame) X(CreateReferenceSpace) X(DestroySpace) X(LocateSpace) X(LocateViews) X(ResultToString)
#define DECLARE(n) static PFN_xr##n xr##n;
XR_FUNCS(DECLARE)
static PFN_xrGetInstanceProcAddr gipa;

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static const char *scratch_file(const char *name) {
    static char buf[4][1200];
    static int at;
    char *b = buf[at++ % 4];
    snprintf(b, sizeof buf[0], "%s/%s", scratch, name);
    return b;
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = calloc(1, n + 1);
    size_t got = fread(s, 1, n, f);
    fclose(f);
    size_t w = 0;  // (Windows writes text files with CRLF)
    for (size_t i = 0; i < got; i++)
        if (s[i] != 0x0d) s[w++] = s[i];
    s[w] = 0;
    return s;
}

static int count_lines_starting(const char *text, const char *prefix) {
    int n = 0;
    size_t len = strlen(prefix);
    for (const char *p = text; p && *p;) {
        if (!strncmp(p, prefix, len)) n++;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return n;
}

static void load(const char *path) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) { fprintf(stderr, "can't load %s (%lu)\n", path, GetLastError()); exit(2); }
    negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)(void *)GetProcAddress(lib, "xrNegotiateLoaderRuntimeInterface");
    get_stats = (PFN_fake_xr_stats)(void *)GetProcAddress(lib, "fake_xr_stats");
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "can't load %s: %s\n", path, dlerror()); exit(2); }
    negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(lib, "xrNegotiateLoaderRuntimeInterface");
    get_stats = (PFN_fake_xr_stats)dlsym(lib, "fake_xr_stats");
#endif
    if (!negotiate || !get_stats) { fprintf(stderr, "%s: missing exports\n", path); exit(2); }
}

static fake_xr_stats_t stats(void) {
    fake_xr_stats_t s;
    get_stats(&s);
    return s;
}

static void test_negotiate(void) {
    VR_TEST("negotiate");
    XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof li,
                                1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff)};
    XrNegotiateRuntimeRequest rr = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION,
                                    sizeof rr};
    XrNegotiateLoaderInfo bad = li;
    bad.structSize = 4;
    CHECK_XR(negotiate(&bad, &rr), XR_ERROR_INITIALIZATION_FAILED);
    bad = li;
    bad.minInterfaceVersion = bad.maxInterfaceVersion = 2;  // a loader newer than anything fake_xr speaks
    CHECK_XR(negotiate(&bad, &rr), XR_ERROR_INITIALIZATION_FAILED);
    CHECK_XR(negotiate(&li, &rr), XR_SUCCESS);
    CHECK_EQ_INT(rr.runtimeInterfaceVersion, 1);
    CHECK(rr.getInstanceProcAddr != NULL);
    gipa = rr.getInstanceProcAddr;
    PFN_xrVoidFunction fn;
    CHECK_XR(gipa(XR_NULL_HANDLE, "xrCreateInstance", &fn), XR_SUCCESS);
    CHECK_XR(gipa(XR_NULL_HANDLE, "xrGetSystem", &fn), XR_ERROR_HANDLE_INVALID);  // needs an instance
    CHECK(fn == NULL);
}

static void resolve(XrInstance inst) {
#define RESOLVE(n)                                                                    \
    if (XR_FAILED(gipa(inst, "xr" #n, (PFN_xrVoidFunction *)&xr##n)) || !xr##n) {     \
        fprintf(stderr, "fake_xr has no xr" #n "\n");                                 \
        exit(2);                                                                      \
    }
    XR_FUNCS(RESOLVE)
#undef RESOLVE
}

static XrInstance make_instance(bool headless, XrResult expect) {
    const char *ext[] = {XR_MND_HEADLESS_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "test_fake_xr");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = headless ? 1 : 0;
    ci.enabledExtensionNames = ext;
    XrInstance inst = XR_NULL_HANDLE;
    CHECK_XR(xrCreateInstance(&ci, &inst), expect);
    return inst;
}

static XrSystemId get_system(XrInstance inst) {
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId id = XR_NULL_SYSTEM_ID;
    CHECK_XR(xrGetSystem(inst, &gi, &id), XR_SUCCESS);
    return id;
}

static XrSession make_session(XrInstance inst, XrSystemId sys) {
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, NULL, 0, sys};
    XrSession s = XR_NULL_HANDLE;
    CHECK_XR(xrCreateSession(inst, &sci, &s), XR_SUCCESS);
    return s;
}

// the next state change, or UNKNOWN when there is none
static XrSessionState poll_state(XrInstance inst) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    if (xrPollEvent(inst, &ev) != XR_SUCCESS) return XR_SESSION_STATE_UNKNOWN;
    if (ev.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) return XR_SESSION_STATE_UNKNOWN;
    return ((XrEventDataSessionStateChanged *)&ev)->state;
}

static void expect_states(XrInstance inst, const XrSessionState *want, int n) {
    for (int i = 0; i < n; i++) {
        XrSessionState got = poll_state(inst);
        CHECK_EQ_INT(got, want[i]);
    }
}

static void begin_session(XrSession s) {
    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    CHECK_XR(xrBeginSession(s, &bi), XR_SUCCESS);
}

// one frame with no layers; returns its display time (0 when xrWaitFrame failed) and the result of the wait
static XrTime frame(XrSession s, XrResult *wait_result, bool *should_render) {
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    XrResult r = xrWaitFrame(s, &wi, &fs);
    if (wait_result) *wait_result = r;
    if (XR_FAILED(r)) return 0;
    if (should_render) *should_render = fs.shouldRender;
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(s, &bi))) return 0;
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO, NULL, fs.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0,
                         NULL};
    CHECK_XR(xrEndFrame(s, &ei), XR_SUCCESS);
    return fs.predictedDisplayTime;
}

static void test_instance(void) {
    VR_TEST("instance");
    uint32_t n = 0;
    CHECK_XR(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL), XR_SUCCESS);
    CHECK(n >= 1);
    XrExtensionProperties props[8];
    for (int i = 0; i < 8; i++) props[i] = (XrExtensionProperties){XR_TYPE_EXTENSION_PROPERTIES};
    CHECK_XR(xrEnumerateInstanceExtensionProperties(NULL, 8, &n, props), XR_SUCCESS);
    bool headless = false;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(props[i].extensionName, XR_MND_HEADLESS_EXTENSION_NAME)) headless = true;
    CHECK(headless);

    const char *unknown[] = {"XR_KHR_no_such_extension"};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "test");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = unknown;
    XrInstance inst = XR_NULL_HANDLE;
    CHECK_XR(xrCreateInstance(&ci, &inst), XR_ERROR_EXTENSION_NOT_PRESENT);
    ci.enabledExtensionCount = 0;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(2, 0, 0);
    CHECK_XR(xrCreateInstance(&ci, &inst), XR_ERROR_API_VERSION_UNSUPPORTED);

    // a missing script is an error, not a silent default
    set_env("FAKE_XR_SCRIPT", scratch_file("no_such_script.txt"));
    make_instance(true, XR_ERROR_INITIALIZATION_FAILED);
    set_env("FAKE_XR_SCRIPT", NULL);

    // no graphics binding: only with XR_MND_headless
    inst = make_instance(false, XR_SUCCESS);
    XrSystemId sys = get_system(inst);
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, NULL, 0, sys};
    XrSession s = XR_NULL_HANDLE;
    CHECK_XR(xrCreateSession(inst, &sci, &s), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    CHECK_XR(xrGetInstanceProperties(inst, &ip), XR_SUCCESS);
    CHECK(!strcmp(ip.runtimeName, FAKE_XR_RUNTIME_NAME));
    char text[XR_MAX_RESULT_STRING_SIZE];
    CHECK_XR(xrResultToString(inst, XR_ERROR_SESSION_LOST, text), XR_SUCCESS);
    CHECK(!strcmp(text, "XR_ERROR_SESSION_LOST"));
    CHECK_XR(xrDestroyInstance(inst), XR_SUCCESS);
}

static void test_system(XrInstance inst, XrSystemId sys) {
    VR_TEST("system");
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HANDHELD_DISPLAY};
    XrSystemId other;
    CHECK_XR(xrGetSystem(inst, &gi, &other), XR_ERROR_FORM_FACTOR_UNSUPPORTED);
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    CHECK_XR(xrGetSystemProperties(inst, sys, &sp), XR_SUCCESS);
    CHECK(sp.trackingProperties.positionTracking && sp.trackingProperties.orientationTracking);
    CHECK_EQ_INT(sp.graphicsProperties.maxLayerCount, FAKE_XR_MAX_LAYERS);
    uint32_t n = 0;
    CHECK_XR(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, NULL),
             XR_SUCCESS);
    CHECK_EQ_INT(n, 2);
    XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    CHECK_XR(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &n, v),
             XR_ERROR_SIZE_INSUFFICIENT);
    CHECK_XR(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v),
             XR_SUCCESS);
    CHECK_EQ_INT(v[1].recommendedImageRectWidth, FAKE_XR_VIEW_W);
    CHECK_EQ_INT(v[1].recommendedImageRectHeight, FAKE_XR_VIEW_H);
    CHECK_XR(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO, 0, &n, NULL),
             XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    XrEnvironmentBlendMode modes[4];
    CHECK_XR(xrEnumerateEnvironmentBlendModes(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &n, modes),
             XR_SUCCESS);
    CHECK(n == 1 && modes[0] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
}

static void test_lifecycle(void) {
    const char *log = scratch_file("lifecycle.log");
    set_env("FAKE_XR_LOG", log);
    set_env("FAKE_XR_HZ", "90");
    XrInstance inst = make_instance(true, XR_SUCCESS);
    XrSystemId sys = get_system(inst);
    test_system(inst, sys);

    VR_TEST("lifecycle");
    XrSession s = make_session(inst, sys);
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, NULL, 0, sys};
    XrSession s2;
    CHECK_XR(xrCreateSession(inst, &sci, &s2), XR_ERROR_LIMIT_REACHED);
    CHECK_EQ_INT(stats().sessions, 1);

    // READY has been queued but not seen yet: beginning now is wrong
    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    CHECK_XR(xrBeginSession(s, &bi), XR_ERROR_SESSION_NOT_READY);
    const XrSessionState start[] = {XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY, XR_SESSION_STATE_UNKNOWN};
    expect_states(inst, start, 3);
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    CHECK_XR(xrWaitFrame(s, NULL, &fs), XR_ERROR_SESSION_NOT_RUNNING);
    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    CHECK_XR(xrBeginSession(s, &bi), XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    begin_session(s);
    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    CHECK_XR(xrBeginSession(s, &bi), XR_ERROR_SESSION_RUNNING);

    VR_TEST("frame order");
    XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
    CHECK_XR(xrBeginFrame(s, &fbi), XR_ERROR_CALL_ORDER_INVALID);  // no xrWaitFrame yet
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO, NULL, 1, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, NULL};
    CHECK_XR(xrEndFrame(s, &ei), XR_ERROR_CALL_ORDER_INVALID);  // no xrBeginFrame

    VR_TEST("frame timing");
    // the first frame is drawn while still READY (shouldRender false); then SYNCHRONIZED, VISIBLE, FOCUSED
    bool render = true;
    XrTime t0 = frame(s, NULL, &render);
    CHECK(t0 > 0);
    CHECK(!render);
    const XrSessionState front[] = {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED,
                                    XR_SESSION_STATE_UNKNOWN};
    expect_states(inst, front, 4);
    const XrDuration period = 1000000000ll / 90;
    XrTime prev = t0;
    for (int i = 0; i < 9; i++) {
        XrTime t = frame(s, NULL, &render);
        CHECK_EQ_INT(t - prev, period);
        CHECK(render);
        prev = t;
    }
    CHECK_EQ_INT(stats().frames_ended, 10);
    CHECK_EQ_INT(stats().state, XR_SESSION_STATE_FOCUSED);

    VR_TEST("frame errors");
    XrFrameState f2 = {XR_TYPE_FRAME_STATE};
    CHECK_XR(xrWaitFrame(s, NULL, &f2), XR_SUCCESS);
    CHECK_XR(xrBeginFrame(s, &fbi), XR_SUCCESS);
    CHECK_XR(xrWaitFrame(s, NULL, &f2), XR_SUCCESS);
    CHECK_XR(xrBeginFrame(s, &fbi), XR_FRAME_DISCARDED);  // a begun frame never ended is dropped
    CHECK_EQ_INT(stats().frames_discarded, 1);
    ei.displayTime = f2.predictedDisplayTime + 12345;
    CHECK_XR(xrEndFrame(s, &ei), XR_ERROR_TIME_INVALID);
    ei.displayTime = f2.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    CHECK_XR(xrEndFrame(s, &ei), XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = FAKE_XR_MAX_LAYERS + 1;
    CHECK_XR(xrEndFrame(s, &ei), XR_ERROR_LAYER_LIMIT_EXCEEDED);
    ei.layerCount = 0;
    CHECK_XR(xrEndFrame(s, &ei), XR_SUCCESS);

    VR_TEST("exit");
    CHECK_XR(xrEndSession(s), XR_ERROR_SESSION_NOT_STOPPING);
    CHECK_XR(xrRequestExitSession(s), XR_SUCCESS);
    const XrSessionState stop[] = {XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING,
                                   XR_SESSION_STATE_UNKNOWN};
    expect_states(inst, stop, 4);
    CHECK_XR(xrEndSession(s), XR_SUCCESS);
    const XrSessionState end[] = {XR_SESSION_STATE_IDLE, XR_SESSION_STATE_EXITING};
    expect_states(inst, end, 2);
    CHECK_XR(xrWaitFrame(s, NULL, &f2), XR_ERROR_SESSION_NOT_RUNNING);
    CHECK_XR(xrRequestExitSession(s), XR_ERROR_SESSION_NOT_RUNNING);
    CHECK_XR(xrDestroySession(s), XR_SUCCESS);
    CHECK_XR(xrDestroyInstance(inst), XR_SUCCESS);
    CHECK_EQ_INT(stats().instances, 0);
    CHECK_EQ_INT(stats().sessions, 0);

    VR_TEST("log");
    char *text = read_file(log);
    CHECK(text != NULL);
    CHECK_EQ_INT(count_lines_starting(text, "frame "), 11 + 1);  // 11 frames ended, 1 "frame discarded"
    CHECK(strstr(text, "state XR_SESSION_STATE_FOCUSED\n") != NULL);
    CHECK(strstr(text, "state XR_SESSION_STATE_EXITING\n") != NULL);
    CHECK(strstr(text, "layers 0") != NULL);
    free(text);
    set_env("FAKE_XR_LOG", NULL);
    set_env("FAKE_XR_HZ", NULL);
}

static XrSpace ref_space(XrSession s, XrReferenceSpaceType type, float y) {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, type, {{0, 0, 0, 1}, {0, y, 0}}};
    XrSpace sp = XR_NULL_HANDLE;
    CHECK_XR(xrCreateReferenceSpace(s, &ci, &sp), XR_SUCCESS);
    return sp;
}

static void locate(XrSession s, XrSpace base, XrTime t, XrView v[2]) {
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, t, base};
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    v[0] = (XrView){XR_TYPE_VIEW};
    v[1] = (XrView){XR_TYPE_VIEW};
    CHECK_XR(xrLocateViews(s, &li, &vs, 2, &n, v), XR_SUCCESS);
    CHECK_EQ_INT(n, 2);
    CHECK(vs.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT);
}

static void test_views(void) {
    VR_TEST("views");
    const char *script = scratch_file("views.txt");
    write_file(script,
               "# t   head x y z  yaw pitch roll\n"
               "0     head 0 1.7 0  0 0 0\n"
               "0.5   head 1 1.6 -2 90 0 0   # turned to face -X, moved\n");
    set_env("FAKE_XR_SCRIPT", script);
    set_env("FAKE_XR_IPD", "0.064");
    XrInstance inst = make_instance(true, XR_SUCCESS);
    XrSession s = make_session(inst, get_system(inst));
    while (poll_state(inst) != XR_SESSION_STATE_UNKNOWN) {}
    begin_session(s);
    XrSpace stage = ref_space(s, XR_REFERENCE_SPACE_TYPE_STAGE, 0);
    XrSpace local_up = ref_space(s, XR_REFERENCE_SPACE_TYPE_LOCAL, 1.0f);
    XrSpace view = ref_space(s, XR_REFERENCE_SPACE_TYPE_VIEW, 0);
    XrReferenceSpaceCreateInfo badq = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_STAGE,
                                       {{0, 0, 0, 0}, {0, 0, 0}}};
    XrSpace bad;
    CHECK_XR(xrCreateReferenceSpace(s, &badq, &bad), XR_ERROR_POSE_INVALID);

    XrTime t = frame(s, NULL, NULL);
    XrView v[2];
    locate(s, stage, t, v);
    CHECK_NEAR(v[0].pose.position.x, -0.032, 1e-5);
    CHECK_NEAR(v[1].pose.position.x, 0.032, 1e-5);
    CHECK_NEAR(v[0].pose.position.y, 1.7, 1e-5);
    CHECK_NEAR(v[0].pose.orientation.w, 1, 1e-5);
    CHECK_NEAR(v[0].fov.angleLeft, FAKE_XR_FOV_LEFT, 1e-6);
    CHECK_NEAR(v[1].fov.angleRight, -FAKE_XR_FOV_LEFT, 1e-6);  // the right eye's field mirrors the left's
    CHECK_NEAR(v[1].fov.angleLeft, -FAKE_XR_FOV_RIGHT, 1e-6);
    locate(s, local_up, t, v);  // a base space 1 m up sees the eyes 1 m lower
    CHECK_NEAR(v[0].pose.position.y, 0.7, 1e-5);

    // past 0.5 s (45 periods at 90 Hz after the first frame): the second line applies
    for (int i = 0; i < 46; i++) t = frame(s, NULL, NULL);
    locate(s, stage, t, v);
    // yaw +90 turns the head's right (XR +X) to -Z, so the left eye sits at +Z of the head and the right one at -Z
    CHECK_NEAR(v[0].pose.position.x, 1, 1e-4);
    CHECK_NEAR(v[0].pose.position.z, -2 + 0.032, 1e-4);
    CHECK_NEAR(v[1].pose.position.z, -2 - 0.032, 1e-4);
    CHECK_NEAR(v[0].pose.position.y, 1.6, 1e-4);

    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    CHECK_XR(xrLocateSpace(view, stage, t, &loc), XR_SUCCESS);
    CHECK_NEAR(loc.pose.position.x, 1, 1e-4);
    CHECK_NEAR(loc.pose.position.z, -2, 1e-4);
    CHECK_NEAR(fabs(loc.pose.orientation.y), sin(3.14159265358979 / 4), 1e-4);  // 90 degrees about +Y
    CHECK_XR(xrLocateSpace(view, view, t, &loc), XR_SUCCESS);
    CHECK_NEAR(loc.pose.position.x, 0, 1e-5);
    CHECK_NEAR(loc.pose.orientation.w, 1, 1e-5);
    CHECK_XR(xrLocateSpace(stage, view, t, &loc), XR_SUCCESS);  // the stage's origin seen from the head
    CHECK_NEAR(loc.pose.position.y, -1.6, 1e-4);
    CHECK_XR(xrLocateSpace(stage, view, 0, &loc), XR_ERROR_TIME_INVALID);

    CHECK_XR(xrDestroySpace(stage), XR_SUCCESS);
    CHECK_XR(xrDestroySpace(local_up), XR_SUCCESS);
    CHECK_XR(xrDestroySpace(view), XR_SUCCESS);
    CHECK_XR(xrDestroyInstance(inst), XR_SUCCESS);  // takes the session with it
    CHECK_EQ_INT(stats().sessions, 0);
    set_env("FAKE_XR_SCRIPT", NULL);
    set_env("FAKE_XR_IPD", NULL);
}

static void test_focus_and_exit(void) {
    VR_TEST("focus and runtime exit");
    const char *script = scratch_file("focus.txt");
    write_file(script, "0.1 unfocus\n0.2 focus\n0.3 exit\n");
    set_env("FAKE_XR_SCRIPT", script);
    XrInstance inst = make_instance(true, XR_SUCCESS);
    XrSession s = make_session(inst, get_system(inst));
    while (poll_state(inst) != XR_SESSION_STATE_UNKNOWN) {}
    begin_session(s);
    XrSessionState seen[16];
    int n = 0;
    for (int i = 0; i < 40 && n < 16; i++) {
        XrSessionState st;
        while ((st = poll_state(inst)) != XR_SESSION_STATE_UNKNOWN && n < 16) seen[n++] = st;
        if (n && seen[n - 1] == XR_SESSION_STATE_STOPPING) break;
        frame(s, NULL, NULL);
    }
    const XrSessionState want[] = {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE,
                                   XR_SESSION_STATE_FOCUSED,      XR_SESSION_STATE_VISIBLE,
                                   XR_SESSION_STATE_FOCUSED,      XR_SESSION_STATE_VISIBLE,
                                   XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING};
    CHECK_EQ_INT(n, 8);
    for (int i = 0; i < 8 && i < n; i++) CHECK_EQ_INT(seen[i], want[i]);
    CHECK_XR(xrEndSession(s), XR_SUCCESS);
    CHECK_XR(xrDestroyInstance(inst), XR_SUCCESS);
    set_env("FAKE_XR_SCRIPT", NULL);
}

static void test_lost(void) {
    VR_TEST("session lost");
    const char *script = scratch_file("lost.txt");
    write_file(script, "0.05 lose\n");
    set_env("FAKE_XR_SCRIPT", script);
    XrInstance inst = make_instance(true, XR_SUCCESS);
    XrSession s = make_session(inst, get_system(inst));
    while (poll_state(inst) != XR_SESSION_STATE_UNKNOWN) {}
    begin_session(s);
    XrResult r = XR_SUCCESS;
    int frames = 0;
    while (r == XR_SUCCESS && frames < 100) {
        frame(s, &r, NULL);
        frames++;
    }
    CHECK_XR(r, XR_SESSION_LOSS_PENDING);
    CHECK(frames >= 5 && frames <= 7);  // 0.05 s at 90 Hz: the 6th frame (+-1 for rounding)
    XrSessionState st, last = XR_SESSION_STATE_UNKNOWN;
    while ((st = poll_state(inst)) != XR_SESSION_STATE_UNKNOWN) last = st;
    CHECK_EQ_INT(last, XR_SESSION_STATE_LOSS_PENDING);
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    CHECK_XR(xrWaitFrame(s, NULL, &fs), XR_ERROR_SESSION_LOST);
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    CHECK_XR(xrBeginFrame(s, &bi), XR_ERROR_SESSION_LOST);
    CHECK_XR(xrRequestExitSession(s), XR_ERROR_SESSION_LOST);
    CHECK_XR(xrDestroySession(s), XR_SUCCESS);  // what an app does after a loss: drop the session, maybe retry
    XrSession again = make_session(inst, get_system(inst));
    CHECK(again != XR_NULL_HANDLE);
    CHECK_XR(xrDestroyInstance(inst), XR_SUCCESS);
    set_env("FAKE_XR_SCRIPT", NULL);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fake_xr library> <scratch folder>\n", argv[0]);
        return 2;
    }
    snprintf(scratch, sizeof scratch, "%s", argv[2]);
    load(argv[1]);
    test_negotiate();
    // the functions come from an instance (only xrCreateInstance and the enumerations work without one); fake_xr
    // hands out the same ones for every instance, so one probe instance is enough for the whole test
    if (XR_FAILED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&xrCreateInstance))) return 2;
    XrInstance probe = make_instance(true, XR_SUCCESS);
    resolve(probe);
    xrDestroyInstance(probe);
    test_instance();
    test_lifecycle();
    test_views();
    test_focus_and_exit();
    test_lost();
    return VR_TEST_END("test_fake_xr_session");
}
