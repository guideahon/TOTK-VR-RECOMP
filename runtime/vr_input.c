// TotK Recomp VR (ported from Savage Wind VR, runtime/vr/vr_input.c): the VR controllers as the game's controller. An OpenXR action set with an action per control the
// game reads, bindings suggested for the common controllers, and each poll of the game's input (runtime/hle/input.c,
// where the test script's input is merged too) reads them, turns them into the game's buttons and sticks
// (vr_input_map.h) and adds them to the gamepad's and keyboard's. A gamepad keeps working beside them.
//
// Phase 8, motion combat (TOTK_VR_MOTION, on): the hands' poses too. The right hand's slashes and stabs
// (vr_melee.h, BetterVR's analysis) press attack (Y) as they begin; the left hand raised in front of the face holds
// the guard (ZL). The buttons keep working.
#ifdef TOTK_VR
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#include <openxr/openxr.h>

#include "vr.h"
#include "vr_input_map.h"
#include "vr_internal.h"
#include "vr_melee.h"

enum {
    A_MOVE, A_CAMERA, A_A, A_B, A_X, A_Y, A_LCLICK, A_RCLICK, A_MENU, A_LTRIGGER, A_RTRIGGER, A_LGRIP, A_RGRIP, A_HAND,
    A_COUNT
};
static const struct {
    const char *name, *label;
    XrActionType type;
} defs[A_COUNT] = {
    {"move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {"camera", "Camera", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {"a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"crouch", "Crouch (left stick)", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"look", "Look (right stick)", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"menu", "Menu (+; held: -)", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"zl", "ZL", XR_ACTION_TYPE_FLOAT_INPUT},
    {"zr", "ZR", XR_ACTION_TYPE_FLOAT_INPUT},
    {"l", "L (with the right stick: d-pad)", XR_ACTION_TYPE_FLOAT_INPUT},
    {"r", "R", XR_ACTION_TYPE_FLOAT_INPUT},
    {"hand", "Hand pose (motion combat)", XR_ACTION_TYPE_POSE_INPUT},
};

// bindings per controller: action, path under /user/hand/
typedef struct {
    int action;
    const char *path;
} bind;
static const bind touch[] = {
    {A_MOVE, "left/input/thumbstick"}, {A_CAMERA, "right/input/thumbstick"}, {A_A, "right/input/a/click"},
    {A_B, "right/input/b/click"}, {A_X, "left/input/x/click"}, {A_Y, "left/input/y/click"},
    {A_LCLICK, "left/input/thumbstick/click"}, {A_RCLICK, "right/input/thumbstick/click"},
    {A_MENU, "left/input/menu/click"}, {A_LTRIGGER, "left/input/trigger/value"},
    {A_RTRIGGER, "right/input/trigger/value"}, {A_LGRIP, "left/input/squeeze/value"},
    {A_RGRIP, "right/input/squeeze/value"}, {A_HAND, "left/input/grip/pose"}, {A_HAND, "right/input/grip/pose"},
    {-1, NULL},
};
static const bind index_[] = {  // (Valve Index: the left A / B are X / Y; the left trackpad pressed is the menu)
    {A_MOVE, "left/input/thumbstick"}, {A_CAMERA, "right/input/thumbstick"}, {A_A, "right/input/a/click"},
    {A_B, "right/input/b/click"}, {A_X, "left/input/a/click"}, {A_Y, "left/input/b/click"},
    {A_LCLICK, "left/input/thumbstick/click"}, {A_RCLICK, "right/input/thumbstick/click"},
    {A_MENU, "left/input/trackpad/force"}, {A_LTRIGGER, "left/input/trigger/value"},
    {A_RTRIGGER, "right/input/trigger/value"}, {A_LGRIP, "left/input/squeeze/value"},
    {A_RGRIP, "right/input/squeeze/value"}, {A_HAND, "left/input/grip/pose"}, {A_HAND, "right/input/grip/pose"},
    {-1, NULL},
};
static const bind wmr[] = {  // (Windows Mixed Reality: the touchpads pressed are A / X, the right menu B)
    {A_MOVE, "left/input/thumbstick"}, {A_CAMERA, "right/input/thumbstick"}, {A_A, "right/input/trackpad/click"},
    {A_B, "right/input/menu/click"}, {A_X, "left/input/trackpad/click"},
    {A_LCLICK, "left/input/thumbstick/click"}, {A_RCLICK, "right/input/thumbstick/click"},
    {A_MENU, "left/input/menu/click"}, {A_LTRIGGER, "left/input/trigger/value"},
    {A_RTRIGGER, "right/input/trigger/value"}, {A_LGRIP, "left/input/squeeze/click"},
    {A_RGRIP, "right/input/squeeze/click"}, {A_HAND, "left/input/grip/pose"}, {A_HAND, "right/input/grip/pose"},
    {-1, NULL},
};
static const bind vive[] = {  // (HTC Vive wands: the touchpads are the sticks, pressed A / X; the right menu B)
    {A_MOVE, "left/input/trackpad"}, {A_CAMERA, "right/input/trackpad"}, {A_A, "right/input/trackpad/click"},
    {A_B, "right/input/menu/click"}, {A_X, "left/input/trackpad/click"}, {A_MENU, "left/input/menu/click"},
    {A_LTRIGGER, "left/input/trigger/value"}, {A_RTRIGGER, "right/input/trigger/value"},
    {A_LGRIP, "left/input/squeeze/click"}, {A_RGRIP, "right/input/squeeze/click"}, {A_HAND, "left/input/grip/pose"},
    {A_HAND, "right/input/grip/pose"}, {-1, NULL},
};
static const bind simple[] = {  // (the minimal one every runtime has: select is A, the left menu the menu)
    {A_A, "right/input/select/click"}, {A_MENU, "left/input/menu/click"}, {A_HAND, "left/input/grip/pose"},
    {A_HAND, "right/input/grip/pose"}, {-1, NULL},
};
static const struct {
    const char *profile;
    const bind *b;
} profiles[] = {
    {"/interaction_profiles/oculus/touch_controller", touch},
    {"/interaction_profiles/valve/index_controller", index_},
    {"/interaction_profiles/microsoft/motion_controller", wmr},
    {"/interaction_profiles/htc/vive_controller", vive},
    {"/interaction_profiles/khr/simple_controller", simple},
};

static struct {
    bool ready;
    XrSession session;
    XrActionSet set;
    XrAction a[A_COUNT];
    pthread_mutex_t lock;
    uint64_t synced_ns;
    vr_pad_state pad;
    vr_pad_out out;
    bool active;  // the last sync gave the actions (the session has the focus)
    uint64_t syncs;
    float snap_deg;
    // motion combat
    bool motion;           // TOTK_VR_MOTION (on)
    bool shield_gesture;   // TOTK_VR_SHIELD (on)
    XrSpace hand[2];       // the grips' poses, left and right
    XrPath hand_path[2];
    vr_melee melee;        // the right hand's slashes and stabs
    double attack_until;   // attack (Y) is held until this time (s)
    bool shield;           // the left hand holds the guard (ZL)
    XrTime last_sample;    // (the hands are sampled once per display time)
    uint32_t attacks, slashes, stabs;
    float last_power;
} in = {.lock = PTHREAD_MUTEX_INITIALIZER};

static XrPath path(XrInstance inst, const char *s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

void vr_input_setup(XrSession session) {
    XrInstance inst = vr_xr_instance();
    const char *e = getenv("TOTK_VR_CONTROLLERS");
    if (inst == XR_NULL_HANDLE || (e && *e == '0')) return;
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof si.actionSetName, "gameplay");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Gameplay");
    if (!vr_xr_ok(xrCreateActionSet(inst, &si, &in.set), "xrCreateActionSet")) return;
    in.hand_path[0] = path(inst, "/user/hand/left");
    in.hand_path[1] = path(inst, "/user/hand/right");
    for (int i = 0; i < A_COUNT; i++) {
        XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
        ai.actionType = defs[i].type;
        if (i == A_HAND) ai.countSubactionPaths = 2, ai.subactionPaths = in.hand_path;  // (a space per hand)
        snprintf(ai.actionName, sizeof ai.actionName, "%s", defs[i].name);
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", defs[i].label);
        if (!vr_xr_ok(xrCreateAction(in.set, &ai, &in.a[i]), "xrCreateAction")) return;
    }
    int suggested = 0;
    for (size_t p = 0; p < sizeof profiles / sizeof *profiles; p++) {
        XrActionSuggestedBinding sb[32];
        uint32_t n = 0;
        for (const bind *b = profiles[p].b; b->path && n < 32; b++) {
            char full[128];
            snprintf(full, sizeof full, "/user/hand/%s", b->path);
            sb[n++] = (XrActionSuggestedBinding){in.a[b->action], path(inst, full)};
        }
        XrInteractionProfileSuggestedBinding ps = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL,
                                                   path(inst, profiles[p].profile), n, sb};
        // (a runtime may not know a profile: that one is left out, the rest still count)
        XrResult r = xrSuggestInteractionProfileBindings(inst, &ps);
        if (XR_SUCCEEDED(r)) suggested++;
        else rt_log("vr: controllers: %s left out (%d)", profiles[p].profile, (int)r);
    }
    XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, NULL, 1, &in.set};
    if (!vr_xr_ok(xrAttachSessionActionSets(session, &at), "xrAttachSessionActionSets")) return;
    in.session = session;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sc = {XR_TYPE_ACTION_SPACE_CREATE_INFO, NULL, in.a[A_HAND], in.hand_path[h],
                                      {{0, 0, 0, 1}, {0, 0, 0}}};
        if (!vr_xr_ok(xrCreateActionSpace(session, &sc, &in.hand[h]), "xrCreateActionSpace")) in.hand[h] = XR_NULL_HANDLE;
    }
    // motion combat (TOTK_VR_MOTION=0: off; TOTK_VR_SWING=easy: BetterVR's easier swing profile;
    // TOTK_VR_SHIELD=0: no raised-hand guard)
    const char *mo = getenv("TOTK_VR_MOTION"), *sw = getenv("TOTK_VR_SWING"), *sh = getenv("TOTK_VR_SHIELD");
    in.motion = !(mo && *mo == '0') && in.hand[1] != XR_NULL_HANDLE;
    in.shield_gesture = in.motion && !(sh && *sh == '0') && in.hand[0] != XR_NULL_HANDLE;
    vr_melee_init(&in.melee, sw && !strcmp(sw, "easy"));
    if (in.motion)
        rt_log("vr: motion combat: slashes and stabs of the right hand attack (%s swing profile)%s",
            sw && !strcmp(sw, "easy") ? "easy" : "normal", in.shield_gesture ? "; the left hand raised guards" : "");
    in.ready = true;
    // TOTK_VR_TURN=snap: the right stick turns the view a step at once (TOTK_VR_SNAP_DEGREES, 45), through the head
    // tracking's turn (vr_camera.c), instead of turning the game's camera smoothly
    const char *turn = getenv("TOTK_VR_TURN");
    in.pad.snap = turn && !strcmp(turn, "snap");
    const char *deg = getenv("TOTK_VR_SNAP_DEGREES");
    in.snap_deg = deg && atof(deg) >= 5 && atof(deg) <= 180 ? (float)atof(deg) : 45.0f;
    if (in.pad.snap) rt_log("vr: snap turning: %.0f degrees a step", in.snap_deg);
    rt_log("vr: controllers: %d actions, bindings for %d kinds of controller (TOTK_VR_CONTROLLERS=0: off)",
        A_COUNT, suggested);
}

static bool get_bool(int i) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, in.a[i], XR_NULL_PATH};
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(in.session, &gi, &st)) && st.isActive && st.currentState;
}

static float get_float(int i) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, in.a[i], XR_NULL_PATH};
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(in.session, &gi, &st)) && st.isActive ? st.currentState : 0;
}

static void get_vec(int i, float *x, float *y) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, in.a[i], XR_NULL_PATH};
    XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
    bool ok = XR_SUCCEEDED(xrGetActionStateVector2f(in.session, &gi, &st)) && st.isActive;
    *x = ok ? st.currentState.x : 0, *y = ok ? st.currentState.y : 0;
}

static vr_pose pose_of(XrPosef p) {
    vr_pose r = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
                 {p.position.x, p.position.y, p.position.z}};
    return r;
}

// the hands, once per display time: the right one's slashes and stabs, the left one raised (in.out gets Y / ZL)
static void motion(double now) {
    XrTime t = vr_next_display_time();
    if (t && t != in.last_sample) {
        in.last_sample = t;
        XrSpace local = vr_xr_local(), view = vr_xr_view();
        XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
        bool head_ok = view && XR_SUCCEEDED(xrLocateSpace(view, local, t, &head)) &&
                       (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        XrSpaceVelocity vel = {XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation right = {XR_TYPE_SPACE_LOCATION, &vel};
        if (head_ok && XR_SUCCEEDED(xrLocateSpace(in.hand[1], local, t, &right))) {
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            bool tracked = (right.locationFlags & need) == need &&
                           (vel.velocityFlags & (XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT));
            uint32_t before = in.melee.attacks;
            vr_vec3 lin = vr_v3(vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z);
            vr_vec3 ang = vr_v3(vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z);
            vr_melee_update(&in.melee, pose_of(right.pose), lin, ang, pose_of(head.pose).p, t / 1e9, tracked);
            if (in.melee.attacks != before) {  // (an attack begins: the game's attack, held long enough to be seen)
                in.attack_until = now + 0.12;
                in.attacks++;
                if (in.melee.locked == VR_MELEE_STAB) in.stabs++;
                else in.slashes++;
                in.last_power = in.melee.power;
            }
        }
        XrSpaceLocation left = {XR_TYPE_SPACE_LOCATION};
        if (in.shield_gesture && head_ok && XR_SUCCEEDED(xrLocateSpace(in.hand[0], local, t, &left)) &&
            (left.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT))
            in.shield = vr_shield_raised(in.shield, pose_of(head.pose), pose_of(left.pose).p);
        else
            in.shield = false;
    }
    if (now < in.attack_until) in.out.buttons |= 1ull << VRB_Y;
    if (in.shield) in.out.buttons |= 1ull << VRB_ZL;
}

// the controllers' state now (synced at most every 2 ms: the game polls its input several times a frame)
static void update(void) {
    uint64_t now = plat_now_ns();
    if (in.synced_ns && now - in.synced_ns < 2000000) return;
    in.synced_ns = now;
    XrActiveActionSet as = {in.set, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO, NULL, 1, &as};
    XrResult r = xrSyncActions(in.session, &si);
    in.active = r == XR_SUCCESS;  // (XR_SESSION_NOT_FOCUSED: the runtime's own menu has the controllers)
    if (!in.active) {
        memset(&in.out, 0, sizeof in.out);
        return;
    }
    in.syncs++;
    vr_pad_in p = {0};
    get_vec(A_MOVE, &p.lx, &p.ly);
    get_vec(A_CAMERA, &p.rx, &p.ry);
    p.a = get_bool(A_A), p.b = get_bool(A_B), p.x = get_bool(A_X), p.y = get_bool(A_Y);
    p.lclick = get_bool(A_LCLICK), p.rclick = get_bool(A_RCLICK), p.menu = get_bool(A_MENU);
    p.ltrigger = get_float(A_LTRIGGER), p.rtrigger = get_float(A_RTRIGGER);
    p.lgrip = get_float(A_LGRIP), p.rgrip = get_float(A_RGRIP);
    vr_pad_map(&in.pad, &p, now / 1e9, &in.out);
    if (in.out.snap) vr_turn(-in.out.snap * in.snap_deg * 3.14159265f / 180);  // (right: clockwise from above)
    if (in.motion) motion(now / 1e9);
}

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// runtime/hle/input.c, where the game's input is put together: the controllers' buttons and sticks added to it
// (lx... NULL: the buttons only)
void vr_input_apply(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!in.ready || !vr_enabled() || !vr_session_running()) return;
    pthread_mutex_lock(&in.lock);
    update();
    vr_pad_out o = in.out;
    pthread_mutex_unlock(&in.lock);
    *b |= o.buttons;
    if (!lx) return;
    *lx = clamp_axis(*lx + o.lx), *ly = clamp_axis(*ly + o.ly);
    *rx = clamp_axis(*rx + o.rx), *ry = clamp_axis(*ry + o.ry);
}

// for `vr`: the controllers' state as the game last got it
void vr_motion_state(uint32_t *attacks, uint32_t *slashes, uint32_t *stabs, float *power, bool *shield, bool *on) {
    pthread_mutex_lock(&in.lock);
    *attacks = in.attacks, *slashes = in.slashes, *stabs = in.stabs, *power = in.last_power;
    *shield = in.shield, *on = in.motion;
    pthread_mutex_unlock(&in.lock);
}

bool vr_input_state(uint64_t *buttons, int32_t sticks[4], uint64_t *syncs) {
    pthread_mutex_lock(&in.lock);
    *buttons = in.out.buttons;
    sticks[0] = in.out.lx, sticks[1] = in.out.ly, sticks[2] = in.out.rx, sticks[3] = in.out.ry;
    *syncs = in.syncs;
    bool ok = in.ready && in.active;
    pthread_mutex_unlock(&in.lock);
    return ok;
}
#endif
