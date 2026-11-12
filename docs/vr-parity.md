# VR: parity with Savage Wind VR (BotW)

What the BotW VR mode (guideahon/BOTW-VR-RECOMP, branch `vr`) has, and where each piece stands in TotK. TotK's
runtime is laid out differently (one `runtime/` folder, `cfg.c` settings with totk.ini, an ImGui menu, `vk_exec*.c`
instead of BotW's `nvn/vulkan/exec.c`), so each feature is ported, not copied. Engine-independent parts come first;
the game hooks need TotK 1.2.1 addresses, each an optional patch site that asserts the word it replaces.

States: **done** (ported, checked against fake_xr), **headset** (ported, needs the checklist in
[vr-checklist.md](vr-checklist.md)), **todo** (not ported: what it needs).

## Engine-independent

| Feature (BotW file) | TotK | State |
|---|---|---|
| VR options in the settings file and an in-game VR page (`settings.h` VR_OPTIONS, `vr_options.c`, `ui/game_options.mm`) | `cfg.c` rows `TOTK_VR_*` (section `vr`), `runtime/vr_options.h`, a VR tab in `ui/menu.cpp` (English texts) | done (test_vr_options; the tab on the headset: checklist) |
| BetterVR's control scheme and body gestures (`vr_gestures.h`) | `runtime/vr_gestures.h`, TotK's controls (abilities on L, held L the wheel) | done (test_vr_gestures); headset |
| Left-handed, turn speed, head-only turning, walk by the controller (`vr_input.c`) | `runtime/vr_input.c` | done |
| The game's rumble on the VR controllers, haptic pulses of gestures and swings (`vr_input.c` vr_rumble) | `ov_input.c` rumble_update -> `vr_rumble` | done (the game's rumble reached the controllers in play) |
| The desktop window shows one eye (`vr.c`) | `gfx_present.c` present_image | done |
| First person: the eye in Link's head, height offset (`vr_camera.c`) | `vr_camera.c` with `frame_rate_player_pos` | done |
| Third-person distance (`vr_camera.c`) | `vr_camera.c` | done |
| Roomscale: walking in the room walks Link (`vr_camera.c` roomscale) | `vr_camera.c` + `vr_input.c` | done; headset |
| Asymmetric per-eye frustum (`output_scale.c` rt_persp_offset, `vr_camera.c` vr_persp_offset) | sead offset at main+0xc768a0 (new `vr` site) | done (tools/vr/test_vr_frustum.py) |
| HUD apart from the scene, distance / size / follows gaze (`exec.c` vr_hud_prepare) | needs where TotK's frame stops drawing the world and starts the HUD in `vk_exec.c` | todo |
| Shadows drawn once a stereo pair (`exec.c` begin_pass) | `vk_exec.c` vk_exec_vr_shadow_skip: the right frame's draws and depth clears into the sun cascades (DEPTH16 array of 4) | done (A/B against fake_xr) |
| Per-eye TAA history (`exec.c` mvec_taa, `mvec.c`) | TotK's own AA is FXAA (spatial); the only temporal one is DLAA: `vk_dlaa.c` keeps its state per eye (Dv[2]). The off-centre field needs no jitter correction here: DLAA's jitter goes through the viewport, its motion vectors through each eye's own camera | done; not run with DLSS (no nvngx_dlss.dll here): checklist |
| Comfort vignette, controller markers, play position seated / standing (BotW 00ad668, 6d80122) | `vr.c` overlays, STAGE space | done (fake_xr in play) |
| A frame in play without the head shown as the virtual screen for one headset frame | `vr.c` repeat_last | done (1522 -> 2 layer changes a run) |

## Game hooks (TotK 1.2.1 addresses needed)

| Feature (BotW file) | What it needs in TotK | State |
|---|---|---|
| Cutscene modes and black bars (`vr_camera.c` events, `vr_event_table.h`) | `vr_game.c`: EventMgr (GOT main+0x462e3e8), +0x58 the event playing, +0xbc its name; auto: `Dm_*` on the event's camera. Pre-rendered movies on the virtual screen (`ov_movie.c` movie_playing) | done (fake_xr, the opening's Dm_OP_0037); movies: headset |
| Menus and loading on the virtual screen (`vr_world_view`: title, loading, map, pause) | the game's pause / map / loading states | todo |
| Link's head hidden, arms follow the controllers (`vr_skel.c`, `vr_ik.h`) | Link's skeleton in memory and the hook after its world matrices are made | todo |
| Weapon in hand, weapon hits by motion (`hle/weapon.c`, `vr_skel.c`) | the weapon's matrix and its attack sensor | todo |
| Two-handed grip (`vr_skel.c`) | as above | todo |
| Bow aimed by hand, trajectory arc (`hle/arrow.c`, `vr_skel.c`) | the arrow's launch | todo |
| Drop gesture (`quick_drop*.c`) | TotK's quick menu has no drop: a drop action of the game | todo |
| What each hand holds (`vr_skel.c` vr_skel_held) | the weapon models at the hand bones | todo (the gestures track it themselves) |

### What blocks the remaining game hooks

The weapon in hand and its hits, the two-handed grip, the bow aimed by hand with its arc and the drop all need Link
holding a weapon or a bow while the hook is found (watching what the game writes when he swings or shoots) and while
it is checked against fake_xr. No TotK save is available here and a scripted new game does not reach a weapon soon
enough, so these wait for a save with a weapon and a bow (`build/profile`, a save the player provides) or for play on
the headset. Leads: the strings `ArrowShoot`, `BowShoot`, `ArrowInitialSpeed`, `BowEquippedBoneName` in main, the
player actor (`frame_rate.c` player_pos: the resident actor named "Player").

