// runtime/vr_gestures.h: BetterVR's body slots and control scheme. A standing player looking along -z, head at
// 1.7 m; hands put over each slot, grips and buttons pressed over time, and the game buttons that come out checked.
#include "vr_gestures.h"
#include "vr_test.h"

#define BIT(k) (1ull << (k))

static vr_gesture_in base(double now) {
    vr_gesture_in in = {0};
    in.head = vr_g3v(0, 1.7f, 0);
    in.fwd = vr_g3v(0, 0, -1);
    in.right = vr_g3v(1, 0, 0);
    in.drop_enabled = true;
    in.hand[0] = vr_g3v(-0.25f, 1.1f, -0.35f);   // (hands ahead at the belly: no slot)
    in.hand[1] = vr_g3v(0.25f, 1.1f, -0.35f);
    in.now = now;
    return in;
}

// run frames at 90 Hz from t0 to t1 with in (its now set each frame); returns the OR of the buttons
static uint64_t run(vr_gesture_state *s, vr_gesture_in in, double t0, double t1) {
    uint64_t all = 0;
    for (double t = t0; t < t1; t += 1.0 / 90) {
        in.now = t;
        all |= vr_gestures(s, &in);
    }
    return all;
}

static const vr_g3 RSHOULDER = {0.15f, 1.65f, 0.12f}, LSHOULDER = {-0.15f, 1.65f, 0.12f},
                   LWAIST = {-0.25f, 1.0f, 0.05f}, RWAIST = {0.25f, 1.0f, 0.05f}, MOUTH = {0.02f, 1.62f, -0.1f};

int main(void) {
    VR_TEST("slots");
    vr_g3 h = vr_g3v(0, 1.7f, 0), f = vr_g3v(0, 0, -1), r = vr_g3v(1, 0, 0);
    CHECK_EQ_INT(vr_gesture_slot(h, f, r, RSHOULDER), VR_SLOT_RSHOULDER);
    CHECK_EQ_INT(vr_gesture_slot(h, f, r, LSHOULDER), VR_SLOT_LSHOULDER);
    CHECK_EQ_INT(vr_gesture_slot(h, f, r, LWAIST), VR_SLOT_LWAIST);
    CHECK_EQ_INT(vr_gesture_slot(h, f, r, RWAIST), VR_SLOT_RWAIST);
    CHECK_EQ_INT(vr_gesture_slot(h, f, r, MOUTH), VR_SLOT_MOUTH);
    CHECK_EQ_INT(vr_gesture_slot(h, f, r, vr_g3v(0.3f, 1.2f, -0.4f)), VR_SLOT_NONE);       // ahead
    CHECK_EQ_INT(vr_gesture_slot(h, f, r, vr_g3v(0.5f, 1.7f, 0.3f)), VR_SLOT_NONE);        // behind but far
    // the head turned 90 deg left (looking along -x): the right shoulder is now behind +x... rotated with it
    vr_g3 f2 = vr_g3v(-1, 0, 0), r2 = vr_g3v(0, 0, -1);
    CHECK_EQ_INT(vr_gesture_slot(h, f2, r2, vr_g3v(0.12f, 1.65f, -0.15f)), VR_SLOT_RSHOULDER);
    // a head pitched down: its flattened forward still decides behind / ahead
    vr_g3 f3 = vr_g3v(0, -0.7f, -0.714f);
    CHECK_EQ_INT(vr_gesture_slot(h, f3, r, RSHOULDER), VR_SLOT_RSHOULDER);

    VR_TEST("equip and put away from the shoulders and the waist");
    vr_gesture_state s = {0};
    vr_gesture_in in = base(0);
    CHECK(run(&s, in, 0, 0.2) == 0);   // nothing pressed, no slot: nothing
    in.hand[1] = RSHOULDER;
    in.grip[1] = true;
    uint64_t b = run(&s, in, 0.2, 0.3);
    in.grip[1] = false;
    b |= run(&s, in, 0.3, 0.5);
    CHECK(b & BIT(VRB_Y));
    CHECK(!(b & BIT(VRB_A)));
    CHECK_EQ_INT(s.held, VR_HELD_MELEE);
    in.grip[1] = true;   // again: put away
    b = run(&s, in, 0.5, 0.6);
    in.grip[1] = false;
    b |= run(&s, in, 0.6, 0.62);
    CHECK(b & BIT(VRB_B));
    CHECK_EQ_INT(s.held, VR_HELD_NONE);
    in.hand[1] = base(0).hand[1];
    run(&s, in, 0.62, 0.8);
    in.hand[0] = LSHOULDER;   // the left hand over the left shoulder: the bow
    in.grip[0] = true;
    b = run(&s, in, 0.8, 0.9);
    in.grip[0] = false;
    b |= run(&s, in, 0.9, 0.92);
    CHECK(b & BIT(VRB_ZR));
    CHECK_EQ_INT(s.held, VR_HELD_BOW);
    in.hand[0] = LWAIST;
    run(&s, in, 0.92, 1.0);
    in.grip[0] = true;   // the left waist: the rune
    b = run(&s, in, 1.0, 1.1);
    in.grip[0] = false;
    b |= run(&s, in, 1.1, 1.12);
    CHECK(b & BIT(VRB_L));
    CHECK_EQ_INT(s.held, VR_HELD_RUNE);
    run(&s, in, 1.12, 1.3);
    CHECK(s.haptic[0] == 0);

    VR_TEST("quick menus: held over a slot, open while held, the choice taken out on letting go");
    vr_gesture_state q = {0};
    in = base(0);
    in.hand[1] = RSHOULDER;
    in.grip[1] = true;
    b = run(&q, in, 0, 0.45);
    CHECK(!(b & BIT(VRB_RIGHT)));   // not yet
    b = run(&q, in, 0.45, 1.5);
    CHECK(b & BIT(VRB_RIGHT));      // the weapons menu, held
    CHECK(q.menu_open[1]);
    in.grip[1] = false;
    b = run(&q, in, 1.5, 1.6);
    CHECK(!(b & BIT(VRB_RIGHT)));
    CHECK(b & BIT(VRB_Y));          // the chosen weapon out
    CHECK_EQ_INT(q.held, VR_HELD_MELEE);
    in.hand[1] = base(0).hand[1];
    in.hand[0] = LSHOULDER;          // with a melee weapon out, the left shoulder held: shields (d-pad left)
    in.grip[0] = true;
    b = run(&q, in, 1.6, 2.4);
    CHECK(b & BIT(VRB_LEFT));
    in.grip[0] = false;
    run(&q, in, 2.4, 2.6);
    in.hand[0] = LWAIST;             // the waist held: the abilities' wheel (TotK: L held)
    in.grip[0] = true;
    b = run(&q, in, 2.6, 3.4);
    CHECK(b & BIT(VRB_L));
    CHECK(!(b & BIT(VRB_UP)));
    in.grip[0] = false;
    b = run(&q, in, 3.4, 3.5);
    CHECK(!(b & BIT(VRB_L)));        // (let go: the wheel closes on its choice, no second press)
    CHECK_EQ_INT(q.held, VR_HELD_RUNE);

    VR_TEST("drop: a grip held at the right waist");
    vr_gesture_state d = {0};
    d.held = VR_HELD_MELEE;
    in = base(0);
    in.hand[1] = RWAIST;
    in.hand_held[1] = VR_HAND_MELEE;
    in.grip[1] = true;
    b = run(&d, in, 0, 0.4);
    CHECK(b == 0);                         // no grab at the waist, nothing before the hold
    b = run(&d, in, 0.4, 0.8);
    CHECK(b & BIT(VRB_RIGHT));             // the weapons' quick menu (right hand)
    CHECK(!(b & BIT(VRB_Y)));
    b = run(&d, in, 0.8, 1.6);
    CHECK(b & BIT(VRB_RIGHT));
    CHECK(b & BIT(VRB_Y));                 // the selector's drop
    b = run(&d, in, 1.95, 2.5);
    CHECK(b == 0);                         // done: the menu let go, once a press though the grip stays down
    CHECK_EQ_INT(d.held, VR_HELD_NONE);
    in.grip[1] = false;
    run(&d, in, 2.5, 2.7);
    in.hand[0] = vr_g3v(0.2f, 1.0f, 0.05f);   // the left hand reaching across to the right waist: the shields
    in.hand_held[0] = VR_HAND_SHIELD;
    in.grip[0] = true;
    b = run(&d, in, 2.7, 4.3);
    CHECK(b & BIT(VRB_LEFT));
    CHECK(b & BIT(VRB_Y));
    CHECK(!(b & BIT(VRB_A)));
    in.grip[0] = false;
    run(&d, in, 4.3, 4.8);                 // (the drop ends at 1.4 s, the grip let go or not)
    in.hand[0] = base(0).hand[0];
    in.hand[1] = RWAIST;                   // a short press at the right waist: nothing
    in.grip[1] = true;
    b = run(&d, in, 4.8, 5.0);
    in.grip[1] = false;
    b |= run(&d, in, 5.0, 5.3);
    CHECK(b == 0);

    VR_TEST("drop: a bow in the left hand opens the bows selector");
    vr_gesture_state db = {0};
    db.held = VR_HELD_BOW;
    in = base(0);
    in.hand[0] = RWAIST;
    in.hand_held[0] = VR_HAND_BOW;
    in.grip[0] = true;
    b = run(&db, in, 0, 0.8);
    CHECK(b & BIT(VRB_RIGHT));
    CHECK(!(b & BIT(VRB_LEFT)));
    b = run(&db, in, 0.8, 1.6);
    CHECK(b & BIT(VRB_Y));
    CHECK(!(b & BIT(VRB_LEFT)));
    in.grip[0] = false;
    run(&db, in, 1.6, 2.1);
    CHECK(!db.drop_t0[0] && !db.drop_done[0]);
    CHECK_EQ_INT(db.held, VR_HELD_NONE);
    in.hand_held[0] = VR_HAND_EMPTY;
    in.grip[0] = true;
    b = run(&db, in, 2.1, 3.9);
    CHECK(!(b & (BIT(VRB_LEFT) | BIT(VRB_RIGHT) | BIT(VRB_Y))));

    VR_TEST("drop: the Sheikah Slate is not droppable");
    vr_gesture_state dr = {0};
    dr.held = VR_HELD_RUNE;
    in = base(0);
    in.hand[0] = RWAIST;
    in.grip[0] = true;
    b = run(&dr, in, 0, 2.0);
    CHECK(!(b & (BIT(VRB_LEFT) | BIT(VRB_RIGHT) | BIT(VRB_Y) | BIT(VRB_A))));
    CHECK_EQ_INT(dr.held, VR_HELD_RUNE);

    VR_TEST("drop: disabled option leaves the quick selector closed");
    vr_gesture_state off = {0};
    in = base(0);
    in.drop_enabled = false;
    in.hand[1] = RWAIST;
    in.hand_held[1] = VR_HAND_MELEE;
    in.grip[1] = true;
    b = run(&off, in, 0, 2.0);
    CHECK(!(b & (BIT(VRB_LEFT) | BIT(VRB_RIGHT) | BIT(VRB_Y) | BIT(VRB_A))));

    VR_TEST("a grip that takes hold of the weapon with both hands does not grab");
    vr_gesture_state n = {0};
    in = base(0);
    in.no_grab[0] = true;
    in.grip[0] = true;
    CHECK(!(run(&n, in, 0, 0.3) & BIT(VRB_A)));
    in.grip[0] = false;
    in.no_grab[0] = false;
    run(&n, in, 0.3, 0.5);
    in.grip[0] = true;
    CHECK(run(&n, in, 0.5, 0.7) & BIT(VRB_A));   // (elsewhere it grabs again)

    VR_TEST("grab, whistle");
    vr_gesture_state g = {0};
    in = base(0);
    in.grip[0] = true;   // a grip away from every slot: grab / interact
    b = run(&g, in, 0, 0.1);
    CHECK(b & BIT(VRB_A));
    in.grip[0] = false;
    run(&g, in, 0.1, 0.3);
    in.hand[0] = vr_g3v(-0.03f, 1.62f, -0.1f);
    in.hand[1] = MOUTH;
    in.grip[0] = in.grip[1] = true;
    b = run(&g, in, 0.3, 0.6);
    CHECK(b & BIT(VRB_DOWN));
    CHECK(!(b & BIT(VRB_A)));

    VR_TEST("right trigger: use what is in hand; throw from the shoulder");
    vr_gesture_state u = {0};
    in = base(0);
    in.trigger[1] = true;
    b = run(&u, in, 0, 0.1);
    CHECK(b & BIT(VRB_Y));           // nothing out: attack takes the weapon out
    CHECK_EQ_INT(u.held, VR_HELD_MELEE);
    in.trigger[1] = false;
    run(&u, in, 0.1, 0.2);
    in.hand[1] = RSHOULDER;          // the weapon out, the hand at the shoulder, trigger held: throw
    in.trigger[1] = true;
    b = run(&u, in, 0.2, 0.6);
    CHECK(b & BIT(VRB_R));
    in.hand[1] = vr_g3v(0.2f, 1.5f, -0.6f);   // swung forward still holding: still R
    b = run(&u, in, 0.6, 0.7);
    CHECK(b & BIT(VRB_R));
    in.trigger[1] = false;           // let go: thrown
    b = run(&u, in, 0.7, 0.8);
    CHECK(!(b & BIT(VRB_R)));
    CHECK_EQ_INT(u.held, VR_HELD_NONE);
    u.held = VR_HELD_BOW;
    in.trigger[1] = true;
    b = run(&u, in, 0.8, 0.9);
    CHECK(b & BIT(VRB_ZR));
    CHECK(!(b & BIT(VRB_Y)));

    VR_TEST("buttons: A tap grab / held run, B jump, Y rune / menu, stick clicks");
    vr_gesture_state k = {0};
    in = base(0);
    in.a = true;
    b = run(&k, in, 0, 0.2);
    CHECK(!(b & BIT(VRB_B)));
    in.a = false;
    b = run(&k, in, 0.2, 0.3);
    CHECK(b & BIT(VRB_A));
    in.a = true;
    b = run(&k, in, 0.3, 1.0);
    CHECK(b & BIT(VRB_B));            // held: run
    in.a = false;
    b = run(&k, in, 1.0, 1.1);
    CHECK(!(b & BIT(VRB_A)));         // (no grab after a run)
    in.b = true;
    CHECK(run(&k, in, 1.1, 1.2) & BIT(VRB_X));
    in.b = false;
    in.y = true;
    b = run(&k, in, 1.2, 1.3);
    in.y = false;
    b |= run(&k, in, 1.3, 1.4);
    CHECK(b & BIT(VRB_L));
    CHECK(!(b & BIT(VRB_UP)));
    in.y = true;
    b = run(&k, in, 1.4, 2.2);
    CHECK(b & BIT(VRB_L));            // held: the abilities' wheel (TotK: L held)
    CHECK(!(b & BIT(VRB_UP)));
    in.y = false;
    run(&k, in, 2.2, 2.4);
    in.lclick = true;
    run(&k, in, 2.4, 2.5);
    in.lclick = false;
    CHECK(run(&k, in, 2.5, 2.6) & BIT(VRB_LSTICK));
    in.lclick = true;
    CHECK(run(&k, in, 2.6, 3.4) & BIT(VRB_RSTICK));   // held: scope
    in.lclick = false;
    run(&k, in, 3.4, 3.6);
    in.rclick = true;
    run(&k, in, 3.6, 3.7);
    in.rclick = false;
    CHECK(run(&k, in, 3.7, 3.8) & BIT(VRB_PLUS));
    in.rclick = true;
    CHECK(run(&k, in, 3.8, 4.6) & BIT(VRB_MINUS));

    VR_TEST("parry: a fast left thrust while guarding");
    vr_gesture_state p = {0};
    in = base(0);
    in.trigger[0] = true;
    for (int i = 0; i < 20; i++) {
        in.now = i / 90.0;
        in.hand_speed[0] = 0.3f;
        vr_gestures(&p, &in);
    }
    in.now = 20 / 90.0;
    in.hand_speed[0] = 1.0f;   // +0.7 m/s in a frame: 63 m/s^2
    CHECK(vr_gestures(&p, &in) & BIT(VRB_A));
    vr_gesture_state p2 = {0};
    in.trigger[0] = false;     // not guarding: no parry
    for (int i = 0; i < 20; i++) {
        in.now = i / 90.0;
        in.hand_speed[0] = i == 19 ? 1.0f : 0.3f;
        CHECK(!(vr_gestures(&p2, &in) & BIT(VRB_A)));
    }
    VR_TEST("magnesis: a rune out, a grip held: rise and fall to the stick, push and pull to the d-pad");
    vr_gesture_state m = {0};
    m.held = VR_HELD_RUNE;
    in = base(0);
    in.grip[1] = true;
    run(&m, in, 0, 0.1);
    CHECK(m.stick_ry_set);
    CHECK_NEAR(m.stick_ry, 0, 1e-6);
    in.hand[1].y += 0.15f;   // up 15 cm: the stick fully up
    b = run(&m, in, 0.1, 0.2);
    CHECK_NEAR(m.stick_ry, 1, 1e-4);
    in.hand[1].y -= 0.30f;   // down: fully down
    run(&m, in, 0.2, 0.3);
    CHECK_NEAR(m.stick_ry, -1, 1e-4);
    in.hand[1].y += 0.15f;
    in.hand[1].z -= 0.12f;   // pushed away (ahead is -z): d-pad up
    b = run(&m, in, 0.3, 0.4);
    CHECK(b & BIT(VRB_UP));
    in.hand[1].z += 0.24f;   // pulled near: d-pad down
    b = run(&m, in, 0.4, 0.5);
    CHECK(b & BIT(VRB_DOWN));
    in.grip[1] = false;      // let go: nothing more
    run(&m, in, 0.5, 0.6);
    CHECK(!m.stick_ry_set);
    vr_gesture_state m2 = {0};   // no rune out: a grip is a grab, no magnesis
    in = base(0);
    in.grip[1] = true;
    in.hand[1].y += 0.2f;
    run(&m2, in, 0, 0.2);
    CHECK(!m2.stick_ry_set);
    VR_TEST_END("test_vr_gestures");
}
