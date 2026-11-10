// VR math: poses, the tracking space to game world mapping, and projections from OpenXR field-of-view angles.
// Header only and free of the rest of the runtime, so tests/vr/test_vr_math.c and tools/vr/fake_xr use it as is.
//
// Conventions (every conversion between them goes through here, never inline):
//   - OpenXR: right handed, +Y up, a view looks along -Z, metres.
//   - The game (sead / BOTW): right handed, +Y up, metres (1 unit = 1 m). A direction's yaw is measured from +Z towards
//     +X (yaw 0 looks along +Z, 90 degrees along +X) and its pitch up from the horizon, as runtime/hle/freecam.c has it:
//     dir = (sin yaw cos pitch, sin pitch, cos yaw cos pitch).
//   - Both turn the same way (counter-clockwise seen from above is a positive rotation about +Y), so the tracking space
//     goes into the world by one rotation about +Y and a translation: the anchor below. XR's forward (-Z) becomes the
//     anchor's game yaw.
//   - Matrices are column major (m[col * 4 + row]), as OpenXR's xr_linear.h, and multiply column vectors.
#pragma once
#include <math.h>
#include <stdbool.h>

#ifndef VR_PI
#define VR_PI 3.14159265358979323846f
#endif

typedef struct { float x, y, z; } vr_vec3;
typedef struct { float x, y, z, w; } vr_quat;  // unit quaternion, w the scalar part (XrQuaternionf's order)
typedef struct { vr_quat q; vr_vec3 p; } vr_pose;  // rotate by q, then move by p (XrPosef)

// Where the tracking space is in the game world: the game position of the tracking origin, the game yaw that XR's
// forward (-Z) looks towards, and the world units per tracked metre (1: the game's own scale).
typedef struct { vr_vec3 origin; float yaw; float scale; } vr_anchor;

// ---- vectors ----
static inline vr_vec3 vr_v3(float x, float y, float z) { vr_vec3 v = {x, y, z}; return v; }
static inline vr_vec3 vr_v3_add(vr_vec3 a, vr_vec3 b) { return vr_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vr_vec3 vr_v3_sub(vr_vec3 a, vr_vec3 b) { return vr_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vr_vec3 vr_v3_scale(vr_vec3 a, float s) { return vr_v3(a.x * s, a.y * s, a.z * s); }
static inline float vr_v3_dot(vr_vec3 a, vr_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vr_vec3 vr_v3_cross(vr_vec3 a, vr_vec3 b) {
    return vr_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float vr_v3_len(vr_vec3 a) { return sqrtf(vr_v3_dot(a, a)); }
static inline vr_vec3 vr_v3_normalize(vr_vec3 a) {
    float l = vr_v3_len(a);
    return l > 0 ? vr_v3_scale(a, 1.0f / l) : a;
}

// ---- quaternions ----
static inline vr_quat vr_quat_identity(void) { vr_quat q = {0, 0, 0, 1}; return q; }
static inline vr_quat vr_quat_mul(vr_quat a, vr_quat b) {  // a after b
    vr_quat r = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                 a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return r;
}
static inline vr_quat vr_quat_conj(vr_quat q) { vr_quat r = {-q.x, -q.y, -q.z, q.w}; return r; }
static inline vr_quat vr_quat_normalize(vr_quat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 0)) return vr_quat_identity();
    vr_quat r = {q.x / l, q.y / l, q.z / l, q.w / l};
    return r;
}
static inline vr_quat vr_quat_axis_angle(vr_vec3 axis, float angle) {
    axis = vr_v3_normalize(axis);
    float s = sinf(angle * 0.5f);
    vr_quat q = {axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f)};
    return q;
}
static inline vr_quat vr_quat_yaw(float angle) { return vr_quat_axis_angle(vr_v3(0, 1, 0), angle); }
static inline vr_vec3 vr_quat_rotate(vr_quat q, vr_vec3 v) {  // q v q*
    vr_vec3 u = vr_v3(q.x, q.y, q.z);
    vr_vec3 t = vr_v3_scale(vr_v3_cross(u, v), 2.0f);
    return vr_v3_add(vr_v3_add(v, vr_v3_scale(t, q.w)), vr_v3_cross(u, t));
}
// The rotation's heading about +Y in XR terms: 0 when its forward (-Z) stays -Z, +a after vr_quat_yaw(a). Its pitch and
// roll are left out (the forward projected on the floor); straight up or down it is 0.
static inline float vr_quat_heading(vr_quat q) {
    vr_vec3 f = vr_quat_rotate(q, vr_v3(0, 0, -1));
    if (fabsf(f.x) < 1e-6f && fabsf(f.z) < 1e-6f) return 0;
    return atan2f(-f.x, -f.z);
}

// ---- poses ----
static inline vr_pose vr_pose_identity(void) { vr_pose p = {{0, 0, 0, 1}, {0, 0, 0}}; return p; }
static inline vr_vec3 vr_pose_point(vr_pose a, vr_vec3 v) { return vr_v3_add(vr_quat_rotate(a.q, v), a.p); }
static inline vr_pose vr_pose_mul(vr_pose a, vr_pose b) {  // b in a's space -> b in a's parent space
    vr_pose r = {vr_quat_normalize(vr_quat_mul(a.q, b.q)), vr_pose_point(a, b.p)};
    return r;
}
static inline vr_pose vr_pose_inverse(vr_pose a) {
    vr_quat qi = vr_quat_conj(a.q);
    vr_pose r = {qi, vr_v3_scale(vr_quat_rotate(qi, a.p), -1.0f)};
    return r;
}

// ---- the game's directions ----
static inline vr_vec3 vr_game_dir(float yaw, float pitch) {
    float cp = cosf(pitch);
    return vr_v3(sinf(yaw) * cp, sinf(pitch), cosf(yaw) * cp);
}
static inline float vr_game_yaw_of(vr_vec3 d) { return atan2f(d.x, d.z); }
static inline float vr_game_pitch_of(vr_vec3 d) {
    float h = sqrtf(d.x * d.x + d.z * d.z);
    return atan2f(d.y, h);
}

// ---- tracking space -> game world ----
// The rotation that takes XR directions into the world: XR forward (-Z) to the anchor's yaw (yaw + pi about +Y).
static inline vr_quat vr_anchor_rotation(vr_anchor a) { return vr_quat_yaw(a.yaw + VR_PI); }
static inline vr_vec3 vr_xr_to_game_dir(vr_anchor a, vr_vec3 d) { return vr_quat_rotate(vr_anchor_rotation(a), d); }
static inline vr_vec3 vr_xr_to_game_point(vr_anchor a, vr_vec3 p) {
    return vr_v3_add(a.origin, vr_xr_to_game_dir(a, vr_v3_scale(p, a.scale)));
}
static inline vr_pose vr_xr_to_game_pose(vr_anchor a, vr_pose p) {
    vr_pose r = {vr_quat_normalize(vr_quat_mul(vr_anchor_rotation(a), p.q)), vr_xr_to_game_point(a, p.p)};
    return r;
}

// A LookAtCamera's eye / at / up (runtime/hle/freecam.c, +0x38) for a view pose in the tracking space: at is one unit
// ahead of eye.
static inline void vr_lookat_from_view(vr_anchor a, vr_pose view, float eye[3], float at[3], float up[3]) {
    vr_pose g = vr_xr_to_game_pose(a, view);
    vr_vec3 f = vr_quat_rotate(g.q, vr_v3(0, 0, -1));
    vr_vec3 u = vr_quat_rotate(g.q, vr_v3(0, 1, 0));
    eye[0] = g.p.x; eye[1] = g.p.y; eye[2] = g.p.z;
    at[0] = g.p.x + f.x; at[1] = g.p.y + f.y; at[2] = g.p.z + f.z;
    up[0] = u.x; up[1] = u.y; up[2] = u.z;
}

// Recentring: the anchor that puts the head's current tracked position at the game position `game_eye` and its
// heading at the game yaw `game_yaw` (the height of the head kept: origin.y follows from the head's tracked height).
static inline vr_anchor vr_anchor_recenter(vr_pose head, vr_vec3 game_eye, float game_yaw, float scale) {
    vr_anchor a = {vr_v3(0, 0, 0), game_yaw - vr_quat_heading(head.q), scale};
    vr_vec3 at_origin = vr_xr_to_game_point(a, head.p);  // where the head is with the origin at 0
    a.origin = vr_v3_sub(game_eye, at_origin);
    return a;
}

// ---- projection ----
// From XrFovf's four angles (radians; left and down are negative for a usual field). OpenGL clip space by default:
// x, y in -1..1 with +y up, z in -1..1. VR_PROJ_Z01 for a 0..1 depth (Vulkan / D3D), VR_PROJ_Y_DOWN for clip +y down
// (Vulkan without a flipped viewport). far <= near gives an infinite far plane. Which one the game's renderer wants is
// decided in phase 2 (docs/VR.md); the matrix is xr_linear.h's XrMatrix4x4f_CreateProjection.
enum { VR_PROJ_Z01 = 1, VR_PROJ_Y_DOWN = 2 };
static inline void vr_projection(float m[16], float left, float right, float up, float down, float nearz, float farz,
                                 int flags) {
    float tl = tanf(left), tr = tanf(right), tu = tanf(up), td = tanf(down);
    float w = tr - tl;
    float h = (flags & VR_PROJ_Y_DOWN) ? (td - tu) : (tu - td);
    float offz = (flags & VR_PROJ_Z01) ? 0.0f : nearz;
    for (int i = 0; i < 16; i++) m[i] = 0;
    m[0] = 2.0f / w;
    m[8] = (tr + tl) / w;
    m[5] = 2.0f / h;
    m[9] = (tu + td) / h;
    m[11] = -1.0f;
    if (farz <= nearz) {  // infinite far plane
        m[10] = -1.0f;
        m[14] = -(nearz + offz);
    } else {
        m[10] = -(farz + offz) / (farz - nearz);
        m[14] = -(farz * (nearz + offz)) / (farz - nearz);
    }
}

// m * (x, y, z, 1), divided by w: the normalised device coordinates of a point in view space. false behind the eye.
static inline bool vr_project_point(const float m[16], vr_vec3 p, vr_vec3 *ndc) {
    float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
    float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (!(w > 0)) return false;
    *ndc = vr_v3(x / w, y / w, z / w);
    return true;
}
