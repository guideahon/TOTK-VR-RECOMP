// VR mode, head tracking (docs/vr.md, phase 2), ported from Savage Wind VR (runtime/vr/vr_camera.c there).
//
// Two hooks of the `vr` patch set (recomp/patches-v121.txt; on with TOTK_VR=1):
//
//   - the view's copy of its camera (1.2.1 main+0xbb7f9c copies a sead::LookAtCamera, laid out as in BotW: matrix
//     +0x8, eye +0x38, at +0x44, up +0x50, into the renderer's view at +0x78): before the copy the camera takes the head
//     and its view matrix is made again (found by scanning guest memory for the view matrix near the player and
//     watching its writers: TOTK_VR_SCAN, TOTK_VR_WATCH below).
//   - the perspective projection's matrix (main+0xc76898, ldp s1, s2, [x0, #0xac]: tan(fovy / 2) and the aspect):
//     the host call loads them, and in VR gives the headset's field instead for the world's camera.
//
// The world's view cameras then take the head's pose on top of the game's own camera, as in Savage Wind: the game's
// camera keeps its place and heading, the head adds its turn and its movement since the last recentring and gives the
// pitch and roll itself.
//
// TOTK_VR_LOG_CAMERAS=<n> logs the first n calls of each hook (camera, caller, vectors), to tell the cameras apart.
// The host functions are registered in every build (a TOTK_VR=1 run of a build without --vr reaches them too); without
// --vr they only perform the displaced loads.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dispatch.h"
#include "host.h"
#if __has_include("patches.h")   // (generated: the patch page; absent in the tests)
#include "patches.h"
#endif

#ifdef TOTK_VR
#include <pthread.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "vr.h"
#include "vr_internal.h"
#include "vr_math.h"
#endif

#define PROJ_NEAR 0x98
#define PROJ_FAR 0x9c
#define PROJ_TAN 0xac

static int log_left = -1, plog_left = -1;
static int log_budget(void) {
    if (log_left < 0) log_left = getenv("TOTK_VR_LOG_CAMERAS") ? atoi(getenv("TOTK_VR_LOG_CAMERAS")) : 0;
    return log_left;
}
static int plog_budget(void) {
    if (plog_left < 0) plog_left = getenv("TOTK_VR_LOG_PERSP") ? atoi(getenv("TOTK_VR_LOG_PERSP")) : 0;
    return plog_left;
}

static inline float ldf(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static inline __attribute__((unused)) void stf_(uint64_t a, float f) { memcpy((void *)(uintptr_t)a, &f, 4); }
static inline __uint128_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

#ifdef TOTK_VR
#define QUEUE 32
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int head;              // -1 not read yet: TOTK_VR_HEAD (default 1)
    bool have_neutral, recenter;
    vr_pose neutral;
    float turn;
    float window_aspect;
    float out[9];
    bool out_ok;
    float game[9];
    bool cam_set;
    vr_view cur;
    float tan_x, tan_y;
    vr_view q[QUEUE];
    unsigned q_head, q_count;
    uint64_t updates;
} c = {.head = -1, .window_aspect = 16.0f / 9.0f};

static bool head_on(void) {
    if (c.head < 0) {
        const char *e = getenv("TOTK_VR_HEAD");
        c.head = !(e && *e == '0');
        rt_log("vr: head tracking %s", c.head ? "on (the world's view cameras follow the headset)"
                                              : "off (TOTK_VR_HEAD=0): the virtual screen only");
    }
    return c.head;
}

bool vr_world_view(void) { return vr_enabled() && vr_session_running() && head_on(); }

void vr_recenter(void) {
    pthread_mutex_lock(&lock);
    c.recenter = true;
    pthread_mutex_unlock(&lock);
}

void vr_turn(float radians) {
    pthread_mutex_lock(&lock);
    c.turn = remainderf(c.turn + radians, 2 * VR_PI);
    pthread_mutex_unlock(&lock);
}

void vr_window_aspect(float a) {
    if (a > 0.1f && a < 10) c.window_aspect = a;
}

static vr_pose from_xr(XrPosef p) {
    vr_pose r = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
                 {p.position.x, p.position.y, p.position.z}};
    return r;
}

static XrPosef to_xr(vr_pose p) {
    XrPosef r = {{p.q.x, p.q.y, p.q.z, p.q.w}, {p.p.x, p.p.y, p.p.z}};
    return r;
}

static bool locate(XrView v[2]) {
    XrTime t = vr_next_display_time();
    XrSession s = vr_xr_session();
    if (!t || s == XR_NULL_HANDLE) return false;
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, t, vr_xr_local()};
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    v[0] = (XrView){XR_TYPE_VIEW};
    v[1] = (XrView){XR_TYPE_VIEW};
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(s, &li, &vs, 2, &n, v)) || n != 2) return false;
    return (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
}

static bool near9(const float *a, const float *b) {
    for (int i = 0; i < 9; i++)
        if (fabsf(a[i] - b[i]) > 1e-4f) return false;
    return true;
}

// a view camera (eye off the origin, up near +y): f = eye[3], at[3], up[3]; mono (one view for both eyes)
static void camera_lookat(float *f) {
    if (!vr_world_view()) {
        c.out_ok = false;
        return;
    }
    pthread_mutex_lock(&lock);
    if (!(c.out_ok && near9(f, c.out))) memcpy(c.game, f, sizeof c.game);
    XrView v[2];
    if (!locate(v)) {
        pthread_mutex_unlock(&lock);
        return;
    }
    vr_pose head = from_xr(v[0].pose);
    head.p = vr_v3_scale(vr_v3_add(head.p, from_xr(v[1].pose).p), 0.5f);
    if (!c.have_neutral || c.recenter) {
        c.neutral = head;
        c.have_neutral = true;
        c.recenter = false;
        rt_log("vr: recentred: head at %.2f %.2f %.2f, heading %.1f deg", head.p.x, head.p.y, head.p.z,
               vr_quat_heading(head.q) * 57.29578f);
    }
    c.cam_set = true;
    const float *gf = c.game;
    vr_vec3 geye = vr_v3(gf[0], gf[1], gf[2]);
    float yaw = vr_game_yaw_of(vr_v3(gf[3] - gf[0], gf[4] - gf[1], gf[5] - gf[2]));
    vr_anchor a = vr_anchor_recenter(c.neutral, geye, yaw + c.turn, 1.0f);
    vr_lookat_from_view(a, head, f, f + 3, f + 6);
    memcpy(c.out, f, sizeof c.out);
    c.out_ok = true;
    float tx = 0, ty = 0;
    for (int i = 0; i < 2; i++) {
        tx = fmaxf(tx, fmaxf(tanf(v[i].fov.angleRight), tanf(-v[i].fov.angleLeft)));
        ty = fmaxf(ty, fmaxf(tanf(v[i].fov.angleUp), tanf(-v[i].fov.angleDown)));
    }
    if (tx > 0.05f && ty > 0.05f) {
        float wa = c.window_aspect;
        if (tx / ty < wa) tx = ty * wa;
        else ty = tx / wa;
        c.tan_x = tx, c.tan_y = ty;
        c.cur = (vr_view){true, -1, to_xr(head), {-atanf(tx), atanf(tx), atanf(ty), -atanf(ty)}};
    }
    c.updates++;
    pthread_mutex_unlock(&lock);
}

static bool camera_persp(float *tan_fovy, float *aspect) {
    if (!vr_world_view()) return false;
    pthread_mutex_lock(&lock);
    bool ok = c.cur.world;
    if (ok) *tan_fovy = c.tan_y, *aspect = c.tan_x / c.tan_y;
    pthread_mutex_unlock(&lock);
    return ok;
}

#ifdef _WIN32
// (reverse engineering) the writers of a camera block: its page made read-only; each write inside [lo, hi) logs the
// host instruction (totk.map turns it into the generated function, named by its guest address); the write then goes
// on by a single step with the page writable, and the page is protected again
#include <windows.h>
static uint64_t watch_lo, watch_hi, watch_page;
static volatile LONG watch_left;
static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && watch_page) {
        uint64_t a = e->ExceptionRecord->ExceptionInformation[1];
        if (e->ExceptionRecord->ExceptionInformation[0] != 1 || (a & ~0xfffull) != watch_page) return EXCEPTION_CONTINUE_SEARCH;
        if (a >= watch_lo && a < watch_hi && InterlockedDecrement(&watch_left) >= 0)
            rt_log("vr-watch: write to +%lld at exe+%llx", (long long)(a - watch_lo), (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleW(NULL)));
        DWORD old;
        VirtualProtect((void *)(uintptr_t)watch_page, 4096, PAGE_READWRITE, &old);
        e->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && watch_page) {
        DWORD old;
        if (watch_left > 0) VirtualProtect((void *)(uintptr_t)watch_page, 4096, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void watch_block(uint64_t lo, uint64_t hi) {
    if (watch_page) return;
    watch_lo = lo, watch_hi = hi, watch_page = lo & ~0xfffull;
    watch_left = getenv("TOTK_VR_WATCH") ? atoi(getenv("TOTK_VR_WATCH")) : 0;
    if (watch_left <= 0) return;
    AddVectoredExceptionHandler(1, watch_handler);
    DWORD old;
    VirtualProtect((void *)(uintptr_t)watch_page, 4096, PAGE_READONLY, &old);
    rt_log("vr-watch: %llx..%llx", (unsigned long long)lo, (unsigned long long)hi);
}
#endif

// (reverse engineering) TOTK_VR_SCAN=<presents>: that many presents after the start, every readable guest region is
// searched for 3x4 view matrices (orthonormal rotation rows) whose eye is within 30 m of the player; each is logged
// with the two words before it (a camera object's vtable is often there) and the module offsets they point into.
static void camera_scan(void) {
    static long at = -2, presents;
    if (at == -2) at = getenv("TOTK_VR_SCAN") ? atol(getenv("TOTK_VR_SCAN")) : -1;
    if (at < 0 || ++presents != at) return;
    extern int frame_rate_player_pos(float xyz[3]);
    float pl[3];
    if (!frame_rate_player_pos(pl)) {
        rt_log("vr-scan: no player yet");
        at = presents + 600;
        return;
    }
    rt_log("vr-scan: the player at %.2f %.2f %.2f", pl[0], pl[1], pl[2]);
    int found = 0;
    for (uint64_t a = 0x8000000; a < 0x8000000000ull && found < 200;) {
        RtMemoryInfo mi;
        if (mem_query(a, &mi) || !mi.size) break;
        uint64_t end = mi.base + mi.size;
        if ((mi.perm & 1) && mi.state && mi.size >= 64 && mem_guest_readable(mi.base, mi.size)) {
            const float *m = (const float *)(uintptr_t)mi.base;
            uint64_t n = mi.size / 4 - 12;
            for (uint64_t i = 0; i < n && found < 200; i++) {
                float r0 = m[i];
                if (!(r0 >= -1.001f && r0 <= 1.001f) || r0 == 0) continue;
                const float *M = m + i;   // rows: M[0..3], M[4..7], M[8..11]
                float n0 = M[0] * M[0] + M[1] * M[1] + M[2] * M[2];
                if (!(fabsf(n0 - 1) <= 2e-3f)) continue;
                float n1 = M[4] * M[4] + M[5] * M[5] + M[6] * M[6], n2 = M[8] * M[8] + M[9] * M[9] + M[10] * M[10];
                if (!(fabsf(n1 - 1) <= 2e-3f && fabsf(n2 - 1) <= 2e-3f)) continue;
                float d01 = M[0] * M[4] + M[1] * M[5] + M[2] * M[6], d02 = M[0] * M[8] + M[1] * M[9] + M[2] * M[10];
                if (!(fabsf(d01) <= 2e-3f && fabsf(d02) <= 2e-3f)) continue;
                bool fin = true;
                for (int k = 0; k < 12; k++) fin &= isfinite(M[k]) && fabsf(M[k]) < 1e6f;
                if (!fin) continue;
                if (!(M[5] >= 0.3f)) continue;   // up row, near +y
                float e[3];
                for (int k = 0; k < 3; k++) e[k] = -(M[k] * M[3] + M[4 + k] * M[7] + M[8 + k] * M[11]);
                float dx = e[0] - pl[0], dy = e[1] - pl[1], dz = e[2] - pl[2];
                if (!(dx * dx + dy * dy + dz * dz <= 900 && dx * dx + dy * dy + dz * dz >= 0.25f)) continue;
                uint64_t addr = mi.base + 4 * i, w1 = 0, w2 = 0;
                if (addr >= mi.base + 16) memcpy(&w1, (const void *)(uintptr_t)(addr - 8), 8),
                                          memcpy(&w2, (const void *)(uintptr_t)(addr - 16), 8);
                char n1s[64], n2s[64];
                rt_log("vr-scan: matrix at %llx eye %.2f %.2f %.2f fwd %.2f %.2f %.2f; [-8] %s [-16] %s",
                       (unsigned long long)addr, e[0], e[1], e[2], -M[8], -M[9], -M[10],
                       rt_guest_name(w1, n1s, sizeof n1s), rt_guest_name(w2, n2s, sizeof n2s));
#ifdef _WIN32
                {   // eye / at / up right before the matrix: the camera block; watch its writers
                    float ev[3];
                    memcpy(ev, (const void *)(uintptr_t)(addr - 36), 12);
                    if (fabsf(ev[0] - e[0]) < 0.01f && fabsf(ev[1] - e[1]) < 0.01f && fabsf(ev[2] - e[2]) < 0.01f &&
                        ((addr - 36) & 0xfff) <= 0xfff - 84)
                        watch_block(addr - 36, addr + 48);
                }
#endif
                if (found < 8) {   // the object around it: module pointers (vtables) and vector-looking floats
                    for (int64_t o = -0x300; o < 0x80; o += 8) {
                        uint64_t q;
                        memcpy(&q, (const void *)(uintptr_t)(addr + o), 8);
                        if ((q >> 32) == 0x70 || (q >> 36) == 0x7) {
                            char nm[64];
                            rt_log("vr-scan:   %+lld ptr %s", (long long)o, rt_guest_name(q, nm, sizeof nm));
                        }
                    }
                    for (int64_t o = -0x300; o < 0x80; o += 4) {
                        float v[3];
                        memcpy(v, (const void *)(uintptr_t)(addr + o), 12);
                        if (fabsf(v[0] - pl[0]) < 30 && fabsf(v[1] - pl[1]) < 30 && fabsf(v[2] - pl[2]) < 30)
                            rt_log("vr-scan:   %+lld vec %.2f %.2f %.2f", (long long)o, v[0], v[1], v[2]);
                    }
                }
                found++;
            }
        }
        a = end;
    }
    rt_log("vr-scan: %d matrices", found);
}

void vr_present_mark(void) {
    camera_scan();
    if (!vr_enabled()) return;
    pthread_mutex_lock(&lock);
    if (c.q_count == QUEUE) {
        c.q_head = (c.q_head + 1) % QUEUE;
        c.q_count--;
    }
    vr_view v = c.cur;
    v.world = v.world && c.cam_set && vr_world_view();
    c.q[(c.q_head + c.q_count++) % QUEUE] = v;
    c.cam_set = false;
    pthread_mutex_unlock(&lock);
}

bool vr_present_take(vr_view *out) {
    pthread_mutex_lock(&lock);
    bool ok = c.q_count > 0;
    if (ok) {
        *out = c.q[c.q_head];
        c.q_head = (c.q_head + 1) % QUEUE;
        c.q_count--;
    }
    pthread_mutex_unlock(&lock);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

uint64_t vr_camera_updates(void) { return c.updates; }
#endif

static bool view_camera(const float *f) {
    return (fabsf(f[0]) + fabsf(f[1]) + fabsf(f[2])) > 1.0f && f[7] > 0.5f;
}

// host call at the look-at update's first load (x0 the camera)
// the view matrix sead::LookAtCamera::doUpdateMatrix makes from eye / at / up (BotW 1.9.0 main+0xeb2d5c does the
// same): rows right, up, back (back = eye - at), each with -dot(row, eye) as its fourth element
static __attribute__((unused)) void lookat_matrix(const float *f, float m[12]) {
    float d[3] = {f[0] - f[3], f[1] - f[4], f[2] - f[5]};
    float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n > 0) d[0] /= n, d[1] /= n, d[2] /= n;
    const float *u = f + 6;
    float r[3] = {u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0]};
    n = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (n > 0) r[0] /= n, r[1] /= n, r[2] /= n;
    float v[3] = {d[1] * r[2] - d[2] * r[1], d[2] * r[0] - d[0] * r[2], d[0] * r[1] - d[1] * r[0]};
    const float *rows[3] = {r, v, d};
    for (int i = 0; i < 3; i++) {
        m[4 * i] = rows[i][0], m[4 * i + 1] = rows[i][1], m[4 * i + 2] = rows[i][2];
        m[4 * i + 3] = -(rows[i][0] * f[0] + rows[i][1] * f[1] + rows[i][2] * f[2]);
    }
}

// host call in place of `ldr x8, [x2, #0x38]` in main+0xbb7f9c (1.2.1 main+0xbb8074), where the renderer's view takes
// a copy of a sead::LookAtCamera (x2: its view matrix at +0x8, eye +0x38, at +0x44, up +0x50, as in BotW): the head
// goes on the camera here, eye / at / up and the matrix made from them, right before the copy
static __attribute__((unused)) void vr_lookat_hook(Cpu *cpu) {
    uint64_t cam = cpu->x[2];
    float *f = (float *)(uintptr_t)(cam + 0x38);
    if (log_budget() > 0) {
        log_left--;
        rt_log("vr-cam: view cam %llx (x19 %llx) view %d eye %.2f %.2f %.2f at %.2f %.2f %.2f up %.2f %.2f %.2f",
               (unsigned long long)cam, (unsigned long long)cpu->x[19], view_camera(f), f[0], f[1], f[2], f[3], f[4],
               f[5], f[6], f[7], f[8]);
    }
#ifdef TOTK_VR
    if (view_camera(f)) {
        float in[9];
        memcpy(in, f, sizeof in);
        camera_lookat(f);
        if (memcmp(in, f, sizeof in)) {
            float m[12];
            lookat_matrix(f, m);
            memcpy((void *)(uintptr_t)(cam + 0x8), m, sizeof m);
        }
    }
#endif
    uint64_t x8;
    memcpy(&x8, (const void *)(uintptr_t)(cam + 0x38), 8);
    cpu->x[8] = x8;   // the displaced ldr x8, [x2, #0x38]
}

// host call in place of the projection's ldp s1, s2, [x0, #0xac] (x0 the projection)
static __attribute__((unused)) void vr_persp_hook(Cpu *cpu) {
    uint64_t p = cpu->x[0];
    float tan_fovy = ldf(p + PROJ_TAN), aspect = ldf(p + PROJ_TAN + 4);
    float near_ = ldf(p + PROJ_NEAR), far_ = ldf(p + PROJ_FAR);
    if (plog_budget() > 0) {
        plog_left--;
        char who[64];
        rt_log("vr-cam: persp %llx from %s near %.3f far %.1f tan %.4f aspect %.4f", (unsigned long long)p,
               rt_guest_name(cpu->x[30], who, sizeof who), near_, far_, tan_fovy, aspect);
    }
#ifdef TOTK_VR
    static float min_far = -1;
    // (the world's projection: far 25000; the cube maps' 200 and 10000)
    if (min_far < 0) min_far = getenv("TOTK_VR_WORLD_FAR") ? (float)atof(getenv("TOTK_VR_WORLD_FAR")) : 20000.0f;
    if (far_ >= min_far) camera_persp(&tan_fovy, &aspect);
#endif
    cpu->v[1] = fbits(tan_fovy);
    cpu->v[2] = fbits(aspect);
}

#ifdef RT_PATCH_PAGE
#define HOST_VR_LOOKAT (RT_PATCH_PAGE + 0xf60)
#define HOST_VR_PERSP (RT_PATCH_PAGE + 0xf50)
__attribute__((constructor)) static void vr_camera_register(void) {
    rt_register_host(HOST_VR_LOOKAT, vr_lookat_hook);
    rt_register_host(HOST_VR_PERSP, vr_persp_hook);
}
#endif
