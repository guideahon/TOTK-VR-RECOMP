// BetterVR's body gestures and control scheme, for runtime/vr_input.c. Header only and free of the rest of the
// runtime: tests/vr/test_vr_gestures.c checks it. From Savage Wind VR (runtime/vr/vr_gestures.h), which ported it in C
// from BotW-BetterVR (Crementif and contributors, MIT: src/hooking/controls.cpp calculateHandGesture,
// openDpadMenuBodySlots, processLeftHandInGameInput, processRightHandInGameInput). TotK's controls differ from BotW's
// in one place used here: the abilities (BotW's runes) are on L, a tap uses the one chosen and L held opens their wheel
// (BotW: the runes' menu on d-pad up). VR_G_ABILITY_MENU is that button.
//
// Body slots, around the head (tracking space, metres): over a shoulder (behind the head, within 0.35 m of it, on
// that side), at the left or right waist (0.45 m below the head and at or behind it), at the mouth (within 0.2 m, in
// front). A grip pressed and let go over a slot (a short press, as BetterVR):
//   right shoulder   the melee weapon (Y; again: put away, B)      held 0.5 s: the weapons quick menu (d-pad right)
//   left shoulder    the bow (ZR; again: B)                         held: shields (d-pad left) with a melee weapon out,
//                                                                    else the bows (d-pad right after drawing the bow)
//   left waist       the ability (L; again: B)                      held: the abilities' wheel (L held)
//   both at mouth    whistle (d-pad down)
//   right waist      held (either hand): drop what that hand holds (BetterVR's drop): the hand's quick menu opens
//                    (right hand: weapons, d-pad right; left: bows, d-pad right, or shields, d-pad left) and the drop
//                    button (Y) is tapped. Off in TotK (drop_enabled false): its quick menu has no drop (docs/vr-parity.md)
// A quick menu stays open while the grip is held; let go, the item chosen is taken out (as BetterVR does).
// Elsewhere the grip is grab / interact (A).
//
// BetterVR's buttons (scheme VR_SCHEME_BETTERVR; vr_input_map.h has the earlier one):
//   right trigger   use what is in hand: the melee weapon attacks (Y), the bow draws (ZR), an ability is used (R); over
//                   the right shoulder with a weapon out, held: throw (R, let go to throw)
//   right A         grab / interact (A); held 0.4 s: run (B)          right B   jump (X)
//   right stick     camera; its click: a tap the inventory (+), held the map (-)
//   left trigger    lock on / shield (ZL)                              left Y    a tap the ability (L), held its wheel
//   left stick      move; its click: a tap crouch (L stick), held the scope (R stick)
//   left X          (BetterVR's own menu there; here: held, the game's map)
//   left hand swung at the face while guarding (ZL held): parry (A)
//
// What the game has in hand is not read from the game yet (BetterVR reads it from Link's bones): it is tracked here
// from the gestures and buttons that change it, and a put-away (B, run) clears it.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "vr_input_map.h"

typedef struct { float x, y, z; } vr_g3;

enum { VR_HELD_NONE, VR_HELD_MELEE, VR_HELD_BOW, VR_HELD_RUNE };
enum { VR_HAND_EMPTY, VR_HAND_MELEE, VR_HAND_BOW, VR_HAND_SHIELD };
enum { VR_SLOT_NONE, VR_SLOT_RSHOULDER, VR_SLOT_LSHOULDER, VR_SLOT_LWAIST, VR_SLOT_RWAIST, VR_SLOT_MOUTH };

#define VR_G_SHOULDER_R 0.35f
#define VR_G_MOUTH_R 0.2f
#define VR_G_WAIST_Y 0.45f
#define VR_G_WAIST_BEHIND (-0.05f)
#define VR_G_LONG 0.5            // a grip held this long (s) over a slot: its quick menu
#define VR_G_RUN_HOLD 0.4        // right A held this long: run
#define VR_G_STICK_HOLD 0.5      // a stick click held this long: its second action
#define VR_G_TAP 0.12            // a tap's button is held this long for the game to see it
#define VR_G_PARRY_ACCEL 35.0f   // m/s^2: the left hand's speed rising this fast while guarding is a parry
#define VR_G_ABILITY_MENU VRB_L  // the abilities' menu (TotK: L held opens their wheel; BotW: d-pad up)

static inline vr_g3 vr_g3v(float x, float y, float z) { vr_g3 r = {x, y, z}; return r; }
static inline vr_g3 vr_g3sub(vr_g3 a, vr_g3 b) { return vr_g3v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline float vr_g3dot(vr_g3 a, vr_g3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// which body slot the hand is over. head: position; fwd, right: the head's forward and right (unit, any pitch)
static inline int vr_gesture_slot(vr_g3 head, vr_g3 fwd, vr_g3 right, vr_g3 hand) {
    vr_g3 f = vr_g3v(fwd.x, 0, fwd.z);
    float fl = sqrtf(f.x * f.x + f.z * f.z);
    f = fl > 1e-4f ? vr_g3v(f.x / fl, 0, f.z / fl) : vr_g3v(0, 0, -1);
    vr_g3 d = vr_g3sub(hand, head);
    float dist2 = vr_g3dot(d, d);
    float ahead = vr_g3dot(f, d);
    bool left = vr_g3dot(right, d) < 0;
    if (ahead < 0 && dist2 < VR_G_SHOULDER_R * VR_G_SHOULDER_R) return left ? VR_SLOT_LSHOULDER : VR_SLOT_RSHOULDER;
    if (ahead + VR_G_WAIST_BEHIND < 0 && hand.y < head.y - VR_G_WAIST_Y) return left ? VR_SLOT_LWAIST : VR_SLOT_RWAIST;
    if (ahead >= 0 && dist2 < VR_G_MOUTH_R * VR_G_MOUTH_R) return VR_SLOT_MOUTH;
    return VR_SLOT_NONE;
}

typedef struct {
    vr_g3 head, fwd, right;     // the head (tracking space)
    vr_g3 hand[2];              // left, right hand positions
    int hand_held[2];           // model at each hand (vr_skel_held: melee, bow, shield)
    float hand_speed[2];        // their speeds (m/s)
    bool grip[2], trigger[2];   // pressed
    bool a, b, x, y;            // right A, B; left X, Y
    bool lclick, rclick;
    bool drop_enabled;           // the live VR option also disables the gesture's selector input
    double now;                 // s
    bool no_grab[2];            // this hand's grip takes hold of something else now (both hands on a weapon)
} vr_gesture_in;

typedef struct {
    int held;                   // what is in hand (VR_HELD_*), as tracked here
    bool grip_was[2];
    double grip_down[2];
    int grip_slot[2];           // the slot the grip was pressed over
    bool menu_open[2];          // a quick menu is held open by this hand's grip
    int menu_button[2];         // ... with this d-pad button (VRB_*)
    int menu_take;              // let go of a quick menu: what to take out (VR_HELD_*)
    double tap_until[16];       // a tap's button is held until then
    bool a_was, y_was, lclick_was, rclick_was, x_was;
    double a_down, y_down, lclick_down, rclick_down;
    bool y_menu, lclick_long, rclick_long;
    bool throwing;              // the right trigger is holding R for a throw
    float prev_speed0;
    double prev_now;
    int haptic[2];              // set when a slot is entered or a gesture fires: the caller pulses that hand
    int slot_now[2];
    // magnesis by hand (BetterVR's magnesis gestures): with a rune out, a grip held away from the slots moves what the
    // rune holds by the hand's motion from where the grip went down
    bool mag[2];
    vr_g3 mag_from[2];
    float stick_ry;             // out: the right stick's up-down to give the game (valid when stick_ry_set)
    bool stick_ry_set;
    double drop_t0[2];          // a drop going on for this hand since then (0: none); done until the grip is let go
    int drop_button[2];
    bool drop_done[2];
} vr_gesture_state;

#define VR_G_DROP_PRESS 0.9      // s into a drop: the quick menu is open, the drop button is tapped
#define VR_G_DROP_END 1.4        // s: the d-pad let go, the menu closes

#define VR_G_MAG_FULL 0.15f     // m: a hand this far up or down is the stick pushed fully
#define VR_G_MAG_PUSH 0.08f     // m: a hand this far forward or back pushes or pulls (d-pad up / down)

static inline void vr_g_tap(vr_gesture_state *s, int button, double now) { s->tap_until[button] = now + VR_G_TAP; }

// one frame: the game's buttons (VRB_* bits) the gestures and BetterVR's scheme give
static inline uint64_t vr_gestures(vr_gesture_state *s, const vr_gesture_in *in) {
    uint64_t b = 0;
    double now = in->now;
    int slot[2];
    for (int h = 0; h < 2; h++) {
        slot[h] = vr_gesture_slot(in->head, in->fwd, in->right, in->hand[h]);
        s->haptic[h] = slot[h] != VR_SLOT_NONE && slot[h] != s->slot_now[h];
        s->slot_now[h] = slot[h];
    }
    bool whistle = in->grip[0] && in->grip[1] && slot[0] == VR_SLOT_MOUTH && slot[1] == VR_SLOT_MOUTH;
    if (whistle) b |= 1ull << VRB_DOWN;
    // the grips: a slot's item (press), its quick menu (held), else grab
    for (int h = 0; h < 2; h++) {
        bool g = in->grip[h];
        bool slotted = s->grip_slot[h] == VR_SLOT_RSHOULDER || s->grip_slot[h] == VR_SLOT_LSHOULDER ||
                       s->grip_slot[h] == VR_SLOT_LWAIST;
        if (g && !s->grip_was[h]) {
            s->grip_down[h] = now;
            s->grip_slot[h] = slot[h];
            slotted = slot[h] == VR_SLOT_RSHOULDER || slot[h] == VR_SLOT_LSHOULDER || slot[h] == VR_SLOT_LWAIST;
            if (!whistle && !slotted && slot[h] != VR_SLOT_MOUTH && slot[h] != VR_SLOT_RWAIST && !in->no_grab[h])
                vr_g_tap(s, VRB_A, now);   // grab / interact
        }
        // a short press over a slot (let go before the quick menu): its item out, or put away when it is out
        if (!g && s->grip_was[h] && slotted && !s->menu_open[h] && now - s->grip_down[h] < VR_G_LONG) {
            int sl = s->grip_slot[h];
            int want = sl == VR_SLOT_RSHOULDER ? VR_HELD_MELEE : sl == VR_SLOT_LSHOULDER ? VR_HELD_BOW : VR_HELD_RUNE;
            if (s->held == want) {
                vr_g_tap(s, VRB_B, now);
                s->held = VR_HELD_NONE;
            } else {
                vr_g_tap(s, want == VR_HELD_MELEE ? VRB_Y : want == VR_HELD_BOW ? VRB_ZR : VRB_L, now);
                s->held = want;
            }
            s->haptic[h] = 2;
        }
        // held over its slot: the quick menu, open while the grip stays down
        int sl = s->grip_slot[h];
        if (g && !s->menu_open[h] && now - s->grip_down[h] >= VR_G_LONG &&
            (sl == VR_SLOT_RSHOULDER || sl == VR_SLOT_LSHOULDER || sl == VR_SLOT_LWAIST)) {
            s->menu_open[h] = true;
            if (sl == VR_SLOT_RSHOULDER) s->menu_button[h] = VRB_RIGHT, s->menu_take = VR_HELD_MELEE;
            else if (sl == VR_SLOT_LWAIST) s->menu_button[h] = VR_G_ABILITY_MENU, s->menu_take = VR_HELD_RUNE;
            else if (s->held == VR_HELD_MELEE) s->menu_button[h] = VRB_LEFT, s->menu_take = VR_HELD_MELEE;  // (shields)
            else s->menu_button[h] = VRB_RIGHT, s->menu_take = VR_HELD_BOW;   // (bows: the bow is out from the press)
            s->haptic[h] = 2;
        }
        // held at the right waist: drop what this hand holds, once a press
        bool droppable = h == 1 ? in->hand_held[h] == VR_HAND_MELEE :
            in->hand_held[h] == VR_HAND_BOW || in->hand_held[h] == VR_HAND_SHIELD;
        if (g && sl == VR_SLOT_RWAIST && in->drop_enabled && droppable && !s->drop_t0[h] && !s->drop_done[h] &&
            now - s->grip_down[h] >= VR_G_LONG) {
            s->drop_t0[h] = now;
            s->drop_button[h] = h == 1 || in->hand_held[h] == VR_HAND_BOW ? VRB_RIGHT : VRB_LEFT;
            s->haptic[h] = 2;
        }
        if (!in->drop_enabled && s->drop_t0[h]) s->drop_t0[h] = 0, s->drop_done[h] = true;
        if (s->drop_t0[h]) {
            double t = now - s->drop_t0[h];
            if (t < VR_G_DROP_END) {
                b |= 1ull << s->drop_button[h];
                if (t >= VR_G_DROP_PRESS && t < VR_G_DROP_PRESS + VR_G_TAP) b |= 1ull << VRB_Y;
            } else {
                s->drop_t0[h] = 0, s->drop_done[h] = true;
                if ((h == 1 && s->held == VR_HELD_MELEE) || (h == 0 && s->held == VR_HELD_BOW))
                    s->held = VR_HELD_NONE;
            }
        }
        if (!g) s->drop_done[h] = false;
        if (s->menu_open[h]) {
            if (g) b |= 1ull << s->menu_button[h];
            else if (s->grip_was[h]) {   // let go: what was chosen comes out (it was put away by the press's toggle, or is new)
                s->menu_open[h] = false;
                int take = s->menu_take;
                int tb = take == VR_HELD_MELEE ? VRB_Y : take == VR_HELD_BOW ? VRB_ZR : VRB_L;
                if (tb != s->menu_button[h]) vr_g_tap(s, tb, now);   // (L let go closes the wheel on its choice)
                s->held = take;
            }
        }
        s->grip_was[h] = g;
    }
    // right trigger: use what is in hand; over the right shoulder with a weapon out: throw (R held, let go to throw)
    bool rt = in->trigger[1];
    if (rt && (s->throwing || (slot[1] == VR_SLOT_RSHOULDER && s->held == VR_HELD_MELEE))) {
        s->throwing = true;
        b |= 1ull << VRB_R;
    } else {
        if (s->throwing) s->held = VR_HELD_NONE;   // (thrown)
        s->throwing = false;
        if (rt) {
            if (s->held == VR_HELD_BOW) b |= 1ull << VRB_ZR;
            else if (s->held == VR_HELD_RUNE) b |= 1ull << VRB_R;
            else {
                b |= 1ull << VRB_Y;
                s->held = VR_HELD_MELEE;   // (attacking takes the weapon out)
            }
        }
    }
    // magnesis: a rune out and a grip held away from the slots: the hand's rise and fall moves the held object up and
    // down (the right stick), its push and pull away or near (d-pad up, down); nearer the head's frame than the world
    s->stick_ry_set = false;
    for (int h = 0; h < 2; h++) {
        bool held = in->grip[h] && s->held == VR_HELD_RUNE && s->grip_slot[h] == VR_SLOT_NONE && !s->menu_open[h];
        if (held && !s->mag[h]) s->mag[h] = true, s->mag_from[h] = in->hand[h];
        if (!held) s->mag[h] = false;
        if (!s->mag[h]) continue;
        vr_g3 d = vr_g3sub(in->hand[h], s->mag_from[h]);
        vr_g3 f = vr_g3v(in->fwd.x, 0, in->fwd.z);
        float fl = sqrtf(f.x * f.x + f.z * f.z);
        float ahead = fl > 1e-4f ? (d.x * f.x + d.z * f.z) / fl : 0;
        float up = d.y / VR_G_MAG_FULL;
        s->stick_ry = up > 1 ? 1 : up < -1 ? -1 : (fabsf(up) < 0.15f ? 0 : up);
        s->stick_ry_set = true;
        if (ahead > VR_G_MAG_PUSH) b |= 1ull << VRB_UP;
        else if (ahead < -VR_G_MAG_PUSH) b |= 1ull << VRB_DOWN;
    }
    // left trigger: lock on / shield
    if (in->trigger[0]) b |= 1ull << VRB_ZL;
    // parry: the left hand thrust at speed while guarding
    double dt = now - s->prev_now;
    if (dt > 1e-4 && dt < 0.2 && in->trigger[0] && (in->hand_speed[0] - s->prev_speed0) / (float)dt > VR_G_PARRY_ACCEL)
        vr_g_tap(s, VRB_A, now), s->haptic[0] = 2;
    s->prev_speed0 = in->hand_speed[0];
    s->prev_now = now;
    // right A: grab (tap) / run (held: B, which also puts the weapon away)
    if (in->a && !s->a_was) s->a_down = now;
    if (in->a && now - s->a_down >= VR_G_RUN_HOLD) b |= 1ull << VRB_B, s->held = VR_HELD_NONE;
    if (!in->a && s->a_was && now - s->a_down < VR_G_RUN_HOLD) vr_g_tap(s, VRB_A, now);
    s->a_was = in->a;
    // right B: jump
    if (in->b) b |= 1ull << VRB_X;
    // left Y: a tap the rune, held its quick menu (d-pad up while held)
    if (in->y && !s->y_was) s->y_down = now, s->y_menu = false;
    if (in->y && now - s->y_down >= VR_G_LONG) s->y_menu = true, b |= 1ull << VR_G_ABILITY_MENU;
    if (!in->y && s->y_was) {
        if (!s->y_menu) vr_g_tap(s, VRB_L, now);
        s->held = VR_HELD_RUNE;
    }
    s->y_was = in->y;
    // left X held: the map
    if (in->x) b |= 1ull << VRB_MINUS;
    // left stick click: a tap crouch, held the scope; right stick click: a tap the inventory, held the map
    if (in->lclick && !s->lclick_was) s->lclick_down = now, s->lclick_long = false;
    if (in->lclick && now - s->lclick_down >= VR_G_STICK_HOLD) s->lclick_long = true, b |= 1ull << VRB_RSTICK;
    if (!in->lclick && s->lclick_was && !s->lclick_long) vr_g_tap(s, VRB_LSTICK, now);
    s->lclick_was = in->lclick;
    if (in->rclick && !s->rclick_was) s->rclick_down = now, s->rclick_long = false;
    if (in->rclick && now - s->rclick_down >= VR_G_STICK_HOLD) s->rclick_long = true, b |= 1ull << VRB_MINUS;
    if (!in->rclick && s->rclick_was && !s->rclick_long) vr_g_tap(s, VRB_PLUS, now);
    s->rclick_was = in->rclick;
    for (int k = 0; k < 16; k++)
        if (now < s->tap_until[k]) b |= 1ull << k;
    if (b & (1ull << VRB_B)) s->held = VR_HELD_NONE;
    return b;
}
