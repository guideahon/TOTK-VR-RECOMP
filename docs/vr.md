# VR mode (work in progress)

A PC-VR mode for TotK Recomp through OpenXR, ported from Savage Wind VR (the same mode for Breath of the Wild:
github.com/guideahon/Savage-Wind-VR, branch `vr`, its docs/VR.md). Built only with `tools/build_game.py --vr`, turned
on at run time with `TOTK_VR=1`. A build without `--vr` has none of it and is the same as before.

## Status

| Phase | What | State |
|---|---|---|
| 0 | OpenXR headers, the build flag, the math, the `fake_xr` test runtime, tests | done |
| 1 | OpenXR session on the game's Vulkan device; the flat game on a virtual screen in the headset | done (tested against fake_xr, not in the game) |
| 2 | Head tracking: the head on the game's camera, the headset's field of view | not started: needs the camera sites (below) |
| 3 | Stereo: a frame per eye, the game's clock once a pair | not started: needs phase 2 and the frame-step site |
| 4 | HUD apart from the scene | not started: needs phase 3 |
| 5 | VR controllers as the game's controller | done (tested against fake_xr, not in the game) |
| 6 | Comfort (snap turning ...) | not started: snap turning needs phase 2 |
| 8 | Motion combat: slashes and stabs attack, the left hand raised guards (BetterVR's analysis) | done (tested against fake_xr, not in the game) |
| 7, 9, 10 | Arms, performance, release | not started |

**None of it has run with the game yet**: it needs your own game files (base 1.0.0 and the 1.2.1 or 1.4.3 update, dumped
from your own console) set up as [vr-setup.md](vr-setup.md) says. Everything here is tested without the game (below).

## What it does now

With `TOTK_VR=1` the Vulkan instance and device are made through the OpenXR runtime (`XR_KHR_vulkan_enable2`; the
runtime names the GPU), an OpenXR session runs on the game's graphics queue, and every frame the game presents
(`vk_present_blit`, the window texture with the HUD) is copied to a quad layer: a screen 3 m wide 2.5 m ahead at eye
height (`TOTK_VR_SCREEN_WIDTH`, `TOTK_VR_SCREEN_DISTANCE`). xrWaitFrame paces the game to the headset. The VR
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

## Head tracking and stereo: what is needed

Savage Wind VR puts the head on the camera where sead::LookAtCamera::doUpdateMatrix reads the eye, at and up of a view
camera, and the headset's field of view where sead::PerspectiveProjection builds its matrix; stereo then draws a frame
per eye with the game's clock stepped once a pair. TotK uses the same sead library. `tools/vr/find_vr_sites.py <your
extracted main>` looks for those two functions in your own main with Savage Wind's patterns and prints their
module offsets and instruction words (on BotW 1.9.0's main it finds the very sites Savage Wind uses). The next steps:

1. Run it on your main. Each found site becomes a line of a `vr` set in recomp/patches-<ver>.txt: a `bl` to a host
   callback on the patch page, as the `mouse_direct` set does (recomp/patches.py), registered with
   `rt_register_host`.
2. Behind the camera callback, Savage Wind VR's runtime/vr/vr_camera.c and vr_pair.h; behind the projection one, its
   `vr_persp`; the frame's eye and pose carried from the game's present to `vr_present`, as Savage Wind does
   (vr_present_mark / vr_present_begin), and a projection layer in place of the quad.
3. The game's frame step for stereo: the `realtime` set already reads the step from the patch page
   (runtime/frame_rate.c): a pair of frames takes the step once.

## Tests

None needs the game: `.venv\Scripts\python tools\vr\test_vr.py` (MSYS2 UCRT64 clang first on PATH).

| Test | What |
|---|---|
| `test_vr_math` | poses, the tracking space to the world, projections |
| `test_vr_input_map` | each controller input's game button or stick |
| `test_vr_melee` | slashes and stabs against slow motions; the shield gesture |
| `test_fake_xr_session`, `_vulkan`, `_loader` | fake_xr, the scripted OpenXR runtime, driven directly, with Vulkan, and through the system's loader |
| `test_vr_present` | runtime/vr.c and vr_input.c end to end against fake_xr: the instance and device through the runtime, the session, frames of a known colour to the virtual screen, read back from fake_xr's capture |
| compile checks | vk_device.c, gfx_present.c, input.c, vr.c, vr_input.c with and without `TOTK_VR`, `-Werror` |

fake_xr (`tools/vr/fake_xr/`) and how to script it: Savage Wind VR's docs/VR.md, "fake_xr".

## Files

| File | What |
|---|---|
| `runtime/vr.c`, `vr.h`, `vr_internal.h` | the instance, the headset, Vulkan through the runtime, the session, the virtual screen |
| `runtime/vr_input.c`, `vr_input_map.h` | the controllers and motion combat |
| `runtime/vr_math.h`, `vr_melee.h` | the math; BetterVR's swing analysis (MIT) |
| `runtime/third_party/openxr/` | the Khronos OpenXR headers (Apache 2.0) |
| `runtime/vk_device.c`, `gfx_present.c`, `input.c` | the hooks, each behind `TOTK_VR` |
| `tools/build_game.py` | `--vr` |
| `tools/vr/` | fake_xr, its tests, `test_vr.py`, `find_vr_sites.py` |
| `tests/vr/` | the unit tests, `test_vr_present.c` |
