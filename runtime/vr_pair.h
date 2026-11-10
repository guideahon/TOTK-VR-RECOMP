// The stereo pair's state machine (runtime/vr_camera.c): which eye each game frame draws, and how much game time it
// takes. Header only and free of the rest of the runtime: tests/vr/test_vr_pair.c drives it in every order.
//
// A game frame calls, in some order that the game chooses (and that changes with its frame rate):
//   vr_pair_step    once, from its clock (runtime/frame_rate.c frame_step_present, for the next frame): the frame's game time
//   vr_pair_camera  for each of its cameras (vr_camera.c, the view copying its camera): the frame's eye
// and then, once, vr_pair_present (nvnQueuePresentTexture). The frame's eye is chosen by whichever of the first two
// comes first after the last present, so a frame's camera and its game time always agree: a left frame takes the pair's
// time (twice its own), a right frame nearly none, and both eyes of a pair see the same moment.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool open;     // the frame's eye is chosen (a step or a camera came since the last present)
    int eye;       // ... and it is this one: 0 left, 1 right
    int next;      // the eye of the next frame
    uint64_t pairs;
} vr_pair;

static inline int vr_pair_eye(vr_pair *p) {
    if (!p->open) {
        p->eye = p->next;
        p->open = true;
    }
    return p->eye;
}

// the frame's game time (console frames), in play with stereo on
static inline float vr_pair_step(vr_pair *p, float ratio) { return vr_pair_eye(p) == 0 ? ratio * 2 : 0.001f; }

// the eye a camera of the frame is set for
static inline int vr_pair_camera(vr_pair *p) { return vr_pair_eye(p); }

// the frame is presented; world: it was drawn for the headset in play. Returns the eye it was drawn for, -1 if none.
static inline int vr_pair_present(vr_pair *p, bool world) {
    int eye = p->open && world ? p->eye : -1;
    p->next = eye == 0 ? 1 : 0;  // (the other eye after a left one; a pair starts again after anything else)
    if (eye == 1) p->pairs++;
    p->open = false;
    return eye;
}

// out of play (a menu, an event) or stereo off: the next frame in play starts a pair
static inline void vr_pair_reset(vr_pair *p) {
    p->open = false;
    p->next = 0;
}
