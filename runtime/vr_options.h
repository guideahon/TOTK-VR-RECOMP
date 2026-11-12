// The VR mode's options (docs/vr.md): totk.ini's [vr] keys, the menu's VR tab (ui/menu.cpp tab_vr), or a TOTK_VR_*
// variable, which wins (cfg.h). The table of keys, defaults and values is runtime/cfg.c metas. vr_options() parses them
// again only when cfg_version changes, so the VR code reads it every frame and a change on the menu applies at once.
#pragma once
#include <stdbool.h>

enum { VR_TURN_SMOOTH, VR_TURN_SNAP, VR_TURN_HEAD };

typedef struct {
    bool first_person;     // vr_view: first (the eye in Link's head) or third (the game's camera)
    int height_cm;         // vr_height_offset: first person, the eye this much higher (-50..50)
    int third_pct;         // vr_third_distance: third person, the camera's distance (% of the game's)
    bool asymmetric;       // vr_asymmetric: each eye its own off-centre field
    bool bettervr;         // vr_controls: BetterVR's scheme and gestures (else classic)
    bool left_handed;
    int turn;              // vr_turn: VR_TURN_*
    int snap_deg;          // 5..180
    int turn_pct;          // 20..300
    bool walk_controller;  // vr_walk: controller
    bool roomscale;
    bool rumble;
    int rumble_pct;        // 0..400
    bool motion, swing_easy, swing_attack, shield;
} vr_options_t;

// The options now (never NULL; the table defaults before cfg_init)
const vr_options_t *vr_options(void);
