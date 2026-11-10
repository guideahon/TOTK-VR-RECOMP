// runtime/vr/vr_melee.h: slashes and stabs told apart from the rest of a hand's motion, with synthetic motions sampled
// at 90 Hz as a headset gives them.
#include "vr_melee.h"
#include "vr_test.h"

#define HZ 90.0
#define DEG (VR_PI / 180.0f)

static const vr_vec3 head = {0, 1.6f, 0};

// a hand held still in front of the chest, the blade pointing forward (-Z)
static vr_pose guard(void) {
    vr_pose p = {vr_quat_identity(), {0.25f, 1.2f, -0.35f}};
    return p;
}

// hold still for s seconds
static double hold(vr_melee *m, vr_pose p, double t, double s) {
    for (double end = t + s; t < end; t += 1 / HZ) vr_melee_update(m, p, vr_v3(0, 0, 0), vr_v3(0, 0, 0), head, t, true);
    return t;
}

// a speed that eases in over ramp seconds (a hand speeds up, it does not jump to a speed)
static float eased(float top, double since, double ramp) { return ramp > 0 && since < ramp ? top * (float)(since / ramp) : top; }

// turn the hand about its own axis at rate (rad/s, reached after ramp s) for s seconds
static double turn_r(vr_melee *m, vr_pose *p, vr_vec3 local_axis, float top, double ramp, double t, double s,
                     int *peak_type) {
    for (double t0 = t, end = t + s; t < end; t += 1 / HZ) {
        float rate = eased(top, t - t0, ramp);
        vr_vec3 axis = vr_quat_rotate(p->q, local_axis);  // (angular velocity in LOCAL space)
        p->q = vr_quat_normalize(vr_quat_mul(vr_quat_axis_angle(axis, rate / (float)HZ), p->q));
        vr_melee_update(m, *p, vr_v3(0, 0, 0), vr_v3_scale(axis, rate), head, t, true);
        if (m->active && peak_type) *peak_type = m->locked;
    }
    return t;
}
static double turn(vr_melee *m, vr_pose *p, vr_vec3 local_axis, float rate, double t, double s, int *peak_type) {
    return turn_r(m, p, local_axis, rate, 0, t, s, peak_type);
}

// move the hand along its own direction (local) at speed (m/s, reached after ramp s) for s seconds
static double thrust_r(vr_melee *m, vr_pose *p, vr_vec3 local_dir, float top, double ramp, double t, double s,
                       int *peak_type) {
    for (double t0 = t, end = t + s; t < end; t += 1 / HZ) {
        float speed = eased(top, t - t0, ramp);
        vr_vec3 v = vr_v3_scale(vr_quat_rotate(p->q, local_dir), speed);
        p->p = vr_v3_add(p->p, vr_v3_scale(v, (float)(1 / HZ)));
        vr_melee_update(m, *p, v, vr_v3(0, 0, 0), head, t, true);
        if (m->active && peak_type) *peak_type = m->locked;
    }
    return t;
}
static double thrust(vr_melee *m, vr_pose *p, vr_vec3 local_dir, float speed, double t, double s, int *peak_type) {
    return thrust_r(m, p, local_dir, speed, 0, t, s, peak_type);
}

static void test_still(void) {
    VR_TEST("still and gentle");
    vr_melee m;
    vr_melee_init(&m, false);
    vr_pose p = guard();
    double t = hold(&m, p, 1, 2);
    CHECK_EQ_INT(m.attacks, 0);
    // a slow turn (a look at the sword) and a slow reach, easing in as hands do, are not attacks
    t = turn_r(&m, &p, vr_v3(1, 0, 0), 1.2f, 0.25, t, 0.8, NULL);
    t = hold(&m, p, t, 0.5);
    t = thrust_r(&m, &p, vr_v3(0, 0, -1), 0.4f, 0.25, t, 0.5, NULL);
    t = hold(&m, p, t, 0.5);
    CHECK_EQ_INT(m.attacks, 0);
    // a fast twist about the blade (turning a key) is not a slash
    t = hold(&m, p, t, 0.5);
    t = turn(&m, &p, vr_v3(0, 0, 1), 12.0f, t, 0.3, NULL);
    CHECK_EQ_INT(m.attacks, 0);
}

static void test_slash(void) {
    VR_TEST("slash");
    vr_melee m;
    vr_melee_init(&m, false);
    vr_pose p = guard();
    double t = hold(&m, p, 1, 1);
    int type = VR_MELEE_NONE;
    // a fast downward cut: 12 rad/s about the hand's X for 0.2 s (about 140 degrees)
    t = turn(&m, &p, vr_v3(1, 0, 0), 12.0f, t, 0.2, &type);
    CHECK_EQ_INT(m.attacks, 1);
    CHECK_EQ_INT(type, VR_MELEE_SLASH);
    CHECK(m.power > 0.8f);
    // the hand stops: the attack ends; the slow return to guard does not fire again
    t = hold(&m, p, t, 0.2);
    CHECK(!m.active);
    t = turn(&m, &p, vr_v3(1, 0, 0), -4.0f, t, 0.5, NULL);
    t = hold(&m, p, t, 0.5);
    CHECK_EQ_INT(m.attacks, 1);
    // a sideways cut (about Y) is a slash too
    t = turn(&m, &p, vr_v3(0, 1, 0), 11.0f, t, 0.2, &type);
    CHECK_EQ_INT(m.attacks, 2);
}

static void test_stab(void) {
    VR_TEST("stab");
    vr_melee m;
    vr_melee_init(&m, false);
    vr_pose p = guard();
    double t = hold(&m, p, 1, 1);
    int type = VR_MELEE_NONE;
    // a thrust along the blade: 2.5 m/s for 0.15 s (37 cm)
    t = thrust(&m, &p, vr_v3(0, 0, -1), 2.5f, t, 0.15, &type);
    CHECK_EQ_INT(m.attacks, 1);
    CHECK_EQ_INT(type, VR_MELEE_STAB);
    CHECK(m.power > 0.8f);
    t = hold(&m, p, t, 0.3);
    CHECK(!m.active);
    // a sideways push (along X) at the same speed is no stab
    vr_melee_init(&m, false);
    p = guard();
    t = hold(&m, p, t, 1);
    t = thrust(&m, &p, vr_v3(1, 0, 0), 2.5f, t, 0.15, NULL);
    CHECK_EQ_INT(m.attacks, 0);
    // a short jab (5 cm) locks but does not travel far enough to count
    vr_melee_init(&m, false);
    p = guard();
    t = hold(&m, p, t, 1);
    t = thrust(&m, &p, vr_v3(0, 0, -1), 1.5f, t, 0.035, NULL);
    t = hold(&m, p, t, 0.3);
    CHECK_EQ_INT(m.attacks, 0);
}

static void test_untracked(void) {
    VR_TEST("tracking lost");
    vr_melee m;
    vr_melee_init(&m, false);
    vr_pose p = guard();
    double t = hold(&m, p, 1, 1);
    // garbage while untracked: never an attack
    for (int i = 0; i < 30; i++, t += 1 / HZ) {
        vr_pose junk = {vr_quat_axis_angle(vr_v3(1, 0, 0), i * 0.7f), {i * 0.3f, 0, 0}};
        vr_melee_update(&m, junk, vr_v3(30, 0, 0), vr_v3(40, 0, 0), head, t, false);
    }
    CHECK_EQ_INT(m.attacks, 0);
}

static void test_easy(void) {
    VR_TEST("easy profile");
    // a slash too slow for the normal profile counts in the easy one: 6 rad/s (over easy's 5, under normal's 7),
    // reached gently (12 rad/s^2: under both profiles' acceleration gates)
    for (int easy = 0; easy < 2; easy++) {
        vr_melee m;
        vr_melee_init(&m, easy);
        vr_pose p = guard();
        double t = hold(&m, p, 1, 1);
        t = turn_r(&m, &p, vr_v3(1, 0, 0), 6.0f, 0.5, t, 0.8, NULL);
        if (easy) CHECK_EQ_INT(m.attacks, 1);
        else CHECK_EQ_INT(m.attacks, 0);
    }
}

static void test_shield(void) {
    VR_TEST("shield");
    vr_pose h = {vr_quat_identity(), {0, 1.6f, 0}};
    // at the side, at the hip: down
    CHECK(!vr_shield_raised(false, h, vr_v3(-0.25f, 1.0f, -0.1f)));
    // up in front of the face: raised
    CHECK(vr_shield_raised(false, h, vr_v3(-0.1f, 1.55f, -0.35f)));
    // just past the edge to raise it, but inside the edge to keep it (hysteresis)
    CHECK(!vr_shield_raised(false, h, vr_v3(-0.1f, 1.55f, -0.18f)));
    CHECK(vr_shield_raised(true, h, vr_v3(-0.1f, 1.55f, -0.18f)));
    // the head turned 90 degrees left: "ahead" turns with it
    vr_pose left = {vr_quat_yaw(90 * DEG), {0, 1.6f, 0}};
    CHECK(vr_shield_raised(false, left, vr_v3(-0.35f, 1.55f, 0.1f)));
    CHECK(!vr_shield_raised(false, left, vr_v3(-0.1f, 1.55f, -0.35f)));
    // looking down at the hand does not lower the shield (only the heading counts)
    vr_pose down = {vr_quat_axis_angle(vr_v3(1, 0, 0), -40 * DEG), {0, 1.6f, 0}};
    CHECK(vr_shield_raised(false, down, vr_v3(-0.1f, 1.45f, -0.35f)));
}

int main(void) {
    test_shield();
    test_still();
    test_slash();
    test_stab();
    test_untracked();
    test_easy();
    return VR_TEST_END("test_vr_melee");
}
