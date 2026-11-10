// fake_xr: a scripted OpenXR runtime for tests. What it reads and does: fake_xr.h.
//
// It follows the OpenXR 1.0 session lifecycle closely enough that an app which is wrong about it fails here as it
// would on a real runtime: states change only through xrPollEvent, xrBeginSession needs READY, xrEndSession needs
// STOPPING, xrBeginFrame needs an xrWaitFrame before it, xrEndFrame needs xrBeginFrame and a display time the runtime
// predicted. Time is deterministic: frame n is shown at base + n periods, whatever the wall clock says.
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_reflection.h>

#include "vr_math.h"
#include "fake_xr.h"

#define DEG (VR_PI / 180.0f)
#define MAGIC_INSTANCE 0x46584931u  // "FXI1"
#define MAGIC_SESSION 0x46585331u
#define MAGIC_SPACE 0x46585031u
#define BASE_TIME 1000000000ll      // the first display times are around 1 s: never 0, which is not a valid XrTime
#define MAX_EVENTS 32
#define MAX_SCRIPT 4096

// ---- the script ----
enum { EV_HEAD, EV_UNFOCUS, EV_FOCUS, EV_EXIT, EV_LOSE, EV_INPUT };
typedef struct {
    double t;
    int kind;
    vr_pose head;
    char input_kind[8];  // EV_INPUT: button, value or stick, the input's path and its value(s)
    char path[96];
    float x, y;
} script_line;

typedef struct {
    float ipd;
    int hz;
    FILE *log;
    script_line *lines;
    int count;
    struct { double t[1024]; vr_pose p[1024]; int n; } hands[2];  // hand poses over time (interpolated), left / right
    char capture_dir[1024];  // FAKE_XR_CAPTURE: PPMs of the layers' images go here
    int capture_every;       // FAKE_XR_CAPTURE_EVERY: every n-th ended frame (0: none)
} config;

// ---- handles ----
typedef struct session session;
typedef struct {
    uint32_t magic;
    bool headless, vulkan;  // the extensions the app enabled
    bool vk_requirements;   // xrGetVulkanGraphicsRequirements2KHR was called
    VkInstance vk_inst;     // what xrCreateVulkanInstanceKHR / ...Device2KHR / ...DeviceKHR made or chose
    PFN_vkGetInstanceProcAddr vk_gipa;
    VkPhysicalDevice vk_phys;
    VkDevice vk_dev;
    config cfg;
    session *session;
} instance;

typedef struct {
    int kind;  // 0: a state change, 1: instance loss
    XrSessionState state;
} pending_event;

struct session {
    uint32_t magic;
    instance *inst;
    XrSessionState state;   // as the app has seen it (through xrPollEvent)
    XrSessionState queued;  // after every queued event
    bool running, lost, synced;
    pending_event events[MAX_EVENTS];
    int ev_head, ev_count;
    uint64_t frame;         // frames waited
    XrTime first_time, last_predicted;
    int waited;             // xrWaitFrame calls not yet followed by xrBeginFrame
    bool begun;
    int script_at;          // the next script line to apply
    vr_pose head;           // VIEW in STAGE space
    struct fxvk *vk;        // the graphics binding (fake_xr_vk.inc), NULL headless
    bool attached;          // action sets attached (fake_xr_actions.inc)
};

typedef struct {
    uint32_t magic;
    session *s;
    int hand;  // an action space on a hand's pose: 1 left, 2 right; 0 a reference space
    XrReferenceSpaceType type;
    vr_pose pose;  // poseInReferenceSpace
} space;

static fake_xr_stats_t g_stats;

// the controllers (fake_xr_actions.inc)
static void fxa_script(const char *kind, const char *path, float x, float y);
static void fxa_reset(void);

FAKE_XR_EXPORT void fake_xr_stats(fake_xr_stats_t *out) { *out = g_stats; }

static void logf_(instance *in, const char *fmt, ...) {
    if (!in || !in->cfg.log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(in->cfg.log, fmt, ap);
    va_end(ap);
    fflush(in->cfg.log);
}

static const char *state_name(XrSessionState s) {
    switch (s) {
#define FXR_STATE_CASE(name, val) case name: return #name;
        XR_LIST_ENUM_XrSessionState(FXR_STATE_CASE)
#undef FXR_STATE_CASE
    default: return "XR_SESSION_STATE_?";
    }
}

static vr_pose euler_pose(float x, float y, float z, float yaw, float pitch, float roll) {
    vr_quat q = vr_quat_mul(vr_quat_yaw(yaw * DEG),
                            vr_quat_mul(vr_quat_axis_angle(vr_v3(1, 0, 0), pitch * DEG),
                                        vr_quat_axis_angle(vr_v3(0, 0, 1), roll * DEG)));
    vr_pose p = {vr_quat_normalize(q), {x, y, z}};
    return p;
}

static bool load_script(config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    c->lines = calloc(MAX_SCRIPT, sizeof *c->lines);
    char buf[512];
    int lineno = 0;
    while (fgets(buf, sizeof buf, f) && c->count < MAX_SCRIPT) {
        lineno++;
        char *hash = strchr(buf, '#');
        if (hash) *hash = 0;
        double t;
        char word[32];
        int used = 0;
        if (sscanf(buf, " %lf %31s %n", &t, word, &used) < 2) continue;  // a blank or comment line
        script_line *l = &c->lines[c->count];
        l->t = t;
        if (!strcmp(word, "head")) {
            float v[6];
            if (sscanf(buf + used, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
                fprintf(stderr, "fake_xr: %s:%d: head needs x y z yaw pitch roll\n", path, lineno);
                continue;
            }
            l->kind = EV_HEAD;
            l->head = euler_pose(v[0], v[1], v[2], v[3], v[4], v[5]);
        } else if (!strcmp(word, "unfocus")) l->kind = EV_UNFOCUS;
        else if (!strcmp(word, "focus")) l->kind = EV_FOCUS;
        else if (!strcmp(word, "exit")) l->kind = EV_EXIT;
        else if (!strcmp(word, "lose")) l->kind = EV_LOSE;
        else if (!strcmp(word, "hand")) {  // (a key pose of a hand's track, not an event: interpolated in between)
            char side[8];
            float v[6];
            if (sscanf(buf + used, "%7s %f %f %f %f %f %f", side, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7 ||
                (strcmp(side, "left") && strcmp(side, "right"))) {
                fprintf(stderr, "fake_xr: %s:%d: hand needs left|right x y z yaw pitch roll\n", path, lineno);
                continue;
            }
            int h = !strcmp(side, "right");
            if (c->hands[h].n < 1024) {
                c->hands[h].t[c->hands[h].n] = t;
                c->hands[h].p[c->hands[h].n++] = euler_pose(v[0], v[1], v[2], v[3], v[4], v[5]);
            }
            continue;
        }
        else if (!strcmp(word, "button") || !strcmp(word, "value") || !strcmp(word, "stick")) {
            float x = 0, y = 0;
            int want = !strcmp(word, "stick") ? 3 : 2;
            if (sscanf(buf + used, "%95s %f %f", l->path, &x, &y) < want) {
                fprintf(stderr, "fake_xr: %s:%d: %s needs a path and %s\n", path, lineno, word,
                        want == 3 ? "x y" : "a value");
                continue;
            }
            l->kind = EV_INPUT;
            snprintf(l->input_kind, sizeof l->input_kind, "%s", word);
            l->x = x, l->y = y;
        } else {
            fprintf(stderr, "fake_xr: %s:%d: unknown event '%s'\n", path, lineno, word);
            continue;
        }
        c->count++;
    }
    fclose(f);
    return true;
}

// ---- events ----
static void queue_state(session *s, XrSessionState st) {
    if (s->ev_count == MAX_EVENTS) return;
    s->events[(s->ev_head + s->ev_count++) % MAX_EVENTS] = (pending_event){0, st};
    s->queued = st;
}

// down from where the queue is to SYNCHRONIZED, then STOPPING (xrRequestExitSession, a scripted exit)
static void queue_stop(session *s) {
    if (s->queued == XR_SESSION_STATE_STOPPING || s->queued == XR_SESSION_STATE_IDLE ||
        s->queued == XR_SESSION_STATE_EXITING || s->queued == XR_SESSION_STATE_LOSS_PENDING)
        return;
    if (s->queued == XR_SESSION_STATE_FOCUSED) queue_state(s, XR_SESSION_STATE_VISIBLE);
    if (s->queued == XR_SESSION_STATE_VISIBLE || s->queued == XR_SESSION_STATE_READY)
        queue_state(s, XR_SESSION_STATE_SYNCHRONIZED);
    queue_state(s, XR_SESSION_STATE_STOPPING);
}

// ---- argument checks ----
static instance *get_instance(XrInstance h) {
    instance *in = (instance *)(uintptr_t)h;
    return in && in->magic == MAGIC_INSTANCE ? in : NULL;
}
static session *get_session(XrSession h) {
    session *s = (session *)(uintptr_t)h;
    return s && s->magic == MAGIC_SESSION ? s : NULL;
}
static space *get_space(XrSpace h) {
    space *sp = (space *)(uintptr_t)h;
    return sp && sp->magic == MAGIC_SPACE ? sp : NULL;
}

// the two-call idiom: capacity 0 asks for the count
#define ENUMERATE(cap_in, count_out, array, items, n)                     \
    do {                                                                  \
        if (!(count_out)) return XR_ERROR_VALIDATION_FAILURE;             \
        *(count_out) = (n);                                               \
        if ((cap_in) == 0) return XR_SUCCESS;                             \
        if ((cap_in) < (n)) return XR_ERROR_SIZE_INSUFFICIENT;            \
        if (!(array)) return XR_ERROR_VALIDATION_FAILURE;                 \
        for (uint32_t i_ = 0; i_ < (n); i_++) (array)[i_] = (items)[i_];  \
    } while (0)

// ---- instance ----
static const char *const extensions[] = {XR_MND_HEADLESS_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
#define EXTENSION_COUNT (sizeof extensions / sizeof *extensions)

static XrResult XRAPI_CALL fx_EnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *count,
                                                                    XrExtensionProperties *props) {
    if (layer && *layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    XrExtensionProperties items[EXTENSION_COUNT];
    for (uint32_t i = 0; i < EXTENSION_COUNT; i++) {
        items[i] = (XrExtensionProperties){XR_TYPE_EXTENSION_PROPERTIES};
        snprintf(items[i].extensionName, sizeof items[i].extensionName, "%s", extensions[i]);
        items[i].extensionVersion = 1;
    }
    if (props)
        for (uint32_t i = 0; i < cap && i < EXTENSION_COUNT; i++)
            if (props[i].type != XR_TYPE_EXTENSION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    ENUMERATE(cap, count, props, items, (uint32_t)EXTENSION_COUNT);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_CreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out) {
    if (!ci || !out || ci->type != XR_TYPE_INSTANCE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (XR_VERSION_MAJOR(ci->applicationInfo.apiVersion) != 1) return XR_ERROR_API_VERSION_UNSUPPORTED;
    if (!ci->applicationInfo.applicationName[0]) return XR_ERROR_NAME_INVALID;
    if (ci->enabledApiLayerCount) return XR_ERROR_API_LAYER_NOT_PRESENT;
    bool headless = false, vulkan = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        bool known = false;
        for (uint32_t k = 0; k < EXTENSION_COUNT; k++)
            if (!strcmp(ci->enabledExtensionNames[i], extensions[k])) known = true;
        if (!known) return XR_ERROR_EXTENSION_NOT_PRESENT;
        if (!strcmp(ci->enabledExtensionNames[i], XR_MND_HEADLESS_EXTENSION_NAME)) headless = true;
        if (!strcmp(ci->enabledExtensionNames[i], XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) vulkan = true;
    }
    instance *in = calloc(1, sizeof *in);
    in->magic = MAGIC_INSTANCE;
    fxa_reset();
    in->headless = headless;
    in->vulkan = vulkan;
    in->cfg.ipd = FAKE_XR_DEFAULT_IPD;
    in->cfg.hz = FAKE_XR_DEFAULT_HZ;
    const char *v;
    if ((v = getenv("FAKE_XR_IPD")) && atof(v) >= 0) in->cfg.ipd = (float)atof(v);
    if ((v = getenv("FAKE_XR_HZ")) && atoi(v) > 0) in->cfg.hz = atoi(v);
    if ((v = getenv("FAKE_XR_LOG")) && *v) in->cfg.log = fopen(v, "w");
    if ((v = getenv("FAKE_XR_CAPTURE")) && *v) {
        snprintf(in->cfg.capture_dir, sizeof in->cfg.capture_dir, "%s", v);
        in->cfg.capture_every = 30;
    }
    if ((v = getenv("FAKE_XR_CAPTURE_EVERY")) && *v) in->cfg.capture_every = atoi(v);
    if ((v = getenv("FAKE_XR_SCRIPT")) && *v && !load_script(&in->cfg, v)) {
        fprintf(stderr, "fake_xr: can't read FAKE_XR_SCRIPT %s\n", v);
        if (in->cfg.log) fclose(in->cfg.log);
        free(in);
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    logf_(in, "instance app=%s hz=%d ipd=%.4f script_lines=%d headless=%d vulkan=%d\n",
          ci->applicationInfo.applicationName, in->cfg.hz, in->cfg.ipd, in->cfg.count, headless, vulkan);
    g_stats.instances++;
    *out = (XrInstance)(uintptr_t)in;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_DestroySession(XrSession h);

static XrResult XRAPI_CALL fx_DestroyInstance(XrInstance h) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (in->session) fx_DestroySession((XrSession)(uintptr_t)in->session);
    logf_(in, "instance destroyed\n");
    if (in->cfg.log) fclose(in->cfg.log);
    free(in->cfg.lines);
    in->magic = 0;
    free(in);
    g_stats.instances--;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetInstanceProperties(XrInstance h, XrInstanceProperties *p) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (!p || p->type != XR_TYPE_INSTANCE_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    snprintf(p->runtimeName, sizeof p->runtimeName, "%s", FAKE_XR_RUNTIME_NAME);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_ResultToString(XrInstance h, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    switch (r) {
#define FXR_RESULT_CASE(name, val) case name: snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "%.*s", XR_MAX_RESULT_STRING_SIZE - 1, #name); break;
        XR_LIST_ENUM_XrResult(FXR_RESULT_CASE)
#undef FXR_RESULT_CASE
    default: snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XR_%s_%d", r < 0 ? "UNKNOWN_FAILURE" : "UNKNOWN_SUCCESS", (int)r);
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_StructureTypeToString(XrInstance h, XrStructureType t,
                                                     char buf[XR_MAX_STRUCTURE_NAME_SIZE]) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    switch (t) {
#define FXR_TYPE_CASE(name, val) case name: snprintf(buf, XR_MAX_STRUCTURE_NAME_SIZE, "%.*s", XR_MAX_STRUCTURE_NAME_SIZE - 1, #name); break;
        XR_LIST_ENUM_XrStructureType(FXR_TYPE_CASE)
#undef FXR_TYPE_CASE
    default: snprintf(buf, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (int)t);
    }
    return XR_SUCCESS;
}

// ---- system ----
static XrResult XRAPI_CALL fx_GetSystem(XrInstance h, const XrSystemGetInfo *gi, XrSystemId *id) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (!gi || !id || gi->type != XR_TYPE_SYSTEM_GET_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *id = FAKE_XR_SYSTEM_ID;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetSystemProperties(XrInstance h, XrSystemId id, XrSystemProperties *p) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (id != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (!p || p->type != XR_TYPE_SYSTEM_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    p->systemId = id;
    p->vendorId = 0;
    snprintf(p->systemName, sizeof p->systemName, "%s HMD", FAKE_XR_RUNTIME_NAME);
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxLayerCount = FAKE_XR_MAX_LAYERS;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_EnumerateViewConfigurations(XrInstance h, XrSystemId id, uint32_t cap, uint32_t *count,
                                                           XrViewConfigurationType *types) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (id != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    const XrViewConfigurationType items[] = {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    ENUMERATE(cap, count, types, items, 1u);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetViewConfigurationProperties(XrInstance h, XrSystemId id, XrViewConfigurationType t,
                                                              XrViewConfigurationProperties *p) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (id != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (!p || p->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    p->viewConfigurationType = t;
    p->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_EnumerateViewConfigurationViews(XrInstance h, XrSystemId id, XrViewConfigurationType t,
                                                               uint32_t cap, uint32_t *count,
                                                               XrViewConfigurationView *views) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (id != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    XrViewConfigurationView v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    v.recommendedImageRectWidth = FAKE_XR_VIEW_W;
    v.recommendedImageRectHeight = FAKE_XR_VIEW_H;
    v.maxImageRectWidth = v.maxImageRectHeight = 4096;
    v.recommendedSwapchainSampleCount = 1;
    v.maxSwapchainSampleCount = 4;
    const XrViewConfigurationView items[2] = {v, v};
    if (views)
        for (uint32_t i = 0; i < cap && i < 2; i++)
            if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) return XR_ERROR_VALIDATION_FAILURE;
    ENUMERATE(cap, count, views, items, 2u);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_EnumerateEnvironmentBlendModes(XrInstance h, XrSystemId id, XrViewConfigurationType t,
                                                              uint32_t cap, uint32_t *count,
                                                              XrEnvironmentBlendMode *modes) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (id != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    const XrEnvironmentBlendMode items[] = {XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    ENUMERATE(cap, count, modes, items, 1u);
    return XR_SUCCESS;
}

// ---- the graphics part (fake_xr_vk.inc) ----
static XrResult fxvk_session_create(session *s, const XrGraphicsBindingVulkanKHR *b);
static void fxvk_session_destroy(session *s);
static XrResult fxvk_check_layer(session *s, const XrCompositionLayerBaseHeader *l);
static void fxvk_capture(session *s, const XrFrameEndInfo *ei, uint64_t frame);

// ---- session ----
static XrResult XRAPI_CALL fx_CreateSession(XrInstance h, const XrSessionCreateInfo *ci, XrSession *out) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_SESSION_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->systemId != FAKE_XR_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    if (in->session) return XR_ERROR_LIMIT_REACHED;  // one session per instance, as most runtimes
    const XrGraphicsBindingVulkanKHR *binding = NULL;
    for (const XrBaseInStructure *n = ci->next; n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR && in->vulkan) binding = (const XrGraphicsBindingVulkanKHR *)n;
        else return XR_ERROR_GRAPHICS_DEVICE_INVALID;  // a binding of an API this runtime has no extension enabled for
    }
    if (!binding && !in->headless) return XR_ERROR_GRAPHICS_DEVICE_INVALID;  // no binding: only with XR_MND_headless
    session *s = calloc(1, sizeof *s);
    s->magic = MAGIC_SESSION;
    s->inst = in;
    if (binding) {
        XrResult r = fxvk_session_create(s, binding);
        if (XR_FAILED(r)) {
            free(s);
            return r;
        }
    }
    s->state = s->queued = XR_SESSION_STATE_UNKNOWN;
    s->head = euler_pose(0, FAKE_XR_HEAD_HEIGHT, 0, 0, 0, 0);
    in->session = s;
    queue_state(s, XR_SESSION_STATE_IDLE);
    queue_state(s, XR_SESSION_STATE_READY);
    g_stats.sessions++;
    g_stats.state = s->state;
    *out = (XrSession)(uintptr_t)s;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_DestroySession(XrSession h) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    logf_(s->inst, "session destroyed\n");
    fxvk_session_destroy(s);
    s->inst->session = NULL;
    s->magic = 0;
    free(s);
    g_stats.sessions--;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_PollEvent(XrInstance h, XrEventDataBuffer *buf) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!buf || buf->type != XR_TYPE_EVENT_DATA_BUFFER) return XR_ERROR_VALIDATION_FAILURE;
    session *s = in->session;
    if (!s || !s->ev_count) return XR_EVENT_UNAVAILABLE;
    pending_event e = s->events[s->ev_head];
    s->ev_head = (s->ev_head + 1) % MAX_EVENTS;
    s->ev_count--;
    XrEventDataSessionStateChanged *sc = (XrEventDataSessionStateChanged *)buf;
    memset(buf, 0, sizeof *buf);
    sc->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    sc->session = (XrSession)(uintptr_t)s;
    sc->state = e.state;
    sc->time = s->last_predicted ? s->last_predicted : BASE_TIME;
    s->state = e.state;
    g_stats.state = e.state;
    logf_(in, "state %s\n", state_name(e.state));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_BeginSession(XrSession h, const XrSessionBeginInfo *bi) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!bi || bi->type != XR_TYPE_SESSION_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (s->running) return XR_ERROR_SESSION_RUNNING;
    if (s->state != XR_SESSION_STATE_READY) return XR_ERROR_SESSION_NOT_READY;
    s->running = true;
    logf_(s->inst, "session begun\n");
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_RequestExitSession(XrSession h) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    queue_stop(s);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_EndSession(XrSession h) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->state != XR_SESSION_STATE_STOPPING) return XR_ERROR_SESSION_NOT_STOPPING;
    s->running = false;
    s->begun = false;
    s->waited = 0;
    logf_(s->inst, "session ended\n");
    queue_state(s, XR_SESSION_STATE_IDLE);
    queue_state(s, XR_SESSION_STATE_EXITING);
    return XR_SUCCESS;
}

// ---- frames ----
static XrDuration period_ns(session *s) { return (XrDuration)(1000000000ll / s->inst->cfg.hz); }

static void run_script(session *s, XrTime display) {
    config *c = &s->inst->cfg;
    double t = (double)(display - s->first_time) / 1e9;
    while (s->script_at < c->count && c->lines[s->script_at].t <= t + 1e-9) {
        script_line *l = &c->lines[s->script_at++];
        switch (l->kind) {
        case EV_HEAD: s->head = l->head; break;
        case EV_UNFOCUS:
            if (s->queued == XR_SESSION_STATE_FOCUSED) queue_state(s, XR_SESSION_STATE_VISIBLE);
            break;
        case EV_FOCUS:
            if (s->queued == XR_SESSION_STATE_VISIBLE) queue_state(s, XR_SESSION_STATE_FOCUSED);
            break;
        case EV_EXIT: queue_stop(s); break;
        case EV_INPUT: fxa_script(l->input_kind, l->path, l->x, l->y); break;
        case EV_LOSE:
            if (!s->lost) {
                s->lost = true;
                queue_state(s, XR_SESSION_STATE_LOSS_PENDING);
                logf_(s->inst, "session lost t=%.3f\n", t);
            }
            break;
        }
    }
}

static XrResult XRAPI_CALL fx_WaitFrame(XrSession h, const XrFrameWaitInfo *wi, XrFrameState *fs) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if ((wi && wi->type != XR_TYPE_FRAME_WAIT_INFO) || !fs || fs->type != XR_TYPE_FRAME_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    XrTime display = BASE_TIME + (XrTime)(s->frame + 1) * period_ns(s);
    if (!s->frame) s->first_time = display;
    s->frame++;
    s->last_predicted = display;
    s->waited++;
    run_script(s, display);
    fs->predictedDisplayTime = display;
    fs->predictedDisplayPeriod = period_ns(s);
    fs->shouldRender = s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED;
    g_stats.frames_waited++;
    return s->lost ? XR_SESSION_LOSS_PENDING : XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_BeginFrame(XrSession h, const XrFrameBeginInfo *bi) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (bi && bi->type != XR_TYPE_FRAME_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->waited <= 0) return XR_ERROR_CALL_ORDER_INVALID;
    s->waited--;
    if (s->begun) {  // the frame begun before is dropped
        g_stats.frames_discarded++;
        logf_(s->inst, "frame discarded\n");
        return XR_FRAME_DISCARDED;
    }
    s->begun = true;
    return XR_SUCCESS;
}

static const char *layer_kind(XrStructureType t) {
    switch (t) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return "projection";
    case XR_TYPE_COMPOSITION_LAYER_QUAD: return "quad";
    default: return "other";
    }
}

static XrResult XRAPI_CALL fx_EndFrame(XrSession h, const XrFrameEndInfo *ei) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ei || ei->type != XR_TYPE_FRAME_END_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (!s->begun) return XR_ERROR_CALL_ORDER_INVALID;
    if (ei->displayTime <= 0 || ei->displayTime > s->last_predicted ||
        (ei->displayTime - BASE_TIME) % period_ns(s) != 0)
        return XR_ERROR_TIME_INVALID;
    if (ei->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    if (ei->layerCount > FAKE_XR_MAX_LAYERS) return XR_ERROR_LAYER_LIMIT_EXCEEDED;
    if (ei->layerCount && !ei->layers) return XR_ERROR_VALIDATION_FAILURE;
    char kinds[256] = "";
    for (uint32_t i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader *l = ei->layers[i];
        if (!l) return XR_ERROR_VALIDATION_FAILURE;
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
            ((const XrCompositionLayerProjection *)l)->viewCount != 2)
            return XR_ERROR_VALIDATION_FAILURE;
        if (!get_space(l->space)) return XR_ERROR_HANDLE_INVALID;
        if (!s->vk) return XR_ERROR_LAYER_INVALID;  // (headless: nothing to show)
        XrResult lr = fxvk_check_layer(s, l);
        if (XR_FAILED(lr)) return lr;
        size_t n = strlen(kinds);
        snprintf(kinds + n, sizeof kinds - n, "%s%s", n ? "," : "", layer_kind(l->type));
    }
    s->begun = false;
    g_stats.frames_ended++;
    fxvk_capture(s, ei, g_stats.frames_ended);
    g_stats.last_layer_count = ei->layerCount;
    logf_(s->inst, "frame %llu display_time %lld layers %u %s\n", (unsigned long long)s->frame,
          (long long)ei->displayTime, ei->layerCount, kinds);
    if (!s->synced && s->state == XR_SESSION_STATE_READY) {  // the first frame: the session comes to the front
        s->synced = true;
        queue_state(s, XR_SESSION_STATE_SYNCHRONIZED);
        queue_state(s, XR_SESSION_STATE_VISIBLE);
        queue_state(s, XR_SESSION_STATE_FOCUSED);
    }
    return XR_SUCCESS;
}

// ---- spaces ----
static XrResult XRAPI_CALL fx_EnumerateReferenceSpaces(XrSession h, uint32_t cap, uint32_t *count,
                                                        XrReferenceSpaceType *types) {
    if (!get_session(h)) return XR_ERROR_HANDLE_INVALID;
    const XrReferenceSpaceType items[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                          XR_REFERENCE_SPACE_TYPE_STAGE};
    ENUMERATE(cap, count, types, items, 3u);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_CreateReferenceSpace(XrSession h, const XrReferenceSpaceCreateInfo *ci, XrSpace *out) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW && ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE)
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    const XrPosef *p = &ci->poseInReferenceSpace;
    float qq = p->orientation.x * p->orientation.x + p->orientation.y * p->orientation.y +
               p->orientation.z * p->orientation.z + p->orientation.w * p->orientation.w;
    if (qq < 0.99f || qq > 1.01f) return XR_ERROR_POSE_INVALID;
    space *sp = calloc(1, sizeof *sp);
    sp->magic = MAGIC_SPACE;
    sp->s = s;
    sp->type = ci->referenceSpaceType;
    sp->pose = (vr_pose){{p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w},
                         {p->position.x, p->position.y, p->position.z}};
    *out = (XrSpace)(uintptr_t)sp;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_DestroySpace(XrSpace h) {
    space *sp = get_space(h);
    if (!sp) return XR_ERROR_HANDLE_INVALID;
    sp->magic = 0;
    free(sp);
    return XR_SUCCESS;
}

// a hand at time t (the script's track, interpolated; still where no track says otherwise)
static vr_pose hand_at(session *s, int h, XrTime t) {
    config *c = &s->inst->cfg;
    int n = c->hands[h].n;
    if (!n) return euler_pose(h ? 0.2f : -0.2f, 1.3f, -0.3f, 0, 0, 0);
    double ts = s->first_time ? (double)(t - s->first_time) / 1e9 : 0;
    const double *tt = c->hands[h].t;
    const vr_pose *pp = c->hands[h].p;
    if (ts <= tt[0]) return pp[0];
    if (ts >= tt[n - 1]) return pp[n - 1];
    int i = 0;
    while (i + 1 < n && tt[i + 1] <= ts) i++;
    float k = (float)((ts - tt[i]) / (tt[i + 1] - tt[i]));
    vr_pose a = pp[i], b = pp[i + 1], r;
    r.p = vr_v3_add(vr_v3_scale(a.p, 1 - k), vr_v3_scale(b.p, k));
    vr_quat qb = b.q;  // (nlerp the short way)
    if (a.q.x * qb.x + a.q.y * qb.y + a.q.z * qb.z + a.q.w * qb.w < 0) qb = (vr_quat){-qb.x, -qb.y, -qb.z, -qb.w};
    r.q = vr_quat_normalize((vr_quat){a.q.x * (1 - k) + qb.x * k, a.q.y * (1 - k) + qb.y * k,
                                      a.q.z * (1 - k) + qb.z * k, a.q.w * (1 - k) + qb.w * k});
    return r;
}

// where a space is in STAGE space at time t (LOCAL is the same in this runtime: the floor at the origin)
static vr_pose space_at(space *sp, XrTime t) {
    vr_pose ref = sp->hand ? hand_at(sp->s, sp->hand - 1, t)
                : sp->type == XR_REFERENCE_SPACE_TYPE_VIEW ? sp->s->head : vr_pose_identity();
    return vr_pose_mul(ref, sp->pose);
}

static vr_pose space_in_stage(space *sp) { return space_at(sp, sp->s->last_predicted ? sp->s->last_predicted : 1); }

static XrPosef to_xr(vr_pose p) {
    XrPosef r = {{p.q.x, p.q.y, p.q.z, p.q.w}, {p.p.x, p.p.y, p.p.z}};
    return r;
}

static XrResult XRAPI_CALL fx_LocateSpace(XrSpace h, XrSpace base, XrTime t, XrSpaceLocation *loc) {
    space *sp = get_space(h), *bs = get_space(base);
    if (!sp || !bs) return XR_ERROR_HANDLE_INVALID;
    if (!loc || loc->type != XR_TYPE_SPACE_LOCATION) return XR_ERROR_VALIDATION_FAILURE;
    if (t <= 0) return XR_ERROR_TIME_INVALID;
    vr_pose now = vr_pose_mul(vr_pose_inverse(space_at(bs, t)), space_at(sp, t));
    loc->pose = to_xr(now);
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    for (XrBaseOutStructure *n = (XrBaseOutStructure *)loc->next; n; n = n->next) {
        if (n->type != XR_TYPE_SPACE_VELOCITY) continue;
        // the motion over the last 5 ms, in the base space (as a runtime gives it)
        const XrTime dt = 5000000;
        vr_pose before = vr_pose_mul(vr_pose_inverse(space_at(bs, t - dt)), space_at(sp, t - dt));
        XrSpaceVelocity *v = (XrSpaceVelocity *)n;
        vr_vec3 lin = vr_v3_scale(vr_v3_sub(now.p, before.p), 1e9f / dt);
        vr_quat dq = vr_quat_normalize(vr_quat_mul(now.q, vr_quat_conj(before.q)));
        if (dq.w < 0) dq = (vr_quat){-dq.x, -dq.y, -dq.z, -dq.w};
        float s2 = sqrtf(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
        float angle = 2 * atan2f(s2, dq.w);
        vr_vec3 ang = s2 > 1e-7f ? vr_v3_scale(vr_v3(dq.x, dq.y, dq.z), angle / s2 * 1e9f / dt) : vr_v3(0, 0, 0);
        v->linearVelocity = (XrVector3f){lin.x, lin.y, lin.z};
        v->angularVelocity = (XrVector3f){ang.x, ang.y, ang.z};
        v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_LocateViews(XrSession h, const XrViewLocateInfo *li, XrViewState *vs, uint32_t cap,
                                          uint32_t *count, XrView *views) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!li || li->type != XR_TYPE_VIEW_LOCATE_INFO || !vs || vs->type != XR_TYPE_VIEW_STATE || !count)
        return XR_ERROR_VALIDATION_FAILURE;
    if (li->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    space *bs = get_space(li->space);
    if (!bs) return XR_ERROR_HANDLE_INVALID;
    if (li->displayTime <= 0) return XR_ERROR_TIME_INVALID;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!views || views[0].type != XR_TYPE_VIEW || views[1].type != XR_TYPE_VIEW) return XR_ERROR_VALIDATION_FAILURE;
    vr_pose base_inv = vr_pose_inverse(space_in_stage(bs));
    float half = s->inst->cfg.ipd * 0.5f;
    for (int eye = 0; eye < 2; eye++) {
        vr_pose offset = {vr_quat_identity(), {eye ? half : -half, 0, 0}};
        views[eye].pose = to_xr(vr_pose_mul(base_inv, vr_pose_mul(s->head, offset)));
        views[eye].fov.angleLeft = eye ? -FAKE_XR_FOV_RIGHT : FAKE_XR_FOV_LEFT;
        views[eye].fov.angleRight = eye ? -FAKE_XR_FOV_LEFT : FAKE_XR_FOV_RIGHT;
        views[eye].fov.angleUp = FAKE_XR_FOV_UP;
        views[eye].fov.angleDown = FAKE_XR_FOV_DOWN;
    }
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                         XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

#include "fake_xr_vk.inc"
#include "fake_xr_actions.inc"

static void fxa_reset(void) { memset(&ctl, 0, sizeof ctl); }

// ---- the loader's entry points ----
typedef struct {
    const char *name;
    PFN_xrVoidFunction fn;
} proc;

#define P(n) {"xr" #n, (PFN_xrVoidFunction)fx_##n}
static const proc procs[] = {
    P(EnumerateInstanceExtensionProperties), P(CreateInstance), P(DestroyInstance), P(GetInstanceProperties),
    P(ResultToString), P(StructureTypeToString), P(GetSystem), P(GetSystemProperties),
    P(EnumerateViewConfigurations), P(GetViewConfigurationProperties), P(EnumerateViewConfigurationViews),
    P(EnumerateEnvironmentBlendModes), P(CreateSession), P(DestroySession), P(PollEvent), P(BeginSession),
    P(RequestExitSession), P(EndSession), P(WaitFrame), P(BeginFrame), P(EndFrame), P(EnumerateReferenceSpaces),
    P(CreateReferenceSpace), P(DestroySpace), P(LocateSpace), P(LocateViews),
    P(GetVulkanGraphicsRequirements2KHR), P(CreateVulkanInstanceKHR), P(GetVulkanGraphicsDevice2KHR),
    P(CreateVulkanDeviceKHR), P(EnumerateSwapchainFormats), P(CreateSwapchain), P(DestroySwapchain),
    P(EnumerateSwapchainImages), P(AcquireSwapchainImage), P(WaitSwapchainImage), P(ReleaseSwapchainImage),
    P(StringToPath), P(PathToString), P(CreateActionSet), P(DestroyActionSet), P(CreateAction), P(DestroyAction),
    P(SuggestInteractionProfileBindings), P(AttachSessionActionSets), P(SyncActions), P(GetActionStateBoolean),
    P(GetActionStateFloat), P(GetActionStateVector2f), P(GetCurrentInteractionProfile), P(GetActionStatePose),
    P(CreateActionSpace),
};
#undef P

static XrResult XRAPI_CALL fx_GetInstanceProcAddr(XrInstance h, const char *name, PFN_xrVoidFunction *fn) {
    if (!name || !fn) return XR_ERROR_VALIDATION_FAILURE;
    *fn = NULL;
    if (!strcmp(name, "xrGetInstanceProcAddr")) {
        *fn = (PFN_xrVoidFunction)fx_GetInstanceProcAddr;
        return XR_SUCCESS;
    }
    // without an instance only these three are allowed
    if (h == XR_NULL_HANDLE && strcmp(name, "xrEnumerateInstanceExtensionProperties") &&
        strcmp(name, "xrEnumerateApiLayerProperties") && strcmp(name, "xrCreateInstance"))
        return XR_ERROR_HANDLE_INVALID;
    if (h != XR_NULL_HANDLE && !get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    for (size_t i = 0; i < sizeof procs / sizeof *procs; i++)
        if (!strcmp(name, procs[i].name)) {
            *fn = procs[i].fn;
            return XR_SUCCESS;
        }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

FAKE_XR_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *li,
                                                                      XrNegotiateRuntimeRequest *rr) {
    if (!li || !rr || li->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        li->structVersion != XR_LOADER_INFO_STRUCT_VERSION || li->structSize != sizeof *li ||
        rr->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        rr->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || rr->structSize != sizeof *rr)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (li->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        li->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (XR_VERSION_MAJOR(li->minApiVersion) > 1 || XR_VERSION_MAJOR(li->maxApiVersion) < 1)
        return XR_ERROR_INITIALIZATION_FAILED;
    rr->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    rr->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    rr->getInstanceProcAddr = fx_GetInstanceProcAddr;
    return XR_SUCCESS;
}
