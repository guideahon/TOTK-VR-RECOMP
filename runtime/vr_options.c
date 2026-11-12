// The VR mode's options (vr_options.h): the cfg table's TOTK_VR_* rows, parsed once per cfg_version.
#include <string.h>

#include "cfg.h"
#include "vr_options.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

const vr_options_t *vr_options(void) {
    static vr_options_t o;
    static unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v == seen) return &o;
    seen = v;
    vr_options_t n;
    n.first_person = strcmp(cfg_value("TOTK_VR_VIEW"), "third") != 0;
    n.height_cm = clampi(cfg_int("TOTK_VR_HEIGHT_OFFSET"), -50, 50);
    n.third_pct = clampi(cfg_int("TOTK_VR_THIRD_DISTANCE"), 25, 300);
    n.asymmetric = cfg_bool("TOTK_VR_ASYMMETRIC");
    n.bettervr = strcmp(cfg_value("TOTK_VR_CONTROLS"), "classic") != 0;
    n.left_handed = cfg_bool("TOTK_VR_LEFT_HANDED");
    const char *t = cfg_value("TOTK_VR_TURN");
    n.turn = !strcmp(t, "snap") ? VR_TURN_SNAP : !strcmp(t, "head") ? VR_TURN_HEAD : VR_TURN_SMOOTH;
    n.snap_deg = clampi(cfg_int("TOTK_VR_SNAP_DEGREES"), 5, 180);
    n.turn_pct = clampi(cfg_int("TOTK_VR_TURN_SPEED"), 20, 300);
    n.walk_controller = !strcmp(cfg_value("TOTK_VR_WALK"), "controller");
    n.roomscale = cfg_bool("TOTK_VR_ROOMSCALE");
    n.rumble = cfg_bool("TOTK_VR_RUMBLE");
    n.rumble_pct = clampi(cfg_int("TOTK_VR_RUMBLE_STRENGTH"), 0, 400);
    n.motion = cfg_bool("TOTK_VR_MOTION");
    n.swing_easy = !strcmp(cfg_value("TOTK_VR_SWING"), "easy");
    n.swing_attack = cfg_bool("TOTK_VR_SWING_ATTACK");
    n.shield = cfg_bool("TOTK_VR_SHIELD");
    const char *vg = cfg_value("TOTK_VR_VIGNETTE");
    n.vignette = !strcmp(vg, "low") ? 1 : !strcmp(vg, "medium") ? 2 : !strcmp(vg, "high") ? 3 : 0;
    n.show_controllers = cfg_bool("TOTK_VR_SHOW_CONTROLLERS");
    n.movies_on_screen = strcmp(cfg_value("TOTK_VR_MOVIES"), "world") != 0;
    n.standing = !strcmp(cfg_value("TOTK_VR_PLAY_POSITION"), "standing");
    o = n;   // (one thread at a time changes it: the game's input and camera threads see whole values either way)
    return &o;
}
