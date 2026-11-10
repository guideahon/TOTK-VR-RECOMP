// VR controllers -> the game's controller (runtime/vr/vr_input.c): what each input becomes. Header only and free of
// the rest of the runtime: tests/vr/test_vr_input_map.c checks it.
//
//   left stick   move                  right stick   camera
//   left click   crouch (L stick)      right click   look (R stick)
//   X / Y        X / Y                 A / B         A / B
//   left grip    L (runes)             right grip    R (throw)
//   left trigger ZL (shield, aim)      right trigger ZR (bow)
//   menu (left)  a tap: + (inventory); held 0.6 s: - (map), for as long as it is held
//   left grip held + right stick: the d-pad (weapons, bows, shields, runes) in the stick's direction; the camera and
//   L wait until the grip is let go
//   snap turning (s->snap on): the right stick pushed sideways turns the view a step at once (snap: -1 left, +1 right,
//   once a push) and does not turn the game's camera; its up-down still goes to the game
//
// The game's buttons are bits of a mask in its order: a b x y lstick rstick l r zl zr plus minus left up right down;
// its sticks run from -32767 to 32767, +y up.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

enum {
    VRB_A, VRB_B, VRB_X, VRB_Y, VRB_LSTICK, VRB_RSTICK, VRB_L, VRB_R, VRB_ZL, VRB_ZR, VRB_PLUS, VRB_MINUS,
    VRB_LEFT, VRB_UP, VRB_RIGHT, VRB_DOWN
};

typedef struct {
    float lx, ly, rx, ry;               // sticks (or touchpads), -1..1, +y up
    bool a, b, x, y, lclick, rclick, menu;
    float ltrigger, rtrigger, lgrip, rgrip;  // 0..1
} vr_pad_in;

typedef struct {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    int snap;  // snap turning: a step to the left (-1) or right (+1) to make now, else 0
} vr_pad_out;

typedef struct {
    bool chord;          // the left grip is held and the right stick has been used as a d-pad since it was pressed
    bool menu;           // the menu button was down at the last update
    double menu_down;    // ... since this time (s)
    double plus_until;   // a tap's + is held until this time (s)
    bool snap;           // snap turning on (set by the caller)
    bool snap_armed;     // the right stick has come back to the middle since the last step
} vr_pad_state;

#define VR_PAD_PRESS 0.5f         // a trigger or grip counts as pressed from here
#define VR_PAD_DEAD 0.12f         // the sticks' dead zone (radial)
#define VR_PAD_DPAD 0.6f          // the right stick picks a d-pad direction from here
#define VR_PAD_MENU_HOLD 0.6      // the menu button held this long (s) is -
#define VR_PAD_TAP 0.1            // a tap's + is held this long (s): the game sees it for a few frames
#define VR_PAD_SNAP 0.7f          // snap turning: a push this far sideways is a step...
#define VR_PAD_SNAP_REARM 0.3f    // ... and the stick back under this, the next one can come

// a stick's position past the dead zone, scaled so the edge of the dead zone is 0 and the rim 32767
static inline void vr_pad_stick(float x, float y, int32_t *ox, int32_t *oy) {
    float m = sqrtf(x * x + y * y);
    if (m <= VR_PAD_DEAD) {
        *ox = *oy = 0;
        return;
    }
    float k = ((m > 1 ? 1 : m) - VR_PAD_DEAD) / (1 - VR_PAD_DEAD) / m;  // (a corner of a square pad: the rim)
    float sx = x * k, sy = y * k;
    *ox = (int32_t)lrintf(sx * 32767), *oy = (int32_t)lrintf(sy * 32767);
}

static inline void vr_pad_map(vr_pad_state *s, const vr_pad_in *in, double now, vr_pad_out *out) {
    uint64_t b = 0;
    if (in->a) b |= 1ull << VRB_A;
    if (in->b) b |= 1ull << VRB_B;
    if (in->x) b |= 1ull << VRB_X;
    if (in->y) b |= 1ull << VRB_Y;
    if (in->lclick) b |= 1ull << VRB_LSTICK;
    if (in->rclick) b |= 1ull << VRB_RSTICK;
    if (in->rgrip >= VR_PAD_PRESS) b |= 1ull << VRB_R;
    if (in->ltrigger >= VR_PAD_PRESS) b |= 1ull << VRB_ZL;
    if (in->rtrigger >= VR_PAD_PRESS) b |= 1ull << VRB_ZR;
    vr_pad_stick(in->lx, in->ly, &out->lx, &out->ly);
    vr_pad_stick(in->rx, in->ry, &out->rx, &out->ry);
    // the left grip: L, or with the right stick the d-pad
    bool grip = in->lgrip >= VR_PAD_PRESS;
    float rm = sqrtf(in->rx * in->rx + in->ry * in->ry);
    if (!grip) s->chord = false;
    else if (rm >= VR_PAD_DPAD) s->chord = true;
    if (s->chord) {
        if (rm >= VR_PAD_DPAD) {
            if (fabsf(in->rx) > fabsf(in->ry)) b |= 1ull << (in->rx > 0 ? VRB_RIGHT : VRB_LEFT);
            else b |= 1ull << (in->ry > 0 ? VRB_UP : VRB_DOWN);
        }
        out->rx = out->ry = 0;
    } else if (grip) {
        b |= 1ull << VRB_L;
    }
    // the menu button: a tap is +, held it is -
    if (in->menu && !s->menu) s->menu_down = now;
    if (in->menu && now - s->menu_down >= VR_PAD_MENU_HOLD) b |= 1ull << VRB_MINUS;
    if (!in->menu && s->menu && now - s->menu_down < VR_PAD_MENU_HOLD) s->plus_until = now + VR_PAD_TAP;
    if (now < s->plus_until) b |= 1ull << VRB_PLUS;
    s->menu = in->menu;
    // snap turning: a sideways push is one step, the stick sideways is not the camera's
    out->snap = 0;
    if (s->snap && !s->chord) {
        if (fabsf(in->rx) < VR_PAD_SNAP_REARM) s->snap_armed = true;
        else if (s->snap_armed && fabsf(in->rx) >= VR_PAD_SNAP) {
            out->snap = in->rx > 0 ? 1 : -1;
            s->snap_armed = false;
        }
        out->rx = 0;
    }
    out->buttons = b;
}
