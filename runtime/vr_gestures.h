// BetterVR's body gestures and control scheme, for runtime/vr_input.c. Header only and free of the rest of the
// runtime: tests/vr/test_vr_gestures.c checks it. From BOTW VR (guideahon/BOTW-VR-RECOMP runtime/vr/vr_gestures.h,
// 2026-11-21), which ported it in C from BotW-BetterVR (Crementif and contributors, MIT: src/hooking/controls.cpp
// calculateHandGesture, openDpadMenuBodySlots, processLeftHandInGameInput, processRightHandInGameInput). TotK's
// controls differ from BotW's in one place used here: the abilities (BotW's runes) are on L, a tap uses the one chosen
// and L held opens their wheel (BotW: the runes' menu on d-pad up). VR_G_ABILITY_MENU is that button. As in BotW, A
// uses the ability chosen (Ultrahand's grab, Fuse, Ascend...).
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
//   right trigger   use what is in hand: the melee weapon attacks (Y), the bow draws (ZR), an ability is used (A); over
//                   the right shoulder with a weapon out, held: throw (R, let go to throw)
//   right A         grab / interact (A); held 0.4 s: run (B);         right B   jump (X)
//                   climbing or gliding: let go (B); riding: A, the grips A / B (spur / brake)
//   riding, an empty hand patting low ahead (the horse's neck): pet the horse (L)
//   right stick     camera; its click: a tap the inventory (+), held the scope (R stick)
//   left trigger    lock on / shield (ZL)                              left Y    a tap the ability (L), held its wheel
//   left stick      move; its click: a tap crouch (L stick), held run (B, while held)
//   left X          (BetterVR's own menu there; here: held, the game's map)
//   left hand swung at the face while guarding (ZL held): parry (A)
//
// The Quest layout (in->quest; TOTK_VR_CONTROLS=quest, the default since 2026-11-20, the player's own): the body
// slots as above, and
//   right A         A (accept, interact; not while guarding)            right B   B (cancel, put away: a tap, never run)
//   right stick up  jump (X; first person: the head looks, the stick's up-down is free)
//   right trigger   use the right hand's: attack (Y), the bow drawn (ZR), the rune (A); held 0.6 s with no bow or rune
//                   out: the weapons' quick menu (d-pad right) while held
//   left trigger    the left hand's: guard / lock on (ZL) and a parry at the press (A a moment after); held 0.7 s:
//                   the shields' quick menu (d-pad left) while held
//   right grip      away from the slots: the bow out (again: put away); held: the bows' quick menu (the bow drawn,
//                   ZR, then d-pad right)
//   left grip       away from the slots: grab / interact (A); with the bow out, held: the arrows' quick menu (ZR, then
//                   d-pad left)
//   left Y          the ability (L), held its wheel                     left X    the map (-)
//   menu button     the pause menu (+)                                  sticks' clicks as above
//   both hands at the mouth (within VR_G_MOUTH_R_QUEST), both triggers held: whistle (d-pad down); with both hands
//   there the triggers do nothing else (no guard, attack or parry while raising them)
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
enum { VR_SLOT_NONE, VR_SLOT_RSHOULDER, VR_SLOT_LSHOULDER, VR_SLOT_LWAIST, VR_SLOT_RWAIST, VR_SLOT_MOUTH,
       VR_SLOT_BOWGRIP,     // (the Quest layout's right grip away from the slots: the bow, as the left shoulder)
       VR_SLOT_ARROWGRIP }; // (... its left grip away from them with the bow out: the arrows' menu when held)

#define VR_G_SHOULDER_R 0.35f
#define VR_G_MOUTH_R 0.2f
#define VR_G_WAIST_Y 0.45f
#define VR_G_WAIST_BEHIND (-0.05f)
#define VR_G_WAIST_BEHIND_SEATED 0.15f   // seated players lean back: their hips are ahead of the head (BetterVR 1.0.1)
#define VR_G_MOUTH_AHEAD 0.08f           // the mouth: below and ahead of the eyes, where cupped hands end up (BetterVR)
#define VR_G_MOUTH_BELOW 0.10f
#define VR_G_WAIST_SETTLED 1.2f          // m/s: a hand faster than this only swings past the hip (no rune out)
#define VR_G_LONG 0.5            // a grip held this long (s) over a slot: its quick menu
#define VR_G_RUN_HOLD 0.4        // right A held this long: run
#define VR_G_STICK_HOLD 0.5      // a stick click held this long: its second action
#define VR_G_L3_RUN 0.25         // the left stick's click held this long: run (B) while it stays held
#define VR_G_TRIG_MENU_R 0.6     // the Quest layout: the right trigger held this long opens the weapons' quick menu
#define VR_G_TRIG_MENU_L 0.7     // ... the left trigger, the shields'
#define VR_G_JUMP_STICK 0.7f     // ... the right stick pushed up this far: jump
#define VR_G_MOUTH_R_QUEST 0.28f // ... both hands within this of the mouth: the triggers whistle (the grips' 0.2 m
                                 // was too tight in a Quest 3, 2026-11-21)
#define VR_G_PARRY_TRIGGER 0.06  // ... the left trigger's press: A this long after the guard, the parry
#define VR_G_BOW_MENU_DRAW 0.3   // ... the bows' / arrows' menu: the bow drawn (ZR) this long before the d-pad
#define VR_G_TAP 0.12            // a tap's button is held this long for the game to see it
#define VR_G_ABILITY_MENU VRB_L  // the abilities' menu (TotK: L held opens their wheel; BotW: d-pad up)
#define VR_G_PAT_LOW 0.55f       // riding: an empty hand this far under the eyes, ahead (the horse's neck)...
#define VR_G_PAT_SPEED 0.9f      // ... moving at least this fast (m/s): a pat, the game's L (pet the horse)
#define VR_G_PAT_EVERY 0.6       // s between pats
// parry: while guarding, the left hand pushed this far (m) within VR_G_PARRY_WINDOW (s), ahead or away from the head,
// moving at least VR_G_PARRY_SPEED (m/s) - a push of the shield, not the tracking's jitter or a small move. (A rise of
// speed of 35 m/s^2 between two frames was enough before, 2026-11-20: too sensitive; then 18 cm ahead in 0.15 s at
// 1.6 m/s: never, in a Quest 3.) Then none for VR_G_PARRY_EVERY (s)
#define VR_G_PARRY_THRUST 0.10f
#define VR_G_PARRY_WINDOW 0.2
#define VR_G_PARRY_SPEED 1.0f
#define VR_G_PARRY_EVERY 0.6

static inline vr_g3 vr_g3v(float x, float y, float z) { vr_g3 r = {x, y, z}; return r; }
static inline vr_g3 vr_g3sub(vr_g3 a, vr_g3 b) { return vr_g3v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline float vr_g3dot(vr_g3 a, vr_g3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// which body slot the hand is over. head: position; fwd, right: the head's forward and right (unit, any pitch);
// seated: the player sits (Options > VR > Play position)
static inline int vr_gesture_slot_ex(vr_g3 head, vr_g3 fwd, vr_g3 right, vr_g3 hand, bool seated) {
    vr_g3 f = vr_g3v(fwd.x, 0, fwd.z);
    float fl = sqrtf(f.x * f.x + f.z * f.z);
    f = fl > 1e-4f ? vr_g3v(f.x / fl, 0, f.z / fl) : vr_g3v(0, 0, -1);
    vr_g3 d = vr_g3sub(hand, head);
    float dist2 = vr_g3dot(d, d);
    float ahead = vr_g3dot(f, d);
    bool left = vr_g3dot(right, d) < 0;
    if (ahead < 0 && dist2 < VR_G_SHOULDER_R * VR_G_SHOULDER_R) return left ? VR_SLOT_LSHOULDER : VR_SLOT_RSHOULDER;
    if (ahead - (seated ? VR_G_WAIST_BEHIND_SEATED : -VR_G_WAIST_BEHIND) < 0 && hand.y < head.y - VR_G_WAIST_Y)
        return left ? VR_SLOT_LWAIST : VR_SLOT_RWAIST;
    vr_g3 m = vr_g3sub(d, vr_g3v(f.x * VR_G_MOUTH_AHEAD, -VR_G_MOUTH_BELOW, f.z * VR_G_MOUTH_AHEAD));   // (from the mouth)
    if (vr_g3dot(m, m) < VR_G_MOUTH_R * VR_G_MOUTH_R) return VR_SLOT_MOUTH;
    return VR_SLOT_NONE;
}
// the middle of each body slot as vr_gesture_slot_ex finds them (for the guide that shows them, BetterVR's world
// guide): out[VR_SLOT_RSHOULDER .. VR_SLOT_MOUTH] (out[0] unused)
static inline void vr_gesture_slot_centres(vr_g3 head, vr_g3 fwd, vr_g3 right, bool seated, vr_g3 out[VR_SLOT_MOUTH + 1]) {
    vr_g3 f = vr_g3v(fwd.x, 0, fwd.z);
    float fl = sqrtf(f.x * f.x + f.z * f.z);
    f = fl > 1e-4f ? vr_g3v(f.x / fl, 0, f.z / fl) : vr_g3v(0, 0, -1);
    vr_g3 r = vr_g3v(-f.z, 0, f.x);   // (level: the head's roll left out)
    (void)right;
    float wa = seated ? VR_G_WAIST_BEHIND_SEATED * 0.5f : 0.0f;   // (the hips' slots: ahead of the line they start at)
    out[0] = head;
    out[VR_SLOT_RSHOULDER] = vr_g3v(head.x - f.x * 0.15f + r.x * 0.18f, head.y - 0.12f, head.z - f.z * 0.15f + r.z * 0.18f);
    out[VR_SLOT_LSHOULDER] = vr_g3v(head.x - f.x * 0.15f - r.x * 0.18f, head.y - 0.12f, head.z - f.z * 0.15f - r.z * 0.18f);
    out[VR_SLOT_LWAIST] = vr_g3v(head.x + f.x * wa - r.x * 0.2f, head.y - VR_G_WAIST_Y - 0.2f, head.z + f.z * wa - r.z * 0.2f);
    out[VR_SLOT_RWAIST] = vr_g3v(head.x + f.x * wa + r.x * 0.2f, head.y - VR_G_WAIST_Y - 0.2f, head.z + f.z * wa + r.z * 0.2f);
    out[VR_SLOT_MOUTH] = vr_g3v(head.x + f.x * VR_G_MOUTH_AHEAD, head.y - VR_G_MOUTH_BELOW, head.z + f.z * VR_G_MOUTH_AHEAD);
}

static inline int vr_gesture_slot(vr_g3 head, vr_g3 fwd, vr_g3 right, vr_g3 hand) {
    return vr_gesture_slot_ex(head, fwd, right, hand, false);
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
    bool seated;                // the player sits: the hips' slots further forward
    bool climbing;              // Link climbs a wall or glides (the player's movement flags)
    bool riding;                // Link is on a horse
    bool quest;                 // the Quest layout (above)
    float ry;                   // the right stick's up-down (-1..1)
    bool magnesis;              // Magnesis held moved by the hand (vr_magnesis.c: the right stick pulls or pushes it)
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
    bool a_was, b_was, y_was, lclick_was, rclick_was, x_was;
    double a_down, y_down, lclick_down, rclick_down;
    bool y_menu, lclick_long, rclick_long;
    bool throwing;              // the right trigger is holding R for a throw
    vr_g3 parry_pos[48];        // the left hand's recent positions (a ring) and when
    double parry_t[48];
    int parry_i;
    double parry_next;          // no parry before then
    int haptic[2];              // set when a slot is entered or a gesture fires: the caller pulses that hand
    int slot_now[2];
    // magnesis by hand (BetterVR's magnesis gestures): with a rune out, a grip held away from the slots moves what the
    // rune holds by the hand's motion from where the grip went down
    bool mag[2];
    vr_g3 mag_from[2];
    float stick_ry;             // out: the right stick's up-down to give the game (valid when stick_ry_set)
    bool stick_ry_set;
    double menu_t0[2];          // when each hand's quick menu opened
    double parry_at;            // the Quest layout: the left trigger's parry (A) due then (0: none)
    double bow_close;           // ... the bows' / arrows' menu let go then: its draw being cancelled (0: none)
    double trig_down[2];        // the Quest layout: when each trigger went down, and whether its quick menu is open
    bool trig_was[2], trig_menu[2];
    double pat_next;            // riding: no pat before then
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
    // the Quest layout's whistle: both hands at the mouth, both triggers held. With both hands there the triggers are
    // kept from everything else (a copy of the input without them)
    vr_gesture_in quiet;
    bool qwhistle = false;
    if (in->quest) {
        vr_g3 f = vr_g3v(in->fwd.x, 0, in->fwd.z);
        float fl = sqrtf(f.x * f.x + f.z * f.z);
        f = fl > 1e-4f ? vr_g3v(f.x / fl, 0, f.z / fl) : vr_g3v(0, 0, -1);
        vr_g3 mouth = vr_g3v(in->head.x + f.x * VR_G_MOUTH_AHEAD, in->head.y - VR_G_MOUTH_BELOW, in->head.z + f.z * VR_G_MOUTH_AHEAD);
        bool at = true;
        for (int h = 0; h < 2; h++) {
            vr_g3 d = vr_g3sub(in->hand[h], mouth);
            if (vr_g3dot(d, d) > VR_G_MOUTH_R_QUEST * VR_G_MOUTH_R_QUEST) at = false;
        }
        if (at) {
            qwhistle = in->trigger[0] && in->trigger[1];
            quiet = *in;
            quiet.trigger[0] = quiet.trigger[1] = false;
            in = &quiet;
            s->parry_at = 0;
        }
    }
    int slot[2];
    for (int h = 0; h < 2; h++) {
        slot[h] = vr_gesture_slot_ex(in->head, in->fwd, in->right, in->hand[h], in->seated);
        s->haptic[h] = slot[h] != VR_SLOT_NONE && slot[h] != s->slot_now[h];
        s->slot_now[h] = slot[h];
    }
    bool whistle = (in->grip[0] && in->grip[1] && slot[0] == VR_SLOT_MOUTH && slot[1] == VR_SLOT_MOUTH) || qwhistle;
    if (whistle) b |= 1ull << VRB_DOWN;
    // the grips: a slot's item (press), its quick menu (held), else grab
    for (int h = 0; h < 2; h++) {
        bool g = in->grip[h];
        bool slotted = s->grip_slot[h] == VR_SLOT_RSHOULDER || s->grip_slot[h] == VR_SLOT_LSHOULDER ||
                       s->grip_slot[h] == VR_SLOT_LWAIST || s->grip_slot[h] == VR_SLOT_BOWGRIP;
        if (g && !s->grip_was[h]) {
            s->grip_down[h] = now;
            // (a hand swinging past the left hip - a slash's follow-through - takes no rune out: BetterVR 1.0.1)
            s->grip_slot[h] = slot[h] == VR_SLOT_LWAIST && in->hand_speed[h] > VR_G_WAIST_SETTLED ? VR_SLOT_NONE : slot[h];
            // (the Quest layout: the right grip away from the slots is the bow's)
            if (in->quest && h == 1 && s->grip_slot[h] == VR_SLOT_NONE && !whistle && !in->no_grab[h] && !in->riding &&
                s->held != VR_HELD_RUNE)
                s->grip_slot[h] = VR_SLOT_BOWGRIP;
            // (... and its left grip, with the bow out: the arrows' menu when held; no grab)
            if (in->quest && h == 0 && s->grip_slot[h] == VR_SLOT_NONE && !whistle && !in->riding && s->held == VR_HELD_BOW)
                s->grip_slot[h] = VR_SLOT_ARROWGRIP;
            slotted = s->grip_slot[h] == VR_SLOT_RSHOULDER || s->grip_slot[h] == VR_SLOT_LSHOULDER ||
                      s->grip_slot[h] == VR_SLOT_LWAIST || s->grip_slot[h] == VR_SLOT_BOWGRIP;
            // (not with a rune out: the grip steers Magnesis, and A would let go of what it holds - BetterVR 1.0.1)
            if (!whistle && !slotted && s->grip_slot[h] != VR_SLOT_ARROWGRIP && slot[h] != VR_SLOT_MOUTH &&
                slot[h] != VR_SLOT_RWAIST && slot[h] != VR_SLOT_LWAIST && !in->no_grab[h] && s->held != VR_HELD_RUNE &&
                !in->riding)
                vr_g_tap(s, VRB_A, now);   // grab / interact
        }
        // a short press over a slot (let go before the quick menu): its item out, or put away when it is out
        if (!g && s->grip_was[h] && slotted && !s->menu_open[h] && now - s->grip_down[h] < VR_G_LONG) {
            int sl = s->grip_slot[h];
            int want = sl == VR_SLOT_RSHOULDER ? VR_HELD_MELEE : sl == VR_SLOT_LSHOULDER || sl == VR_SLOT_BOWGRIP ? VR_HELD_BOW
                     : VR_HELD_RUNE;
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
            (sl == VR_SLOT_RSHOULDER || sl == VR_SLOT_LSHOULDER || sl == VR_SLOT_LWAIST || sl == VR_SLOT_BOWGRIP ||
             sl == VR_SLOT_ARROWGRIP)) {
            s->menu_open[h] = true;
            s->menu_t0[h] = now;
            if (sl == VR_SLOT_RSHOULDER) s->menu_button[h] = VRB_RIGHT, s->menu_take = VR_HELD_MELEE;
            else if (sl == VR_SLOT_BOWGRIP) s->menu_button[h] = VRB_RIGHT, s->menu_take = VR_HELD_BOW;   // (the bows)
            else if (sl == VR_SLOT_ARROWGRIP) s->menu_button[h] = VRB_LEFT, s->menu_take = VR_HELD_BOW;  // (the arrows)
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
            // (the bows' and arrows' menus are the bow drawn's d-pad: ZR held, the d-pad once the draw has begun)
            bool bowmenu = sl == VR_SLOT_BOWGRIP || sl == VR_SLOT_ARROWGRIP;
            if (g && bowmenu) {
                b |= 1ull << VRB_ZR;
                if (now - s->menu_t0[h] >= VR_G_BOW_MENU_DRAW) b |= 1ull << s->menu_button[h];
            } else if (g) {
                b |= 1ull << s->menu_button[h];
            }
            else if (s->grip_was[h] && bowmenu) {   // let go of the bows' / arrows' menu: the draw is cancelled (B with
                s->menu_open[h] = false;              // ZR still held), so no arrow flies
                s->bow_close = now;
                s->held = VR_HELD_BOW;
            } else if (s->grip_was[h]) {   // let go: what was chosen comes out (it was put away by the press's toggle, or is new)
                s->menu_open[h] = false;
                int take = s->menu_take;
                int tb = take == VR_HELD_MELEE ? VRB_Y : take == VR_HELD_BOW ? VRB_ZR : VRB_L;
                if (tb != s->menu_button[h]) vr_g_tap(s, tb, now);   // (L let go closes the wheel on its choice)
                s->held = take;
            }
        }
        s->grip_was[h] = g;
    }
    if (s->bow_close > 0) {   // (after the bows' / arrows' menu: ZR kept a moment, B in it, then both let go)
        double t = now - s->bow_close;
        if (t < 0.2) {
            b |= 1ull << VRB_ZR;
            if (t >= 0.05 && t < 0.17) b |= 1ull << VRB_B;
        } else {
            s->bow_close = 0;
        }
    }
    // right trigger: use what is in hand; over the right shoulder with a weapon out: throw (R held, let go to throw)
    bool rt = in->trigger[1];
    if (rt && (s->throwing || (slot[1] == VR_SLOT_RSHOULDER && s->held == VR_HELD_MELEE))) {
        s->throwing = true;
        b |= 1ull << VRB_R;
    } else {
        if (s->throwing) s->held = VR_HELD_NONE;   // (thrown)
        s->throwing = false;
        if (rt && !(in->quest && s->trig_menu[1])) {
            if (s->held == VR_HELD_BOW) b |= 1ull << VRB_ZR;
            else if (s->held == VR_HELD_RUNE) b |= 1ull << VRB_A;
            else if (!in->quest) {
                b |= 1ull << VRB_Y;
                s->held = VR_HELD_MELEE;   // (attacking takes the weapon out)
            } else if (!s->trig_was[1]) {   // (the Quest layout: a press attacks once; held, the weapons' menu)
                vr_g_tap(s, VRB_Y, now);
                s->held = VR_HELD_MELEE;
            }
        }
    }
    // the Quest layout's triggers held: their hand's quick menu, open while held (the right one with no bow or rune
    // out; let go: what was chosen is taken out)
    if (in->quest) {
        for (int h = 0; h < 2; h++) {
            bool t = in->trigger[h];
            if (t && !s->trig_was[h]) {
                s->trig_down[h] = now, s->trig_menu[h] = false;
                if (h == 0) s->parry_at = now + VR_G_PARRY_TRIGGER;   // (the guard first, then A: the parry)
            }
            if (h == 0 && s->parry_at > 0 && now >= s->parry_at) {
                if (t) vr_g_tap(s, VRB_A, now), s->haptic[0] = 1;
                s->parry_at = 0;
            }
            bool may = h == 0 || (s->held != VR_HELD_BOW && s->held != VR_HELD_RUNE && !s->throwing);
            if (t && may && !s->trig_menu[h] && now - s->trig_down[h] >= (h ? VR_G_TRIG_MENU_R : VR_G_TRIG_MENU_L))
                s->trig_menu[h] = true, s->haptic[h] = 2;
            if (t && s->trig_menu[h]) b |= 1ull << (h ? VRB_RIGHT : VRB_LEFT);
            if (!t && s->trig_was[h] && s->trig_menu[h]) {
                s->trig_menu[h] = false;
                if (h) vr_g_tap(s, VRB_Y, now), s->held = VR_HELD_MELEE;
            }
            s->trig_was[h] = t;
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
    // left trigger: lock on / shield (the Quest layout: not once its menu is open)
    if (in->trigger[0] && !(in->quest && s->trig_menu[0])) b |= 1ull << VRB_ZL;
    // parry: the left hand thrust ahead, fast, while guarding
    s->parry_i = (s->parry_i + 1) % 48;
    s->parry_pos[s->parry_i] = in->hand[0], s->parry_t[s->parry_i] = now;
    if (in->trigger[0] && in->hand_speed[0] >= VR_G_PARRY_SPEED && now >= s->parry_next) {
        vr_g3 f = vr_g3v(in->fwd.x, 0, in->fwd.z);
        float fl = sqrtf(f.x * f.x + f.z * f.z);
        vr_g3 out = vr_g3sub(in->hand[0], in->head);   // (away from the head: the shield shoved out)
        float ol = sqrtf(vr_g3dot(out, out));
        float most = 0;
        for (int i = 0; i < 48 && fl > 1e-4f; i++) {
            if (s->parry_t[i] <= 0 || now - s->parry_t[i] > VR_G_PARRY_WINDOW) continue;
            vr_g3 d = vr_g3sub(in->hand[0], s->parry_pos[i]);
            float ahead = (d.x * f.x + d.z * f.z) / fl;
            float away = ol > 1e-3f ? vr_g3dot(d, out) / ol : 0;
            if (ahead > most) most = ahead;
            if (away > most) most = away;
        }
        if (most >= VR_G_PARRY_THRUST) vr_g_tap(s, VRB_A, now), s->haptic[0] = 2, s->parry_next = now + VR_G_PARRY_EVERY;
    }
    // right A: grab (tap) / run (held: B, which also puts the weapon away). Climbing or gliding it lets go at once (B
    // while held); riding it is the game's A while held (spur, interact), the right grip away from the slots A too and
    // the left one B (brake): BetterVR 1.0.1
    if (in->a && !s->a_was) s->a_down = now;
    if (in->climbing) {
        if (in->a) b |= 1ull << VRB_B;
    } else if (in->riding) {
        if (in->a) b |= 1ull << VRB_A;
        if (in->grip[1] && slot[1] == VR_SLOT_NONE && s->grip_slot[1] == VR_SLOT_NONE) b |= 1ull << VRB_A;
        if (in->grip[0] && slot[0] == VR_SLOT_NONE && s->grip_slot[0] == VR_SLOT_NONE) b |= 1ull << VRB_B;
        // a pat on the horse's neck: an empty hand low ahead, moving (the game's L, pet the horse)
        for (int h = 0; h < 2 && now >= s->pat_next; h++) {
            vr_g3 d = vr_g3sub(in->hand[h], in->head);
            float ahead = d.x * in->fwd.x + d.z * in->fwd.z;
            if (in->hand_held[h] == VR_HAND_EMPTY && !in->grip[h] && d.y < -VR_G_PAT_LOW && ahead > 0.15f &&
                in->hand_speed[h] > VR_G_PAT_SPEED)
                vr_g_tap(s, VRB_L, now), s->pat_next = now + VR_G_PAT_EVERY, s->haptic[h] = 1;
        }
    } else if (in->quest) {   // (the Quest layout: A is A - but not while guarding: A with the guard up is the game's
        if (in->a && !in->trigger[0]) b |= 1ull << VRB_A;   // parry, which is the left trigger's here)
    } else {
        if (in->a && now - s->a_down >= VR_G_RUN_HOLD) b |= 1ull << VRB_B, s->held = VR_HELD_NONE;
        if (!in->a && s->a_was && now - s->a_down < VR_G_RUN_HOLD) vr_g_tap(s, VRB_A, now);
    }
    s->a_was = in->a;
    // right B: jump (the Quest layout: B, cancel / put away / run; jump on the right stick pushed up)
    if (in->quest) {   // (B a tap: held it would run, which is the left stick's click held here)
        if (in->b && !s->b_was) vr_g_tap(s, VRB_B, now);
        s->b_was = in->b;
        if (in->ry > VR_G_JUMP_STICK && !in->magnesis) b |= 1ull << VRB_X;
    } else if (in->b) {
        b |= 1ull << VRB_X;
    }
    // left Y: a tap the ability, held its wheel (L while held)
    if (in->y && !s->y_was) s->y_down = now, s->y_menu = false;
    if (in->y && now - s->y_down >= VR_G_LONG) s->y_menu = true, b |= 1ull << VR_G_ABILITY_MENU;
    if (!in->y && s->y_was) {
        if (!s->y_menu) vr_g_tap(s, VRB_L, now);
        s->held = VR_HELD_RUNE;
    }
    s->y_was = in->y;
    // left X held: the map
    if (in->x) b |= 1ull << VRB_MINUS;
    // left stick click: a tap crouch, held run (B, as long as it is held: 2026-11-20, the player's wish; the scope moved
    // to the right click held); right stick click: a tap the inventory, held the scope (the map is left X held)
    if (in->lclick && !s->lclick_was) s->lclick_down = now, s->lclick_long = false;
    if (in->lclick && now - s->lclick_down >= VR_G_L3_RUN) s->lclick_long = true, b |= 1ull << VRB_B;
    if (!in->lclick && s->lclick_was && !s->lclick_long) vr_g_tap(s, VRB_LSTICK, now);
    s->lclick_was = in->lclick;
    if (in->rclick && !s->rclick_was) s->rclick_down = now, s->rclick_long = false;
    if (in->rclick && now - s->rclick_down >= VR_G_STICK_HOLD) s->rclick_long = true, b |= 1ull << VRB_RSTICK;
    if (!in->rclick && s->rclick_was && !s->rclick_long) vr_g_tap(s, VRB_PLUS, now);
    s->rclick_was = in->rclick;
    for (int k = 0; k < 16; k++)
        if (now < s->tap_until[k]) b |= 1ull << k;
    if ((b & (1ull << VRB_B)) && !s->bow_close) s->held = VR_HELD_NONE;   // (B cancelling the menu's draw keeps the bow)
    return b;
}
