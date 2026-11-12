// VR mode: what the VR code reads of TotK's own state (1.2.1), each read checked and never trusted blindly.
//
// The event manager. Its methods load the instance through one GOT slot, main+0x462e3e8 (for example main+0x7d6094,
// EventMgr::EventSettingLoad by its profiler label, and its caller main+0x19ae8f0): the slot holds the address of the
// static pointer to the instance.
//
// Found with the probe below on the new game's opening: +0x58 points at the event playing (0 none: it was set at the
// opening's cutscene Dm_OP_0037 and cleared when Link got control, set again for DefaultBootEvent), and its name is a
// string at +0xbc (a 0x80-byte buffer, its size at +0xb8), whose first byte is cleared when the event ends.
//
// TOTK_VR_EVENT_PROBE=<n> (reverse engineering): every present, the first 0x400 bytes of the instance are compared
// with the last present's, and up to n changed 8-byte words are logged with the present's number, to tell which field
// follows a cutscene (the new game's opening has several).
#ifdef TOTK_VR
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#if __has_include("layout.h")   // (generated: the module bases; absent in the tests)
#include "layout.h"
#endif

#define EVT_GOT 0x462e3e8

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static __attribute__((unused)) uint64_t rd64(uint64_t a) {
    uint64_t v = 0;
    return rd(a, &v, 8) ? v : 0;
}

uint64_t vr_event_mgr(void) {
#ifdef MOD_MAIN_BASE
    if (RT_V143 || RT_V110) return 0;   // (1.2.1's slot)
    uint64_t slot = rd64(MOD_MAIN_BASE + EVT_GOT);
    return slot ? rd64(slot) : 0;
#else
    return 0;
#endif
}

// An event plays (a cutscene, a talk, a system event): its name into name (size n; "" when unreadable)
bool vr_event_now(char *name, size_t n) {
    if (n) name[0] = 0;
    uint64_t m = vr_event_mgr();
    if (!m || !rd64(m + 0x58)) return false;
    char s[64];
    if (n && rd(m + 0xbc, s, sizeof s)) {
        size_t k = 0;
        while (k < sizeof s && k + 1 < n && s[k] >= 0x20 && s[k] < 0x7f) k++;
        if (k < sizeof s && !s[k]) memcpy(name, s, k), name[k] = 0;
    }
    return true;
}

void vr_game_probe(void) {
    static long left = -2;
    static uint64_t presents;
    static uint8_t last[0x400];
    static int have;
    if (left == -2) left = getenv("TOTK_VR_EVENT_PROBE") ? atol(getenv("TOTK_VR_EVENT_PROBE")) : 0;
    if (left <= 0) return;
    presents++;
    uint64_t m = vr_event_mgr();
    uint8_t now[0x400];
    if (!m || !rd(m, now, sizeof now)) return;
    if (have)
        for (unsigned o = 0; o < sizeof now && left > 0; o += 8)
            if (o >= 0x20 && memcmp(now + o, last + o, 8)) {   // (+0x8..+0x18 change every frame: clocks)
                uint64_t a, b;
                memcpy(&a, last + o, 8), memcpy(&b, now + o, 8);
                rt_log("vr-evt: present %llu +%#x %llx -> %llx", (unsigned long long)presents, o,
                       (unsigned long long)a, (unsigned long long)b);
                left--;
            }
    memcpy(last, now, sizeof now);
    have = 1;
}
#endif
