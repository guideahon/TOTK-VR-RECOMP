// The settings (runtime/cfg.h) for the VR tests that link runtime/vr*.c without the rest of the runtime: a TOTK_*
// variable, else the default the VR rows of runtime/cfg.c metas give (kept in step with them by test_vr_options).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"

static const struct { const char *name, *def; } defaults[] = {
    {"TOTK_VR", "0"}, {"TOTK_VR_HEAD", "1"}, {"TOTK_VR_STEREO", "1"}, {"TOTK_VR_CONTROLLERS", "1"},
    {"TOTK_VR_SCREEN_WIDTH", "3"}, {"TOTK_VR_SCREEN_DISTANCE", "2.5"}, {"TOTK_VR_VIEW", "first"},
    {"TOTK_VR_HEIGHT_OFFSET", "0"}, {"TOTK_VR_THIRD_DISTANCE", "100"}, {"TOTK_VR_ASYMMETRIC", "1"},
    {"TOTK_VR_CONTROLS", "quest"}, {"TOTK_VR_LEFT_HANDED", "0"}, {"TOTK_VR_TURN", "smooth"},
    {"TOTK_VR_SNAP_DEGREES", "45"}, {"TOTK_VR_TURN_SPEED", "100"}, {"TOTK_VR_WALK", "head"},
    {"TOTK_VR_ROOMSCALE", "1"}, {"TOTK_VR_RUMBLE", "1"}, {"TOTK_VR_RUMBLE_STRENGTH", "100"}, {"TOTK_VR_MOTION", "1"},
    {"TOTK_VR_SWING", "normal"}, {"TOTK_VR_SWING_ATTACK", "1"}, {"TOTK_VR_SHIELD", "1"},
    {"TOTK_VR_VIGNETTE", "off"}, {"TOTK_VR_SHOW_CONTROLLERS", "1"}, {"TOTK_VR_PLAY_POSITION", "seated"},
    {"TOTK_VR_MOVIES", "screen"}, {"TOTK_VR_EVENTS", "auto"}, {"TOTK_VR_CUTSCENE_BARS", "0"}, {"TOTK_VR_SHADOWS_ONCE", "0"},
    {"TOTK_VR_STICK_DEADZONE", "12"}, {"TOTK_VR_ANGVEL_FIX", "auto"},
};

const char *cfg_get(const char *name) {
    const char *e = getenv(name);
    return e && *e ? e : NULL;
}

const char *cfg_value(const char *name) {
    const char *e = cfg_get(name);
    if (e) return e;
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++)
        if (!strcmp(defaults[i].name, name)) return defaults[i].def;
    abort();   // (a name without a row: as cfg.c's rt_fatal)
}

int cfg_int(const char *name) { return atoi(cfg_value(name)); }
double cfg_double(const char *name) { return atof(cfg_value(name)); }
int cfg_bool(const char *name) { return strcmp(cfg_value(name), "0") != 0; }

// (a test that changes a variable bumps this so vr_options parses again)
unsigned vr_cfg_stub_version;
unsigned cfg_version(void) { return vr_cfg_stub_version; }

// ui/menu.cpp's menu_toggle (vr_input.c: the menu button held), nothing to open in the tests
void menu_toggle(void) {}
