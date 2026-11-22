# VR mode (work in progress)

A PC-VR mode for TotK Recomp through OpenXR, ported from BOTW VR Recomp (the same mode for Breath of the Wild:
[github.com/guideahon/BOTW-VR-RECOMP](https://github.com/guideahon/BOTW-VR-RECOMP), its docs/VR.md). Built only with `tools/build_game.py --vr`, turned
on at run time with `TOTK_VR=1`. A build without `--vr` has none of it and is the same as before.

## Status

| Phase | What | State |
|---|---|---|
| 0 | OpenXR headers, the build flag, the math, the `fake_xr` test runtime, tests | done |
| 1 | OpenXR session on the game's Vulkan device; the flat game on a virtual screen in the headset | done (tested against fake_xr, not in the game) |
| 2 | Head tracking: the head on the game's camera, the headset's field of view | done on 1.2.1 (tested in play against fake_xr) |
| 3 | Stereo: a frame per eye, the game's clock stepped across a pair | done on 1.2.1 (tested in play against fake_xr) |
| 4 | HUD apart from the scene | not started (the HUD is drawn into each eye's picture for now) |
| 4b | First person (the eye in Link's head), third-person distance, roomscale, each eye's off-centre field, the desktop window showing one eye | done on 1.2.1 (fake_xr in play) |
| 5 | VR controllers as the game's controller | done (tested against fake_xr, not in the game) |
| 6 | Comfort: snap, smooth at a speed or head-only turning, walking by the controller, left-handed | done (fake_xr) |
| 6b | BetterVR's control scheme and body gestures, haptics, the game's rumble on the controllers, the settings on the menu's VR tab | done (fake_xr; docs/vr-checklist.md for the headset) |
| 8 | Motion combat: slashes and stabs attack, the left hand raised guards (BetterVR's analysis) | done (tested against fake_xr, not in the game) |
| 7, 9, 10 | Arms, performance, release | not started |

It has run with the game (1.2.1) against fake_xr, the scripted OpenXR runtime, not yet on a headset. Head tracking and
stereo need the camera sites of a game version: 1.2.1 has them; 1.4.3 and 1.1.0 show the virtual screen only.

## What it does now

With `TOTK_VR=1` the Vulkan instance and device are made through the OpenXR runtime (`XR_KHR_vulkan_enable2`; the
runtime names the GPU), an OpenXR session runs on the game's graphics queue, and every frame the game presents
(`vk_present_blit`, the window texture with the HUD) goes to the headset. Out of play (title, loading) it is a quad
layer, a screen 3 m wide 2.5 m ahead at eye height (`TOTK_VR_SCREEN_WIDTH`, `TOTK_VR_SCREEN_DISTANCE`). In play the
world's camera follows the head (runtime/vr_camera.c): the game's camera keeps its place and heading (it still follows
Link and turns with the stick), the head adds its turn and movement since the recentring and gives pitch and roll; the
world's projection takes the headset's field of view. In stereo the game draws a frame per eye, left then right, and
the pair is shown together: the game runs at twice the headset's rate (`TOTK_FPS_LIMIT=240`, which the launcher sets;
xrWaitFrame paces it once a pair). The right frame of a pair takes the shortest game step TotK allows (1/240 s: its
physics fail on shorter ones) and the left one the rest, so the world keeps its speed and the eyes see moments 4 ms
apart. The VR
controllers are read as the game's controller (input.c, after the test script's input), and the right hand's slashes and
stabs press attack (Y), the left hand raised holds the guard (ZL). A gamepad keeps working. Without a runtime or a
headset, or when the runtime ends or loses the session, the game goes on flat and the log says why (`vr:` lines).

| VR controller | Game | VR controller | Game |
|---|---|---|---|
| left stick | move | right stick | camera |
| left stick click | crouch | right stick click | look |
| X / Y | X / Y | A / B | A / B |
| left grip | L (abilities) | right grip | R |
| left trigger | ZL | right trigger | ZR |
| menu: a tap | + | menu: held 0.6 s | the F1 menu (opens or closes it) |
| left grip + right stick | d-pad | slash / stab (right hand) | Y |
| left hand raised in front of the face | ZL (guard) | | |

Every setting is a `[vr]` key of totk.ini (totk.ini.example lists them), a row of the menu's **VR** tab (F1; all but
the first three apply at once) or a `TOTK_VR_*` variable, which wins (`vr_view` is `TOTK_VR_VIEW`).

| Key | Default | |
|---|---|---|
| `vr` | 0 | `1`: VR on |
| `vr_head`, `vr_stereo`, `vr_controllers` | 1 | `0`: the virtual screen only / one picture for both eyes / the VR controllers not read |
| `vr_screen_width`, `_distance` | 3.0, 2.5 | the virtual screen (m) |
| `vr_view` | first | `third`: the game's camera behind Link |
| `vr_height_offset` | 0 | first person: the eye this many cm higher (-50..50) |
| `vr_third_distance` | 100 | third person: the camera's distance, % of the game's (25..300) |
| `vr_asymmetric` | 1 | `0`: each eye a symmetric field instead of its own off-centre one |
| `vr_controls` | quest | `bettervr`: BetterVR's own buttons; `classic`: the table below instead of the gestures |
| `vr_stick_deadzone` | 12 | the VR controllers' sticks' dead zone, % (5..40) |
| `vr_angvel_fix` | auto | the hands' angular velocity turned back on Meta's own PC runtime (BetterVR's fix); `on`, `off` |
| `vr_shadows_once` | 0 | `1`: in stereo the right eye reuses the left's sun shadows (faster; the left eye's flicker) |
| `vr_left_handed` | 0 | `1`: the hands' roles swapped |
| `vr_turn` | smooth | `snap` (steps of `vr_snap_degrees`, 45) or `head` (the stick does not turn, first person) |
| `vr_turn_speed` | 100 | smooth turning, % |
| `vr_walk` | head | `controller`: the left stick walks the way the left controller points |
| `vr_roomscale` | 1 | first person: walking in the room walks Link |
| `vr_rumble`, `vr_rumble_strength` | 1, 100 | the game's rumble on the VR controllers, % |
| `vr_motion`, `vr_swing`, `vr_swing_attack`, `vr_shield` | 1, normal, 1, 1 | motion combat (`vr_swing = easy`: slower swings count) |
| `TOTK_FPS_LIMIT` | 60 | set it to 240 in VR: the game runs at twice the headset's rate in stereo |

The body gestures (runtime/vr_gestures.h, from BOTW VR): a grip over the right shoulder takes out the weapon (held: its
quick menu), over the left shoulder the bow (held: shields or bows), at the left hip the ability (L; held: the
abilities' wheel), both hands at the mouth whistle (with the grips, or the Quest layout's triggers). The Quest layout
(the default): A accepts and interacts, B cancels, the right trigger uses what is in hand (attack, draw the bow, use the
ability with A; held: the weapons' menu), the left one guards and parries (held: the shields'), the right grip away
from the slots takes the bow out (held: the bows' menu; the left grip with the bow out: the arrows'), the right stick
pushed up jumps, left Y the ability (held: its wheel), left X the map, the left stick's click crouches (held: run), the
right one opens the inventory (held: the scope). BetterVR's own buttons (`vr_controls = bettervr`): right A grabs (held:
run), right B jumps, the rest as above. The table above is the classic scheme. What is still to port from BotW, and why: [vr-parity.md](vr-parity.md); what to try
on a headset: [vr-checklist.md](vr-checklist.md).

## Building and playing

As docs/building.md says, with the MSYS2 UCRT64 toolchain (the installer's llvm-mingw has no OpenXR loader), plus the
loader: `pacman -S mingw-w64-ucrt-x86_64-openxr-sdk`. Build with `--vr`:

```
.venv\Scripts\python tools\build_game.py --version <ver> --vr
set TOTK_VR=1
build\<ver>\totk.exe
```

Start the headset's OpenXR runtime first (Virtual Desktop's VDXR, SteamVR, Quest Link, WMR) and make it the active one.

## Head tracking and stereo: how they hook the game

Two sites of the `vr` patch set (recomp/patches-v121.txt, on with `TOTK_VR=1`):

- **The view's camera.** main+0xbb7f9c copies the renderer's sead::LookAtCamera, laid out as in BotW (view matrix +0x8,
  eye +0x38, at +0x44, up +0x50), into its view. A host call on its first load puts the head on the camera and makes the
  view matrix again from the new eye, at and up, as BotW's LookAtCamera::doUpdateMatrix does.
- **The projection.** sead::PerspectiveProjection's matrix (main+0xc7688c; `tools/vr/find_vr_sites.py` finds it): its
  tan(fovy / 2) and aspect become the headset's field for the world's projection (far plane 25000; the cube maps' 200
  and 10000 are left alone, `TOTK_VR_WORLD_FAR`).

The stereo pair (runtime/vr_pair.h, from Savage Wind VR): `frame_step_present` (runtime/frame_rate.c) fixes the next
frame's eye and splits the pair's step; the camera takes that eye's pose; nvnQueuePresentTexture queues what the frame
was drawn as; `vr_present` copies a left frame to the left eye's swapchain and shows the pair when the right one comes.

How the camera was found, for another game version: `TOTK_VR_SCAN=<presents>` searches guest memory, that many presents
after the start, for 3x4 view matrices whose eye is within 30 m of the player, and logs them with what is around them;
the block of eye / at / up right before a matrix is then watched (`TOTK_VR_WATCH=<n>`: its page is made read-only and
the first n writes log the host instruction, which `build/<ver>/totk.map` turns into the generated function, named by its
guest address). On 1.2.1 that gave main+0xbb7f9c, copying from x2. For 1.4.3: the same function by its code (or the same
two steps), then a `vr` set in patches-v143.txt with the two sites.

## Tests

None needs the game: `.venv\Scripts\python tools\vr\test_vr.py` (MSYS2 UCRT64 clang first on PATH).

| Test | What |
|---|---|
| `test_vr_math` | poses, the tracking space to the world, projections |
| `test_vr_pair` | the stereo pair's state machine in every order of clock and camera |
| `test_vr_input_map` | each controller input's game button or stick |
| `test_vr_melee` | slashes and stabs against slow motions; the shield gesture |
| `test_vr_gestures` | BetterVR's body slots and scheme, TotK's ability wheel on L |
| `test_vr_options` | the `[vr]` settings parsed, live changes, the tests' defaults kept in step with cfg.c |
| `test_fake_xr_session`, `_vulkan`, `_loader` | fake_xr, the scripted OpenXR runtime, driven directly, with Vulkan, and through the system's loader |
| `test_vr_present` | runtime/vr.c and vr_input.c end to end against fake_xr: the instance and device through the runtime, the session, frames of a known colour to the virtual screen, read back from fake_xr's capture |
| compile checks | vk_device.c, gfx_present.c, input.c, vr.c, vr_input.c, vr_camera.c with and without `TOTK_VR`, `-Werror` |

`tools/vr/test_vr_frustum.py --input <script>` runs the game three times against fake_xr (symmetric field, the eye's own,
the offset's sign flipped) and checks that the eye's own lines up with the symmetric picture and the flipped one does not.

fake_xr (`tools/vr/fake_xr/`) and how to script it: Savage Wind VR's docs/VR.md, "fake_xr".

## Files

| File | What |
|---|---|
| `runtime/vr.c`, `vr.h`, `vr_internal.h` | the instance, the headset, Vulkan through the runtime, the session, the virtual screen |
| `runtime/vr_camera.c`, `vr_pair.h` | head tracking and stereo: the camera and projection hooks, the pairs, the camera scan |
| `runtime/vr_input.c`, `vr_input_map.h`, `vr_gestures.h` | the controllers, BetterVR's scheme and gestures, motion combat, rumble |
| `runtime/vr_options.c`, `vr_options.h` | the `[vr]` settings, parsed once per change |
| `runtime/vr_math.h`, `vr_melee.h` | the math; BetterVR's swing analysis (MIT) |
| `runtime/third_party/openxr/` | the Khronos OpenXR headers (Apache 2.0) |
| `runtime/vk_device.c`, `gfx_present.c`, `input.c`, `nvn_api_obj.c`, `frame_rate.c` | the hooks, each behind `TOTK_VR` |
| `recomp/patches-v121.txt` | the `vr` set |
| `tools/build_game.py` | `--vr` |
| `tools/vr/` | fake_xr, its tests, `test_vr.py`, `find_vr_sites.py` |
| `tests/vr/` | the unit tests, `test_vr_present.c` |
