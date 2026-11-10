// runtime/vr/vr_input_map.h: what each VR controller input becomes for the game.
#include "vr_input_map.h"
#include "vr_test.h"

#define BIT(n) (1ull << (n))

static vr_pad_out map(vr_pad_state *s, vr_pad_in in, double t) {
    vr_pad_out o;
    vr_pad_map(s, &in, t, &o);
    return o;
}

static void test_buttons(void) {
    VR_TEST("buttons");
    vr_pad_state s = {0};
    vr_pad_in in = {0};
    CHECK_EQ_INT(map(&s, in, 0).buttons, 0);
    in.a = in.b = in.x = in.y = true;
    CHECK_EQ_INT(map(&s, in, 0).buttons, BIT(VRB_A) | BIT(VRB_B) | BIT(VRB_X) | BIT(VRB_Y));
    in = (vr_pad_in){.lclick = true, .rclick = true};
    CHECK_EQ_INT(map(&s, in, 0).buttons, BIT(VRB_LSTICK) | BIT(VRB_RSTICK));
    // triggers and grips: pressed from half way
    in = (vr_pad_in){.ltrigger = 0.49f, .rtrigger = 0.49f, .rgrip = 0.49f, .lgrip = 0.49f};
    CHECK_EQ_INT(map(&s, in, 0).buttons, 0);
    in = (vr_pad_in){.ltrigger = 0.5f, .rtrigger = 1.0f, .rgrip = 0.8f, .lgrip = 0.9f};
    CHECK_EQ_INT(map(&s, in, 0).buttons, BIT(VRB_ZL) | BIT(VRB_ZR) | BIT(VRB_R) | BIT(VRB_L));
}

static void test_sticks(void) {
    VR_TEST("sticks");
    vr_pad_state s = {0};
    vr_pad_out o = map(&s, (vr_pad_in){.lx = 0.1f, .ly = -0.05f}, 0);  // (inside the dead zone)
    CHECK_EQ_INT(o.lx, 0);
    CHECK_EQ_INT(o.ly, 0);
    o = map(&s, (vr_pad_in){.lx = 0, .ly = 1, .rx = -1, .ry = 0}, 0);  // full: the rim
    CHECK_EQ_INT(o.lx, 0);
    CHECK_EQ_INT(o.ly, 32767);
    CHECK_EQ_INT(o.rx, -32767);
    o = map(&s, (vr_pad_in){.ly = VR_PAD_DEAD + (1 - VR_PAD_DEAD) * 0.5f}, 0);  // half way past the dead zone: half
    CHECK_NEAR(o.ly, 16384, 2);
    o = map(&s, (vr_pad_in){.lx = 1, .ly = 1}, 0);  // a corner of a square pad: kept in the circle
    CHECK_NEAR(sqrt((double)o.lx * o.lx + (double)o.ly * o.ly), 32767, 2);
    CHECK_EQ_INT(o.lx, o.ly);
}

static void test_dpad(void) {
    VR_TEST("d-pad");
    vr_pad_state s = {0};
    // the grip alone: L
    CHECK_EQ_INT(map(&s, (vr_pad_in){.lgrip = 1}, 0).buttons, BIT(VRB_L));
    // ... then the right stick up: the d-pad's up; no L, no camera
    vr_pad_out o = map(&s, (vr_pad_in){.lgrip = 1, .ry = 0.9f}, 0.1);
    CHECK_EQ_INT(o.buttons, BIT(VRB_UP));
    CHECK_EQ_INT(o.rx, 0);
    CHECK_EQ_INT(o.ry, 0);
    // the stick back in the middle, the grip still held: nothing (not L again)
    o = map(&s, (vr_pad_in){.lgrip = 1, .ry = 0.1f}, 0.2);
    CHECK_EQ_INT(o.buttons, 0);
    // right, left, down
    CHECK_EQ_INT(map(&s, (vr_pad_in){.lgrip = 1, .rx = 0.8f, .ry = 0.3f}, 0.3).buttons, BIT(VRB_RIGHT));
    CHECK_EQ_INT(map(&s, (vr_pad_in){.lgrip = 1, .rx = -0.8f}, 0.4).buttons, BIT(VRB_LEFT));
    CHECK_EQ_INT(map(&s, (vr_pad_in){.lgrip = 1, .ry = -0.7f}, 0.5).buttons, BIT(VRB_DOWN));
    // the grip let go: the stick is the camera again
    o = map(&s, (vr_pad_in){.ry = -0.7f}, 0.6);
    CHECK_EQ_INT(o.buttons, 0);
    CHECK(o.ry < -10000);
    // a small stick motion with the grip held is not a d-pad: L stays, the camera moves
    o = map(&s, (vr_pad_in){.lgrip = 1, .rx = 0.4f}, 0.7);
    CHECK_EQ_INT(o.buttons, BIT(VRB_L));
    CHECK(o.rx > 0);
}

static void test_menu(void) {
    VR_TEST("menu");
    vr_pad_state s = {0};
    // a tap: + after it is let go, for VR_PAD_TAP
    CHECK_EQ_INT(map(&s, (vr_pad_in){.menu = true}, 10.0).buttons, 0);
    CHECK_EQ_INT(map(&s, (vr_pad_in){.menu = true}, 10.2).buttons, 0);
    CHECK_EQ_INT(map(&s, (vr_pad_in){0}, 10.3).buttons, BIT(VRB_PLUS));
    CHECK_EQ_INT(map(&s, (vr_pad_in){0}, 10.35).buttons, BIT(VRB_PLUS));
    CHECK_EQ_INT(map(&s, (vr_pad_in){0}, 10.45).buttons, 0);
    // held: - from VR_PAD_MENU_HOLD on, for as long as it is held, and no + after
    CHECK_EQ_INT(map(&s, (vr_pad_in){.menu = true}, 20.0).buttons, 0);
    CHECK_EQ_INT(map(&s, (vr_pad_in){.menu = true}, 20.59).buttons, 0);
    CHECK_EQ_INT(map(&s, (vr_pad_in){.menu = true}, 20.6).buttons, BIT(VRB_MINUS));
    CHECK_EQ_INT(map(&s, (vr_pad_in){.menu = true}, 21.5).buttons, BIT(VRB_MINUS));
    CHECK_EQ_INT(map(&s, (vr_pad_in){0}, 21.6).buttons, 0);
    CHECK_EQ_INT(map(&s, (vr_pad_in){0}, 21.65).buttons, 0);
}

static void test_snap(void) {
    VR_TEST("snap turning");
    vr_pad_state s = {.snap = true};
    vr_pad_out o = map(&s, (vr_pad_in){0}, 0);
    CHECK_EQ_INT(o.snap, 0);
    o = map(&s, (vr_pad_in){.rx = 0.5f}, 0.1);  // (not far enough)
    CHECK_EQ_INT(o.snap, 0);
    CHECK_EQ_INT(o.rx, 0);  // (sideways is never the camera's)
    o = map(&s, (vr_pad_in){.rx = 0.9f, .ry = 0.5f}, 0.2);  // a step right; the up-down still goes to the game
    CHECK_EQ_INT(o.snap, 1);
    CHECK_EQ_INT(o.rx, 0);
    CHECK(o.ry > 0);
    o = map(&s, (vr_pad_in){.rx = 1.0f}, 0.3);  // held: no second step
    CHECK_EQ_INT(o.snap, 0);
    o = map(&s, (vr_pad_in){.rx = 0.4f}, 0.4);  // (not back far enough to re-arm)
    o = map(&s, (vr_pad_in){.rx = 0.9f}, 0.5);
    CHECK_EQ_INT(o.snap, 0);
    o = map(&s, (vr_pad_in){.rx = 0.1f}, 0.6);  // back in the middle, then left: a step left
    o = map(&s, (vr_pad_in){.rx = -0.8f}, 0.7);
    CHECK_EQ_INT(o.snap, -1);
    // the d-pad chord takes the stick, not snap turning
    vr_pad_state d = {.snap = true};
    map(&d, (vr_pad_in){0}, 0);
    o = map(&d, (vr_pad_in){.lgrip = 1, .rx = 0.9f}, 0.1);
    CHECK_EQ_INT(o.snap, 0);
    CHECK_EQ_INT(o.buttons, BIT(VRB_RIGHT));
    // off: the stick is the camera's, no steps
    vr_pad_state f = {0};
    o = map(&f, (vr_pad_in){.rx = 0.9f}, 0);
    CHECK_EQ_INT(o.snap, 0);
    CHECK(o.rx > 20000);
}

int main(void) {
    test_buttons();
    test_sticks();
    test_dpad();
    test_menu();
    test_snap();
    return VR_TEST_END("test_vr_input_map");
}
