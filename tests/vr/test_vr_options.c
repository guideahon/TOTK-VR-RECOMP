// runtime/vr_options.c: the VR rows of the settings table parsed, their defaults, a variable that sets one, a change
// seen after cfg_version moves. Also that tests/vr/vr_cfg_stub.c's defaults are runtime/cfg.c's (the file is read).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vr_options.h"
#include "vr_test.h"

extern unsigned vr_cfg_stub_version;

static void set(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
    vr_cfg_stub_version++;
}

// the default runtime/cfg.c's metas table gives key (without TOTK_), or NULL
static char cfg_c[200000];
static const char *table_default(const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "{\"%s\", \"", key);
    const char *m = strstr(cfg_c, "static const CfgMeta metas[]");   // (the sections table has rows of the keys too)
    const char *p = m ? strstr(m, pat) : NULL;
    if (!p) return NULL;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return out;
}

int main(int argc, char **argv) {
    VR_TEST("defaults");
    const vr_options_t *o = vr_options();
    CHECK(o->first_person);
    CHECK_EQ_INT(o->height_cm, 0);
    CHECK_EQ_INT(o->third_pct, 100);
    CHECK(o->asymmetric);
    CHECK(o->bettervr);
    CHECK(!o->left_handed);
    CHECK_EQ_INT(o->turn, VR_TURN_SMOOTH);
    CHECK_EQ_INT(o->snap_deg, 45);
    CHECK_EQ_INT(o->turn_pct, 100);
    CHECK(!o->walk_controller);
    CHECK(o->roomscale && o->rumble && o->motion && o->swing_attack && o->shield);
    CHECK(!o->swing_easy);
    CHECK_EQ_INT(o->rumble_pct, 100);

    VR_TEST("set, clamped, live");
    set("TOTK_VR_VIEW", "third");
    set("TOTK_VR_TURN", "head");
    set("TOTK_VR_SNAP_DEGREES", "500");
    set("TOTK_VR_HEIGHT_OFFSET", "-20");
    set("TOTK_VR_CONTROLS", "classic");
    set("TOTK_VR_SWING", "easy");
    o = vr_options();
    CHECK(!o->first_person);
    CHECK_EQ_INT(o->turn, VR_TURN_HEAD);
    CHECK_EQ_INT(o->snap_deg, 180);
    CHECK_EQ_INT(o->height_cm, -20);
    CHECK(!o->bettervr);
    CHECK(o->swing_easy);
    set("TOTK_VR_TURN", "snap");
    CHECK_EQ_INT(vr_options()->turn, VR_TURN_SNAP);

    VR_TEST("the stub's defaults are runtime/cfg.c's");
    FILE *f = fopen(argc > 1 ? argv[1] : "runtime/cfg.c", "rb");
    CHECK(f != NULL);
    if (f) {
        size_t n = fread(cfg_c, 1, sizeof cfg_c - 1, f);
        cfg_c[n] = 0;
        fclose(f);
        static const char *const keys[] = {"VR", "VR_HEAD", "VR_STEREO", "VR_CONTROLLERS", "VR_SCREEN_WIDTH",
            "VR_SCREEN_DISTANCE", "VR_VIEW", "VR_HEIGHT_OFFSET", "VR_THIRD_DISTANCE", "VR_ASYMMETRIC", "VR_CONTROLS",
            "VR_LEFT_HANDED", "VR_TURN", "VR_SNAP_DEGREES", "VR_TURN_SPEED", "VR_WALK", "VR_ROOMSCALE", "VR_RUMBLE",
            "VR_RUMBLE_STRENGTH", "VR_MOTION", "VR_SWING", "VR_SWING_ATTACK", "VR_SHIELD"};
        extern const char *cfg_value(const char *);
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
            char want[64], name[64];
            snprintf(name, sizeof name, "TOTK_%s", keys[i]);
            set(name, "");
            const char *d = table_default(keys[i], want, sizeof want);
            CHECK(d != NULL);
            if (d && strcmp(d, cfg_value(name))) fprintf(stderr, "  %s: cfg.c %s, stub %s\n", keys[i], d, cfg_value(name));
            CHECK(d && !strcmp(d, cfg_value(name)));
        }
    }
    return VR_TEST_END("test_vr_options");
}
