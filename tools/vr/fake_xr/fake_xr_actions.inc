// fake_xr's input: paths, action sets, actions and the bindings an app suggests, resolved against one controller pair,
// the Oculus Touch profile (/interaction_profiles/oculus/touch_controller), whose state the script sets:
//   <t> button <path> <0|1>     e.g. "button /user/hand/right/input/a/click 1"
//   <t> value <path> <v>        a trigger or grip, 0..1 (".../trigger/value", ".../squeeze/value")
//   <t> stick <path> <x> <y>    ".../thumbstick", -1..1
// Included by fake_xr.c (it shares its handles and helpers). The rules an app must keep, and fake_xr checks:
//   - bindings are suggested before the action sets are attached; attached once per session, never changed after;
//   - a binding's path must exist on the profile (else XR_ERROR_PATH_UNSUPPORTED);
//   - xrSyncActions only while the session is FOCUSED changes what the actions read (else they read inactive);
//   - an action's state is read through the type it was made with (else XR_ERROR_ACTION_TYPE_MISMATCH).

#define MAX_PATHS 512
#define MAX_ACTIONS 64
#define MAX_BINDINGS 128
#define MAGIC_ACTIONSET 0x46584131u
#define MAGIC_ACTION 0x46584132u

static const char *const touch_profile = "/interaction_profiles/oculus/touch_controller";
// the Touch controllers' inputs (a part of its spec's list: what a game reads)
static const char *const touch_inputs[] = {
    "/user/hand/left/input/x/click", "/user/hand/left/input/y/click", "/user/hand/left/input/menu/click",
    "/user/hand/left/input/trigger/value", "/user/hand/left/input/squeeze/value", "/user/hand/left/input/thumbstick",
    "/user/hand/left/input/thumbstick/click", "/user/hand/left/input/thumbstick/x", "/user/hand/left/input/thumbstick/y",
    "/user/hand/right/input/a/click", "/user/hand/right/input/b/click", "/user/hand/right/input/system/click",
    "/user/hand/right/input/trigger/value", "/user/hand/right/input/squeeze/value", "/user/hand/right/input/thumbstick",
    "/user/hand/right/input/thumbstick/click", "/user/hand/right/input/thumbstick/x", "/user/hand/right/input/thumbstick/y",
    "/user/hand/left/input/grip/pose", "/user/hand/left/input/aim/pose", "/user/hand/right/input/grip/pose",
    "/user/hand/right/input/aim/pose", "/user/hand/left/output/haptic", "/user/hand/right/output/haptic",
};
#define TOUCH_INPUTS (sizeof touch_inputs / sizeof *touch_inputs)

typedef struct {
    char name[256];
} path_entry;

static path_entry paths[MAX_PATHS];  // XrPath n is paths[n - 1] (0 is XR_NULL_PATH); kept for the process
static uint32_t n_paths;

typedef struct {
    uint32_t magic;
    instance *in;
    char name[XR_MAX_ACTION_SET_NAME_SIZE];
    bool attached;
} actionset;

typedef struct {
    uint32_t magic;
    actionset *set;
    char name[XR_MAX_ACTION_NAME_SIZE];
    XrActionType type;
} action;

typedef struct {
    action *a;
    XrPath binding;
} binding;

// per instance (a test makes one at a time): the suggested Touch bindings and the controllers' raw state
static struct {
    binding b[MAX_BINDINGS];
    int n;
    bool touch_suggested;
    float value[TOUCH_INPUTS][2];  // per input: x (or the value / click), y
    bool synced_focus;              // the last sync was in focus (states read active)
    float synced[TOUCH_INPUTS][2];  // the state at the last sync
} ctl;

static XrPath path_of(const char *s) {
    for (uint32_t i = 0; i < n_paths; i++)
        if (!strcmp(paths[i].name, s)) return (XrPath)(i + 1);
    if (n_paths == MAX_PATHS) return XR_NULL_PATH;
    snprintf(paths[n_paths].name, sizeof paths[n_paths].name, "%s", s);
    return (XrPath)(++n_paths);
}

static const char *path_name(XrPath p) { return p >= 1 && p <= n_paths ? paths[p - 1].name : NULL; }

static int touch_input(const char *s) {
    for (size_t i = 0; i < TOUCH_INPUTS; i++)
        if (!strcmp(touch_inputs[i], s)) return (int)i;
    return -1;
}

static XrResult XRAPI_CALL fx_StringToPath(XrInstance h, const char *s, XrPath *out) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    if (!s || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (s[0] != '/' || strlen(s) >= sizeof paths[0].name) return XR_ERROR_PATH_FORMAT_INVALID;
    *out = path_of(s);
    return *out ? XR_SUCCESS : XR_ERROR_PATH_COUNT_EXCEEDED;
}

static XrResult XRAPI_CALL fx_PathToString(XrInstance h, XrPath p, uint32_t cap, uint32_t *count, char *buf) {
    if (!get_instance(h)) return XR_ERROR_HANDLE_INVALID;
    const char *n = path_name(p);
    if (!n) return XR_ERROR_PATH_INVALID;
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = (uint32_t)strlen(n) + 1;
    if (!cap) return XR_SUCCESS;
    if (cap < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, n, *count);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_CreateActionSet(XrInstance h, const XrActionSetCreateInfo *ci, XrActionSet *out) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_ACTION_SET_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (!ci->actionSetName[0] || !ci->localizedActionSetName[0]) return XR_ERROR_NAME_INVALID;
    actionset *s = calloc(1, sizeof *s);
    s->magic = MAGIC_ACTIONSET;
    s->in = in;
    snprintf(s->name, sizeof s->name, "%s", ci->actionSetName);
    *out = (XrActionSet)(uintptr_t)s;
    return XR_SUCCESS;
}

static actionset *get_actionset(XrActionSet h) {
    actionset *s = (actionset *)(uintptr_t)h;
    return s && s->magic == MAGIC_ACTIONSET ? s : NULL;
}

static action *get_action(XrAction h) {
    action *a = (action *)(uintptr_t)h;
    return a && a->magic == MAGIC_ACTION ? a : NULL;
}

static XrResult XRAPI_CALL fx_DestroyActionSet(XrActionSet h) {
    actionset *s = get_actionset(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    s->magic = 0;
    free(s);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_CreateAction(XrActionSet h, const XrActionCreateInfo *ci, XrAction *out) {
    actionset *s = get_actionset(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_ACTION_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    if (!ci->actionName[0] || !ci->localizedActionName[0]) return XR_ERROR_NAME_INVALID;
    action *a = calloc(1, sizeof *a);
    a->magic = MAGIC_ACTION;
    a->set = s;
    a->type = ci->actionType;
    snprintf(a->name, sizeof a->name, "%s", ci->actionName);
    *out = (XrAction)(uintptr_t)a;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_DestroyAction(XrAction h) {
    action *a = get_action(h);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    for (int i = 0; i < ctl.n; i++)
        if (ctl.b[i].a == a) ctl.b[i].a = NULL;
    a->magic = 0;
    free(a);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_SuggestInteractionProfileBindings(XrInstance h,
                                                                 const XrInteractionProfileSuggestedBinding *sb) {
    instance *in = get_instance(h);
    if (!in) return XR_ERROR_HANDLE_INVALID;
    if (!sb || sb->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING || !sb->countSuggestedBindings ||
        !sb->suggestedBindings)
        return XR_ERROR_VALIDATION_FAILURE;
    const char *profile = path_name(sb->interactionProfile);
    if (!profile) return XR_ERROR_PATH_INVALID;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; i++) {
        action *a = get_action(sb->suggestedBindings[i].action);
        if (!a) return XR_ERROR_HANDLE_INVALID;
        if (a->set->attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        if (!path_name(sb->suggestedBindings[i].binding)) return XR_ERROR_PATH_INVALID;
    }
    if (strcmp(profile, touch_profile)) return XR_SUCCESS;  // (another controller: fine, never used here)
    for (uint32_t i = 0; i < sb->countSuggestedBindings; i++)
        if (touch_input(path_name(sb->suggestedBindings[i].binding)) < 0) return XR_ERROR_PATH_UNSUPPORTED;
    ctl.n = 0;  // (a profile's suggestion replaces its earlier one)
    for (uint32_t i = 0; i < sb->countSuggestedBindings && ctl.n < MAX_BINDINGS; i++)
        ctl.b[ctl.n++] = (binding){get_action(sb->suggestedBindings[i].action), sb->suggestedBindings[i].binding};
    ctl.touch_suggested = true;
    logf_(in, "bindings for the Touch profile: %d\n", ctl.n);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_AttachSessionActionSets(XrSession h, const XrSessionActionSetsAttachInfo *ai) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ai || ai->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO || !ai->countActionSets || !ai->actionSets)
        return XR_ERROR_VALIDATION_FAILURE;
    if (s->attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t i = 0; i < ai->countActionSets; i++) {
        actionset *as = get_actionset(ai->actionSets[i]);
        if (!as) return XR_ERROR_HANDLE_INVALID;
        as->attached = true;
    }
    s->attached = true;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_SyncActions(XrSession h, const XrActionsSyncInfo *si) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!si || si->type != XR_TYPE_ACTIONS_SYNC_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (s->lost) return XR_ERROR_SESSION_LOST;
    if (!s->attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    for (uint32_t i = 0; i < si->countActiveActionSets; i++) {
        actionset *as = get_actionset(si->activeActionSets[i].actionSet);
        if (!as) return XR_ERROR_HANDLE_INVALID;
        if (!as->attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    if (s->state != XR_SESSION_STATE_FOCUSED) {  // (not in focus: every action reads inactive)
        ctl.synced_focus = false;
        return XR_SESSION_NOT_FOCUSED;
    }
    ctl.synced_focus = true;
    memcpy(ctl.synced, ctl.value, sizeof ctl.synced);
    return XR_SUCCESS;
}

// the action's inputs on the Touch controllers (subactionPath: one hand only, or XR_NULL_PATH for both)
static int action_inputs(action *a, XrPath sub, int *out, int max) {
    int n = 0;
    const char *subname = sub ? path_name(sub) : NULL;
    for (int i = 0; i < ctl.n && n < max; i++) {
        if (ctl.b[i].a != a) continue;
        const char *b = path_name(ctl.b[i].binding);
        if (subname && strncmp(b, subname, strlen(subname))) continue;
        int k = touch_input(b);
        if (k >= 0) out[n++] = k;
    }
    return n;
}

static XrResult get_state(XrSession h, const XrActionStateGetInfo *gi, XrActionType want, action **out) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!gi || gi->type != XR_TYPE_ACTION_STATE_GET_INFO) return XR_ERROR_VALIDATION_FAILURE;
    action *a = get_action(gi->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    if (!a->set->attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (a->type != want) return XR_ERROR_ACTION_TYPE_MISMATCH;
    *out = a;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetActionStateBoolean(XrSession h, const XrActionStateGetInfo *gi,
                                                     XrActionStateBoolean *st) {
    action *a;
    XrResult r = get_state(h, gi, XR_ACTION_TYPE_BOOLEAN_INPUT, &a);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_BOOLEAN) return XR_ERROR_VALIDATION_FAILURE;
    int in[8];
    int n = action_inputs(a, gi->subactionPath, in, 8);
    st->isActive = ctl.synced_focus && n > 0;
    st->currentState = XR_FALSE;
    for (int i = 0; i < n && st->isActive; i++)  // (a value input bound to a boolean: pressed from 0.5)
        if (ctl.synced[in[i]][0] >= 0.5f) st->currentState = XR_TRUE;
    st->changedSinceLastSync = XR_FALSE;
    st->lastChangeTime = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetActionStateFloat(XrSession h, const XrActionStateGetInfo *gi, XrActionStateFloat *st) {
    action *a;
    XrResult r = get_state(h, gi, XR_ACTION_TYPE_FLOAT_INPUT, &a);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_FLOAT) return XR_ERROR_VALIDATION_FAILURE;
    int in[8];
    int n = action_inputs(a, gi->subactionPath, in, 8);
    st->isActive = ctl.synced_focus && n > 0;
    st->currentState = 0;
    for (int i = 0; i < n && st->isActive; i++)  // (the largest of its inputs)
        if (fabsf(ctl.synced[in[i]][0]) > fabsf(st->currentState)) st->currentState = ctl.synced[in[i]][0];
    st->changedSinceLastSync = XR_FALSE;
    st->lastChangeTime = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetActionStateVector2f(XrSession h, const XrActionStateGetInfo *gi,
                                                      XrActionStateVector2f *st) {
    action *a;
    XrResult r = get_state(h, gi, XR_ACTION_TYPE_VECTOR2F_INPUT, &a);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_VECTOR2F) return XR_ERROR_VALIDATION_FAILURE;
    int in[8];
    int n = action_inputs(a, gi->subactionPath, in, 8);
    st->isActive = ctl.synced_focus && n > 0;
    st->currentState = (XrVector2f){0, 0};
    for (int i = 0; i < n && st->isActive; i++)
        if (fabsf(ctl.synced[in[i]][0]) + fabsf(ctl.synced[in[i]][1]) >
            fabsf(st->currentState.x) + fabsf(st->currentState.y))
            st->currentState = (XrVector2f){ctl.synced[in[i]][0], ctl.synced[in[i]][1]};
    st->changedSinceLastSync = XR_FALSE;
    st->lastChangeTime = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetCurrentInteractionProfile(XrSession h, XrPath user, XrInteractionProfileState *ps) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ps || ps->type != XR_TYPE_INTERACTION_PROFILE_STATE) return XR_ERROR_VALIDATION_FAILURE;
    if (!s->attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    (void)user;
    ps->interactionProfile = ctl.touch_suggested ? path_of(touch_profile) : XR_NULL_PATH;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fx_GetActionStatePose(XrSession h, const XrActionStateGetInfo *gi, XrActionStatePose *st) {
    action *a;
    XrResult r = get_state(h, gi, XR_ACTION_TYPE_POSE_INPUT, &a);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_POSE) return XR_ERROR_VALIDATION_FAILURE;
    int in[8];
    st->isActive = ctl.synced_focus && action_inputs(a, gi->subactionPath, in, 8) > 0;
    return XR_SUCCESS;
}

// a pose action's space: the hand its subaction path names (or its first binding's hand)
static XrResult XRAPI_CALL fx_CreateActionSpace(XrSession h, const XrActionSpaceCreateInfo *ci, XrSpace *out) {
    session *s = get_session(h);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !out || ci->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    action *a = get_action(ci->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    const char *sub = ci->subactionPath ? path_name(ci->subactionPath) : NULL;
    int hand = 0;
    if (sub && !strcmp(sub, "/user/hand/left")) hand = 1;
    else if (sub && !strcmp(sub, "/user/hand/right")) hand = 2;
    else if (sub) return XR_ERROR_PATH_UNSUPPORTED;
    for (int i = 0; i < ctl.n && !hand; i++)
        if (ctl.b[i].a == a) hand = strstr(path_name(ctl.b[i].binding), "/left/") ? 1 : 2;
    if (!hand) hand = 2;
    space *sp = calloc(1, sizeof *sp);
    sp->magic = MAGIC_SPACE;
    sp->s = s;
    sp->hand = hand;
    const XrPosef *p = &ci->poseInActionSpace;
    sp->pose = (vr_pose){{p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w},
                         {p->position.x, p->position.y, p->position.z}};
    *out = (XrSpace)(uintptr_t)sp;
    return XR_SUCCESS;
}

// the script's controller lines (run_script): the raw state the next xrSyncActions takes
static void fxa_script(const char *kind, const char *path, float x, float y) {
    int k = touch_input(path);
    if (k < 0) {
        fprintf(stderr, "fake_xr: no Touch input %s\n", path);
        return;
    }
    ctl.value[k][0] = x;
    ctl.value[k][1] = !strcmp(kind, "stick") ? y : 0;
}
