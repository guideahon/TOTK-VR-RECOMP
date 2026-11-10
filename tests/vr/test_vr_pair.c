// runtime/vr_pair.h (from Savage Wind VR): the stereo pair's state machine. The game calls its clock and its cameras in either order within
// a frame, and that order changes with the frame rate; in every order the eyes must alternate, and a left frame must
// take the pair's game time and a right frame none, so both eyes of a pair see the same moment.
#include "vr_pair.h"
#include "vr_test.h"

// one game frame, as Savage Wind VR's vr_camera.c drives it: the clock first (clock_first) or a camera first, then cams
// more cameras, then the present. world: in play (out of it the clock is the game's and no camera is the head's); a
// frame in play counts as drawn for an eye only when a camera of its own was set
static float frame_clock(vr_pair *p, bool world) { return world ? vr_pair_step(p, 1.0f) : 1.0f; }

static int frame(vr_pair *p, bool clock_first, int cams, bool world, float *step) {
    int eye = -2;
    if (!world) cams = 0;
    if (clock_first) *step = frame_clock(p, world);
    for (int i = 0; i < cams; i++) {
        int e = vr_pair_camera(p);
        if (eye == -2) eye = e;
        CHECK_EQ_INT(e, eye);  // every camera of a frame gets the same eye
    }
    if (!clock_first) *step = frame_clock(p, world);
    int shown = vr_pair_present(p, world && cams > 0);
    if (world && cams) CHECK_EQ_INT(shown, eye);
    return shown;
}

static void test_order(bool clock_first) {
    VR_TEST(clock_first ? "clock first" : "camera first");
    vr_pair p = {0};
    for (int i = 0; i < 20; i++) {
        float step = -1;
        int eye = frame(&p, clock_first, 3, true, &step);
        CHECK_EQ_INT(eye, i % 2);  // left, right, left ...
        if (eye == 0) CHECK_NEAR(step, 2.0, 1e-6);  // the pair's time
        else CHECK_NEAR(step, 0.001, 1e-6);         // the same moment again
    }
    CHECK_EQ_INT(p.pairs, 10);
}

static void test_mixed_orders(void) {
    VR_TEST("orders changing");
    vr_pair p = {0};
    double game_time = 0;
    for (int i = 0; i < 200; i++) {
        float step = -1;
        int eye = frame(&p, (i * 7 + i / 3) % 3 == 0, 1 + i % 3, true, &step);
        CHECK_EQ_INT(eye, i % 2);
        game_time += step;
    }
    CHECK_EQ_INT(p.pairs, 100);
    CHECK_NEAR(game_time, 200 + 100 * 0.001, 1e-3);  // the world keeps its speed: 2 a pair (+ the right frames' bit)
}

static void test_breaks(void) {
    VR_TEST("pairs broken by menus");
    vr_pair p = {0};
    float step;
    CHECK_EQ_INT(frame(&p, true, 1, true, &step), 0);
    // a menu opens after the left frame: that frame shows the screen, and play starts again with a left frame
    CHECK_EQ_INT(frame(&p, true, 0, false, &step), -1);
    CHECK_EQ_INT(frame(&p, false, 1, true, &step), 0);
    CHECK_NEAR(step, 2.0, 1e-6);
    CHECK_EQ_INT(frame(&p, true, 1, true, &step), 1);
    CHECK_EQ_INT(p.pairs, 1);
    // a frame in play with no camera of its own (the game holds a still camera without setting it): not an eye
    CHECK_EQ_INT(frame(&p, true, 0, true, &step), -1);
    CHECK_EQ_INT(frame(&p, true, 2, true, &step), 0);
    // reset (stereo off, out of play): the next frame is a left one
    vr_pair_reset(&p);
    CHECK_EQ_INT(frame(&p, true, 1, true, &step), 0);
}

int main(void) {
    test_order(true);
    test_order(false);
    test_mixed_orders();
    test_breaks();
    return VR_TEST_END("test_vr_pair");
}
