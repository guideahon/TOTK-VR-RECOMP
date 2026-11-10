// Motion combat: telling a slash or a stab from the rest of a hand's motion. A port to C of BetterVR's
// WeaponMotionAnalyser (src/hooking/weapon.h, github.com/Crementif/BotW-BetterVR, MIT: third_party/BetterVR/LICENSE),
// the swing and stab analysis by Acudofy, with its "normal" profile. Header only and free of the rest of the runtime:
// tests/vr/test_vr_melee.c drives it with synthetic hand motions.
//
// Each sample is the hand's pose and velocity (OpenXR's grip pose in LOCAL space, its linear and angular velocity in
// the same space) and the head's position. The hand's own axes: -Z out of the fist along the blade, X to its right.
//   - stab: the hand moving along its -Z, straight, without turning, fast or accelerating; once locked, it becomes an
//     attack after travelling stab_travel (scaled by the arm's length, calibrated as it goes);
//   - slash: the hand turning fast about its X / Y (not twisting about Z), by angular acceleration or speed; once
//     locked, it becomes an attack after turning slash_angle.
// An attack lasts while the motion stays good (bad samples accumulate, good ones heal); after one ends a 250 ms
// refractory period keeps the return to guard from firing again. swing_power (0..1) follows the attack's peak speed.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "vr_math.h"

enum { VR_MELEE_NONE, VR_MELEE_SLASH, VR_MELEE_STAB };

typedef struct {
    float stab_speed, stab_acc, stab_straight, stab_turn, stab_travel, stab_acc_tc, stab_velocity;
    float slash_speed, slash_acc, slash_velocity, slash_drift, slash_angle;
    float max_bad, good_grace, min_good_slash, min_good_stab, smoothing_tc, drift_min_velocity, reference_arm;
} vr_melee_profile;

// BetterVR's presets: normal (the default there) and easy
static inline vr_melee_profile vr_melee_profile_for(bool easy) {
    vr_melee_profile p = {
        .stab_speed = 0.05f, .stab_acc = 4.0f, .stab_straight = 0.5f /* cos 60 */, .stab_turn = 4.5f,
        .stab_travel = 0.12f, .stab_acc_tc = 0.050f, .stab_velocity = 1.0f,
        .slash_speed = 1.0f, .slash_acc = 14.0f, .slash_velocity = 5.0f, .slash_drift = 10.0f,
        .slash_angle = VR_PI / 6,
        .max_bad = 0.033f, .good_grace = 0.080f, .min_good_slash = 0.025f, .min_good_stab = 0.025f,
        .smoothing_tc = 0.030f, .drift_min_velocity = 0.5f, .reference_arm = 0.55f,
    };
    if (!easy) {
        p.stab_straight = 0.70710678f;  // (cos 45)
        p.slash_acc = 20.0f;
        p.slash_velocity = 7.0f;
        p.slash_speed = 1.5f;
        p.slash_angle = VR_PI / 5;
        p.max_bad = 0.022f;
        p.good_grace = 0.040f;
        p.min_good_slash = 0.040f;
        p.min_good_stab = 0.030f;
        p.smoothing_tc = 0.020f;
    }
    return p;
}

typedef struct {
    vr_melee_profile prof;
    double prev_time;  // (s; 0: no sample yet)
    vr_vec3 prev_local_lin, prev_flat_ang, prev_ang;
    vr_vec3 local_lin, local_ang, local_acc;
    float flat_ang_acc, drift, hand_speed;
    vr_vec3 sm_acc, sm_acc_slow;
    float sm_flat_ang_acc, sm_drift, sm_dt;
    int locked;  // VR_MELEE_*
    bool active;
    float good_slash, good_stab, bad;
    double last_good_slash, last_good_stab, lock_time, refractory_until;
    vr_vec3 locked_pos, locked_dir;
    float power, peak;
    float arm;     // the arm's length, calibrated
    uint32_t attacks;  // attacks begun (active turned on)
} vr_melee;

static inline void vr_melee_init(vr_melee *m, bool easy) {
    memset(m, 0, sizeof *m);
    m->prof = vr_melee_profile_for(easy);
    m->arm = 0.55f;
}

static inline float vr_melee_lerp(float a, float b, float k) { return a + (b - a) * k; }
static inline vr_vec3 vr_melee_mix(vr_vec3 a, vr_vec3 b, float k) {
    return vr_v3(vr_melee_lerp(a.x, b.x, k), vr_melee_lerp(a.y, b.y, k), vr_melee_lerp(a.z, b.z, k));
}
static inline float vr_melee_xy(vr_vec3 v) { return sqrtf(v.x * v.x + v.y * v.y); }

static inline void vr_melee_detect(vr_melee *m, vr_vec3 pos, vr_quat rot, double t, float dt) {
    const vr_melee_profile *p = &m->prof;
    if (m->locked != VR_MELEE_NONE) return;
    bool refractory = t < m->refractory_until;
    bool stab_gate = false;
    if (vr_v3_dot(m->local_lin, m->local_lin) >= 1e-8f && !refractory) {
        vr_vec3 d = vr_v3_normalize(m->local_lin);
        bool straight = fabsf(d.z) > p->stab_straight;
        bool steady = fabsf(m->local_ang.x) < p->stab_turn && fabsf(m->local_ang.y) < p->stab_turn;
        bool impulse = -m->sm_acc_slow.z > p->stab_acc || -m->local_lin.z > p->stab_velocity;
        stab_gate = straight && steady && impulse;
        if (stab_gate) {
            if (m->good_stab == 0) m->locked_pos = pos;  // (the whole thrust counts toward the travel)
            m->good_stab += dt;
            m->last_good_stab = t;
            if (m->good_stab > p->min_good_stab) {
                m->locked = VR_MELEE_STAB;
                m->peak = 0;
                m->lock_time = t;
            }
        } else if (t - m->last_good_stab > p->good_grace) {
            m->good_stab = 0;
        }
    }
    if (!stab_gate && m->locked != VR_MELEE_STAB) {
        bool by_acc = !refractory && m->sm_flat_ang_acc > p->slash_acc;
        bool by_vel = vr_melee_xy(m->local_ang) > p->slash_velocity;
        if (by_acc || by_vel) {
            if (m->good_slash == 0) m->locked_dir = vr_quat_rotate(rot, vr_v3(0, 0, 1));
            m->good_slash += dt;
            m->last_good_slash = t;
            if (m->good_slash > p->min_good_slash) {
                m->locked = VR_MELEE_SLASH;
                m->peak = 0;
                m->lock_time = t;
            }
        } else if (t - m->last_good_slash > p->good_grace) {
            m->good_slash = 0;
        }
    }
}

static inline void vr_melee_steadiness(vr_melee *m, double t, float dt) {
    const vr_melee_profile *p = &m->prof;
    bool bad = false;
    if (m->locked == VR_MELEE_NONE) return;
    if (m->locked == VR_MELEE_STAB) {
        if (vr_v3_dot(m->local_lin, m->local_lin) < 1e-8f) bad = true;
        else {
            vr_vec3 d = vr_v3_normalize(m->local_lin);
            bad = fabsf(d.z) < p->stab_straight || -m->local_lin.z < p->stab_speed ||
                  vr_melee_xy(m->local_ang) > p->stab_turn;
        }
    } else {
        bad = m->sm_drift > p->slash_drift || vr_melee_xy(m->local_ang) < p->slash_speed;
    }
    // (a lock does not cancel on the samples that set it)
    double grace = fmax(0.033, 2.5 * m->sm_dt);
    if (t - m->lock_time < grace) bad = false;
    m->bad = bad ? m->bad + dt : fmaxf(0, m->bad - dt);
    float max_bad = fmaxf(p->max_bad, 2.5f * m->sm_dt);
    if (m->bad >= max_bad) {
        m->bad = 0;
        if (m->active) m->refractory_until = t + 0.25;
        if (m->locked == VR_MELEE_SLASH) m->good_slash = 0, m->last_good_slash = 0;
        else m->good_stab = 0, m->last_good_stab = 0;
        m->locked = VR_MELEE_NONE;
    }
}

static inline void vr_melee_activity(vr_melee *m, vr_vec3 pos, vr_quat rot) {
    if (m->locked == VR_MELEE_NONE) {
        m->active = false;
        return;
    }
    if (m->active) return;
    float arm_scale = m->arm / m->prof.reference_arm;
    bool begin = false;
    if (m->locked == VR_MELEE_STAB) {
        begin = vr_v3_len(vr_v3_sub(pos, m->locked_pos)) > m->prof.stab_travel * arm_scale;
    } else {
        vr_vec3 z = vr_quat_rotate(rot, vr_v3(0, 0, 1));
        float c = vr_v3_dot(z, m->locked_dir);
        c = c > 1 ? 1 : c < -1 ? -1 : c;
        begin = acosf(c) > m->prof.slash_angle;
    }
    if (begin) {
        m->active = true;
        m->attacks++;
    }
}

static inline void vr_melee_power(vr_melee *m) {
    if (!m->active) {
        m->power = 0;
        return;
    }
    if (m->locked == VR_MELEE_STAB) {
        m->peak = fmaxf(m->peak, fabsf(m->local_lin.z));
        float straight = vr_v3_dot(m->local_lin, m->local_lin) >= 1e-8f ? fabsf(vr_v3_normalize(m->local_lin).z) : 0;
        float ns = (straight - m->prof.stab_straight) / (1 - m->prof.stab_straight);
        ns = ns < 0 ? 0 : ns > 1 ? 1 : ns;
        float k = m->peak / 2.0f;  // (2 m/s: a strong stab)
        m->power = (k > 1 ? 1 : k) * vr_melee_lerp(0.70f, 1.0f, ns);
    } else {
        m->peak = fmaxf(m->peak, vr_melee_xy(m->local_ang));
        float k = m->peak / 10.0f;  // (10 rad/s: a strong slash)
        m->power = k > 1 ? 1 : k;
    }
}

// The shield: the left hand held up in front of the face, as one raises a shield, holds the game's guard (ZL). In the
// head's own frame (yaw only: looking down at the hand does not lower it): ahead of the eyes, about at their height,
// within an arm's reach. raised: the last answer (a little more is asked to raise it than to keep it: no flicker).
static inline bool vr_shield_raised(bool raised, vr_pose head, vr_vec3 hand) {
    float yaw = vr_quat_heading(head.q);
    vr_vec3 d = vr_quat_rotate(vr_quat_yaw(-yaw), vr_v3_sub(hand, head.p));  // (-Z ahead, +Y up)
    float ahead = -d.z, up = d.y, side = fabsf(d.x);
    float k = raised ? 0.05f : 0;  // (hysteresis, metres)
    return ahead > 0.20f - k && ahead < 0.70f + k && up > -0.30f - k && up < 0.25f + k && side < 0.40f + k;
}

// One sample: the hand (pose, velocity, angular velocity, all in LOCAL space), the head's position, the time (s).
// tracked: the runtime tracks the hand (an untracked sample only keeps the derivatives going).
static inline void vr_melee_update(vr_melee *m, vr_pose hand, vr_vec3 lin, vr_vec3 ang, vr_vec3 head, double t,
                                   bool tracked) {
    float dt = m->prev_time > 0 ? (float)(t - m->prev_time) : 0;
    if (dt < 0) dt = 0;
    vr_quat inv = vr_quat_conj(hand.q);
    m->local_lin = vr_quat_rotate(inv, lin);
    m->local_ang = vr_quat_rotate(inv, ang);
    m->local_acc = dt > 0 ? vr_v3_scale(vr_v3_sub(m->local_lin, m->prev_local_lin), 1 / dt) : vr_v3(0, 0, 0);
    vr_vec3 flat = vr_v3(m->local_ang.x, m->local_ang.y, 0);
    m->flat_ang_acc = dt > 0 ? vr_v3_len(vr_v3_sub(flat, m->prev_flat_ang)) / dt : 0;
    m->prev_flat_ang = flat;
    m->hand_speed = vr_v3_len(lin);
    if (tracked) {
        // angular drift: how fast the turning axis itself turns (a slash keeps its axis)
        float a = vr_v3_len(ang), b = vr_v3_len(m->prev_ang);
        m->drift = 0;
        if (a >= m->prof.drift_min_velocity && b >= m->prof.drift_min_velocity && dt > 0) {
            float c = vr_v3_dot(vr_v3_normalize(ang), vr_v3_normalize(m->prev_ang));
            c = c > 1 ? 1 : c < -1 ? -1 : c;
            m->drift = acosf(c) / dt;
        }
        float k = dt > 0 ? 1 - expf(-dt / m->prof.smoothing_tc) : 0;
        m->sm_acc = vr_melee_mix(m->sm_acc, m->local_acc, k);
        m->sm_flat_ang_acc = vr_melee_lerp(m->sm_flat_ang_acc, m->flat_ang_acc, k);
        m->sm_drift = vr_melee_lerp(m->sm_drift, m->drift, k);
        float ks = dt > 0 ? 1 - expf(-dt / m->prof.stab_acc_tc) : 0;
        m->sm_acc_slow = vr_melee_mix(m->sm_acc_slow, m->local_acc, ks);
        if (dt > 0) m->sm_dt = m->sm_dt == 0 ? dt : vr_melee_lerp(m->sm_dt, dt, 1 - expf(-dt / 0.5f));
        float reach = vr_v3_len(vr_v3_sub(hand.p, head));  // (the arm's length: toward the longest reach, slowly back)
        if (reach <= 1.2f) m->arm = vr_melee_lerp(m->arm, reach, reach > m->arm ? 0.05f : 0.002f);
        float dta = dt < 0.05f ? dt : 0.05f;  // (a pause or a long frame does not pour seconds into the counts)
        vr_melee_detect(m, hand.p, hand.q, t, dta);
        vr_melee_steadiness(m, t, dta);
        vr_melee_activity(m, hand.p, hand.q);
        vr_melee_power(m);
    }
    m->prev_ang = ang;
    m->prev_local_lin = m->local_lin;
    m->prev_time = t;
}
