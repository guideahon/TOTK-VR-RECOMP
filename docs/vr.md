# VR mode (work in progress)

A PC-VR mode for TotK Recomp through OpenXR, ported from Savage Wind VR (the same mode for Breath of the Wild:
github.com/guideahon/Savage-Wind-VR, branch `vr`, its docs/VR.md). Built only with `tools/build_game.py --vr`, turned
on at run time with `TOTK_VR=1`. A build without `--vr` has none of it and is the same as before.

## Status

| Phase | What | State |
|---|---|---|
| 0 | OpenXR headers, the build flag, the math, the `fake_xr` test runtime, tests | done |
| 1 | OpenXR session on the game's Vulkan device; the flat game on a virtual screen in the headset | done (tested against fake_xr, not in the game) |
| 2 | Head tracking: the head on the game's camera, the headset's field of view | done on 1.2.1 (tested in play against fake_xr) |
| 3 | Stereo: a frame per eye, the game's clock stepped across a pair | done on 1.2.1 (tested in play against fake_xr) |
| 4 | HUD apart from the scene | not started (the HUD is drawn into each eye's picture for now) |
| 5 | VR controllers as the game's controller | done (tested against fake_xr, not in the game) |
| 6 | Comfort: snap turning | snap turning done on 1.2.1 (fake_xr in play); vignette and others not yet |
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
| menu: a tap | + | menu: held 0.6 s | - |
| left grip + right stick | d-pad | slash / stab (right hand) | Y |
| left hand raised in front of the face | ZL (guard) | | |

| Variable | Default | |
|---|---|---|
| `TOTK_VR` | 0 | `1`: VR on |
| `TOTK_VR_SCREEN_WIDTH`, `_DISTANCE` | 3.0, 2.5 | the virtual screen (m) |
| `TOTK_VR_HEAD` | 1 | `0`: no head tracking, the virtual screen only |
| `TOTK_VR_STEREO` | 1 | `0`: mono, one picture for both eyes |
| `TOTK_VR_TURN` | smooth | `snap`: the right stick turns the view in steps (`TOTK_VR_SNAP_DEGREES`, 45) |
| `TOTK_FPS_LIMIT` | 60 | set it to 240 in VR: the game runs at twice the headset's rate in stereo |
| `TOTK_VR_CONTROLLERS` | 1 | `0`: the VR controllers are not read |
| `TOTK_VR_MOTION` | 1 | `0`: no motion combat |
| `TOTK_VR_SWING` | normal | `easy`: slower swings count |
| `TOTK_VR_SHIELD` | 1 | `0`: raising the left hand does not guard |

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
| `test_fake_xr_session`, `_vulkan`, `_loader` | fake_xr, the scripted OpenXR runtime, driven directly, with Vulkan, and through the system's loader |
| `test_vr_present` | runtime/vr.c and vr_input.c end to end against fake_xr: the instance and device through the runtime, the session, frames of a known colour to the virtual screen, read back from fake_xr's capture |
| compile checks | vk_device.c, gfx_present.c, input.c, vr.c, vr_input.c, vr_camera.c with and without `TOTK_VR`, `-Werror` |

fake_xr (`tools/vr/fake_xr/`) and how to script it: Savage Wind VR's docs/VR.md, "fake_xr".

## Files

| File | What |
|---|---|
| `runtime/vr.c`, `vr.h`, `vr_internal.h` | the instance, the headset, Vulkan through the runtime, the session, the virtual screen |
| `runtime/vr_camera.c`, `vr_pair.h` | head tracking and stereo: the camera and projection hooks, the pairs, the camera scan |
| `runtime/vr_input.c`, `vr_input_map.h` | the controllers and motion combat |
| `runtime/vr_math.h`, `vr_melee.h` | the math; BetterVR's swing analysis (MIT) |
| `runtime/third_party/openxr/` | the Khronos OpenXR headers (Apache 2.0) |
| `runtime/vk_device.c`, `gfx_present.c`, `input.c`, `nvn_api_obj.c`, `frame_rate.c` | the hooks, each behind `TOTK_VR` |
| `recomp/patches-v121.txt` | the `vr` set |
| `tools/build_game.py` | `--vr` |
| `tools/vr/` | fake_xr, its tests, `test_vr.py`, `find_vr_sites.py` |
| `tests/vr/` | the unit tests, `test_vr_present.c` |
