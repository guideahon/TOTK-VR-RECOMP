// runtime/vr/vr_math.h: quaternions, poses, the tracking space to game world mapping and the projection.
// Every expected value here comes from the conventions written at the top of vr_math.h, worked out by hand, not from
// the code under test.
#include "vr_math.h"
#include "vr_test.h"

#define EPS 1e-4
#define DEG (VR_PI / 180.0f)

static void check_v3(vr_vec3 a, float x, float y, float z, double eps) {
    CHECK_NEAR(a.x, x, eps);
    CHECK_NEAR(a.y, y, eps);
    CHECK_NEAR(a.z, z, eps);
}

static float wrap(float a) {  // to -pi..pi
    while (a > VR_PI) a -= 2 * VR_PI;
    while (a < -VR_PI) a += 2 * VR_PI;
    return a;
}

static void test_quat(void) {
    VR_TEST("quat");
    // a positive yaw turns counter-clockwise seen from above: -Z (XR forward) to -X after 90 degrees
    check_v3(vr_quat_rotate(vr_quat_yaw(90 * DEG), vr_v3(0, 0, -1)), -1, 0, 0, EPS);
    check_v3(vr_quat_rotate(vr_quat_yaw(90 * DEG), vr_v3(1, 0, 0)), 0, 0, -1, EPS);
    // pitch up: a positive rotation about +X lifts -Z towards +Y
    vr_vec3 up30 = vr_quat_rotate(vr_quat_axis_angle(vr_v3(1, 0, 0), 30 * DEG), vr_v3(0, 0, -1));
    check_v3(up30, 0, sinf(30 * DEG), -cosf(30 * DEG), EPS);
    // composition: a after b
    vr_quat ab = vr_quat_mul(vr_quat_yaw(30 * DEG), vr_quat_yaw(60 * DEG));
    check_v3(vr_quat_rotate(ab, vr_v3(0, 0, -1)), -1, 0, 0, EPS);
    // conjugate undoes
    vr_quat q = vr_quat_normalize((vr_quat){0.3f, -0.5f, 0.1f, 0.8f});
    check_v3(vr_quat_rotate(vr_quat_mul(vr_quat_conj(q), q), vr_v3(0.2f, 0.4f, -0.7f)), 0.2f, 0.4f, -0.7f, EPS);
    // normalize keeps unit length, a zero quaternion becomes the identity
    vr_quat n = vr_quat_normalize((vr_quat){2, 0, 0, 2});
    CHECK_NEAR(n.x * n.x + n.y * n.y + n.z * n.z + n.w * n.w, 1, EPS);
    vr_quat z = vr_quat_normalize((vr_quat){0, 0, 0, 0});
    CHECK_NEAR(z.w, 1, 0);
    // rotations keep lengths
    CHECK_NEAR(vr_v3_len(vr_quat_rotate(q, vr_v3(3, 4, 12))), 13, 1e-3);
}

static void test_heading(void) {
    VR_TEST("heading");
    const float angles[] = {0, 10, 45, 90, 135, 170, -10, -90, -170};
    for (unsigned i = 0; i < sizeof angles / sizeof *angles; i++)
        CHECK_NEAR(vr_quat_heading(vr_quat_yaw(angles[i] * DEG)), angles[i] * DEG, EPS);
    // pitch and roll leave the heading alone
    vr_quat q = vr_quat_mul(vr_quat_yaw(40 * DEG),
                            vr_quat_mul(vr_quat_axis_angle(vr_v3(1, 0, 0), 35 * DEG),
                                        vr_quat_axis_angle(vr_v3(0, 0, 1), 20 * DEG)));
    CHECK_NEAR(vr_quat_heading(q), 40 * DEG, 1e-3);
    // straight down: no heading, no NaN
    float h = vr_quat_heading(vr_quat_axis_angle(vr_v3(1, 0, 0), -90 * DEG));
    CHECK(h == h);
}

static void test_pose(void) {
    VR_TEST("pose");
    vr_pose a = {vr_quat_normalize((vr_quat){0.1f, 0.7f, -0.2f, 0.6f}), {1, 2, 3}};
    vr_pose b = {vr_quat_yaw(25 * DEG), {-0.5f, 0.25f, 4}};
    // a * a^-1 = identity
    vr_pose id = vr_pose_mul(a, vr_pose_inverse(a));
    check_v3(id.p, 0, 0, 0, EPS);
    CHECK_NEAR(fabsf(id.q.w), 1, EPS);
    // (a * b) applied to a point = a(b(point))
    vr_vec3 v = vr_v3(0.3f, -1.1f, 2.0f);
    vr_vec3 lhs = vr_pose_point(vr_pose_mul(a, b), v);
    vr_vec3 rhs = vr_pose_point(a, vr_pose_point(b, v));
    check_v3(lhs, rhs.x, rhs.y, rhs.z, EPS);
    // the inverse takes a point back
    vr_vec3 back = vr_pose_point(vr_pose_inverse(a), vr_pose_point(a, v));
    check_v3(back, v.x, v.y, v.z, EPS);
}

static void test_game_dirs(void) {
    VR_TEST("game directions");
    // freecam.c: yaw 0 looks along +Z, 90 along +X, pitch up from the horizon
    check_v3(vr_game_dir(0, 0), 0, 0, 1, EPS);
    check_v3(vr_game_dir(90 * DEG, 0), 1, 0, 0, EPS);
    check_v3(vr_game_dir(0, 90 * DEG), 0, 1, 0, EPS);
    for (int yaw = -170; yaw <= 180; yaw += 35)
        for (int pitch = -80; pitch <= 80; pitch += 40) {
            vr_vec3 d = vr_game_dir(yaw * DEG, pitch * DEG);
            CHECK_NEAR(wrap(vr_game_yaw_of(d) - yaw * DEG), 0, 1e-3);
            CHECK_NEAR(vr_game_pitch_of(d), pitch * DEG, 1e-3);
        }
}

static void test_anchor(void) {
    VR_TEST("anchor");
    // XR forward becomes the anchor's game yaw, whatever it is
    for (int yaw = -150; yaw <= 180; yaw += 30) {
        vr_anchor a = {{0, 0, 0}, yaw * DEG, 1};
        vr_vec3 f = vr_xr_to_game_dir(a, vr_v3(0, 0, -1));
        CHECK_NEAR(wrap(vr_game_yaw_of(f) - yaw * DEG), 0, 1e-3);
        CHECK_NEAR(vr_game_pitch_of(f), 0, EPS);
    }
    // up stays up
    vr_anchor a = {{10, 20, 30}, 1.234f, 1};
    check_v3(vr_xr_to_game_dir(a, vr_v3(0, 1, 0)), 0, 1, 0, EPS);
    // facing +Z in the game (yaw 0), the head's right (XR +X) is the game's -X (right handed: right = forward x up)
    vr_anchor a0 = {{0, 0, 0}, 0, 1};
    check_v3(vr_xr_to_game_dir(a0, vr_v3(1, 0, 0)), -1, 0, 0, EPS);
    // facing +X (yaw 90) the right is +Z ((1,0,0) x (0,1,0))
    vr_anchor a90 = {{0, 0, 0}, 90 * DEG, 1};
    check_v3(vr_xr_to_game_dir(a90, vr_v3(1, 0, 0)), 0, 0, 1, EPS);
    // points: origin and scale
    vr_anchor as = {{100, 5, -50}, 0, 2};
    check_v3(vr_xr_to_game_point(as, vr_v3(0, 1.5f, 0)), 100, 8, -50, EPS);
    check_v3(vr_xr_to_game_point(as, vr_v3(0, 0, -1)), 100, 5, -48, EPS);  // 1 m forward = 2 units along +Z
}

static void test_lookat(void) {
    VR_TEST("lookat");
    vr_anchor a = {{0, 0, 0}, 0, 1};
    float eye[3], at[3], up[3];
    // a head 1.7 m up, looking 30 degrees up: the game sees pitch 30 along +Z
    vr_pose head = {vr_quat_axis_angle(vr_v3(1, 0, 0), 30 * DEG), {0, 1.7f, 0}};
    vr_lookat_from_view(a, head, eye, at, up);
    check_v3(vr_v3(eye[0], eye[1], eye[2]), 0, 1.7f, 0, EPS);
    vr_vec3 f = vr_v3(at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]);
    CHECK_NEAR(vr_v3_len(f), 1, EPS);
    CHECK_NEAR(vr_game_pitch_of(f), 30 * DEG, 1e-3);
    CHECK_NEAR(vr_game_yaw_of(f), 0, 1e-3);
    vr_vec3 u = vr_v3(up[0], up[1], up[2]);
    CHECK_NEAR(vr_v3_dot(u, f), 0, EPS);
    CHECK(u.y > 0.8f);
    // rolled 20 degrees: up tilts, forward does not move
    vr_pose rolled = {vr_quat_axis_angle(vr_v3(0, 0, 1), 20 * DEG), {0, 1.7f, 0}};
    vr_lookat_from_view(a, rolled, eye, at, up);
    CHECK_NEAR(at[2] - eye[2], 1, EPS);
    CHECK_NEAR(up[1], cosf(20 * DEG), EPS);
}

static void test_recenter(void) {
    VR_TEST("recenter");
    vr_pose head = {vr_quat_mul(vr_quat_yaw(40 * DEG), vr_quat_axis_angle(vr_v3(1, 0, 0), -15 * DEG)),
                    {0.3f, 1.7f, -0.2f}};
    vr_vec3 game_eye = vr_v3(100, 50, -20);
    vr_anchor a = vr_anchor_recenter(head, game_eye, 1.0f, 1);
    vr_pose g = vr_xr_to_game_pose(a, head);
    check_v3(g.p, 100, 50, -20, 1e-3);
    vr_vec3 f = vr_quat_rotate(g.q, vr_v3(0, 0, -1));
    CHECK_NEAR(wrap(vr_game_yaw_of(f) - 1.0f), 0, 1e-3);
    CHECK_NEAR(vr_game_pitch_of(f), -15 * DEG, 1e-3);  // the head's pitch is kept
    // a step of 1 m to the head's right after recentring lands 1 m to the right of the game's view
    vr_vec3 right_xr = vr_quat_rotate(vr_quat_yaw(40 * DEG), vr_v3(1, 0, 0));
    vr_vec3 moved = vr_xr_to_game_point(a, vr_v3_add(head.p, right_xr));
    vr_vec3 game_right = vr_v3_cross(vr_game_dir(1.0f, 0), vr_v3(0, 1, 0));
    vr_vec3 d = vr_v3_sub(moved, game_eye);
    check_v3(d, game_right.x, game_right.y, game_right.z, 1e-3);
}

static void test_projection_edges(int flags) {
    // a Quest-like asymmetric field (left eye): every frustum edge lands on its clip-space edge
    const float l = -0.942f, r = 0.698f, u = 0.768f, dn = -0.890f, n = 0.1f, f = 1000.0f;
    float m[16];
    vr_projection(m, l, r, u, dn, n, f, flags);
    float ytop = (flags & VR_PROJ_Y_DOWN) ? -1.0f : 1.0f;
    float znear = (flags & VR_PROJ_Z01) ? 0.0f : -1.0f;
    vr_vec3 ndc;
    const float depth = 3.0f;
    CHECK(vr_project_point(m, vr_v3(tanf(l) * depth, 0, -depth), &ndc));
    CHECK_NEAR(ndc.x, -1, EPS);
    CHECK(vr_project_point(m, vr_v3(tanf(r) * depth, 0, -depth), &ndc));
    CHECK_NEAR(ndc.x, 1, EPS);
    CHECK(vr_project_point(m, vr_v3(0, tanf(u) * depth, -depth), &ndc));
    CHECK_NEAR(ndc.y, ytop, EPS);
    CHECK(vr_project_point(m, vr_v3(0, tanf(dn) * depth, -depth), &ndc));
    CHECK_NEAR(ndc.y, -ytop, EPS);
    CHECK(vr_project_point(m, vr_v3(0, 0, -n), &ndc));
    CHECK_NEAR(ndc.z, znear, 1e-3);
    CHECK(vr_project_point(m, vr_v3(0, 0, -f), &ndc));
    CHECK_NEAR(ndc.z, 1, 1e-3);
    // depth grows with distance
    vr_vec3 a, b;
    CHECK(vr_project_point(m, vr_v3(0, 0, -2), &a) && vr_project_point(m, vr_v3(0, 0, -20), &b));
    CHECK(b.z > a.z);
    // behind the eye
    CHECK(!vr_project_point(m, vr_v3(0, 0, 1), &ndc));
}

static void test_projection(void) {
    VR_TEST("projection gl");
    test_projection_edges(0);
    VR_TEST("projection z01");
    test_projection_edges(VR_PROJ_Z01);
    VR_TEST("projection z01 y-down");
    test_projection_edges(VR_PROJ_Z01 | VR_PROJ_Y_DOWN);
    VR_TEST("projection symmetric");
    float m[16];
    vr_projection(m, -45 * DEG, 45 * DEG, 45 * DEG, -45 * DEG, 0.1f, 100, 0);
    CHECK_NEAR(m[0], 1, EPS);
    CHECK_NEAR(m[5], 1, EPS);
    CHECK_NEAR(m[8], 0, EPS);  // a symmetric field has no off-centre term
    CHECK_NEAR(m[9], 0, EPS);
    VR_TEST("projection infinite");
    vr_projection(m, -45 * DEG, 45 * DEG, 45 * DEG, -45 * DEG, 0.1f, 0, VR_PROJ_Z01);
    vr_vec3 ndc;
    CHECK(vr_project_point(m, vr_v3(0, 0, -0.1f), &ndc));
    CHECK_NEAR(ndc.z, 0, 1e-3);
    CHECK(vr_project_point(m, vr_v3(0, 0, -1e6f), &ndc));
    CHECK(ndc.z > 0.999f && ndc.z <= 1.0f);
}

int main(void) {
    test_quat();
    test_heading();
    test_pose();
    test_game_dirs();
    test_anchor();
    test_lookat();
    test_recenter();
    test_projection();
    return VR_TEST_END("test_vr_math");
}
