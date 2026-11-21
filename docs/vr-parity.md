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

## Brought over from BOTW VR on 2026-11-21 (its main at 1307fa8)

| What (BOTW VR commit) | TotK | State |
|---|---|---|
| Stereo pairs split the game's time evenly (6b77955, 5b0cfbd: an uneven split shook physics) | `vr_pair.h` VR_PAIR_SECOND, `vr_camera.c` vr_frame_step (`TOTK_VR_PAIR_SECOND` to compare) | done (test_vr_pair) |
| Shadows once a pair off by default (0de01fd: the left eye's shadows flickered) | `vr_shadows_once = 0` | done |
| The queue's lock around every OpenXR call that may submit (0cf4d33, 96b5a30: Virtual Desktop / WiVRn lost the device) | `vr.c` XRQ (`TOTK_VR_QUEUE_LOCK=0`: without) | done; headset |
| A runtime failure or a lost session ends VR at once, the game goes on flat (4695348) | `vr.c` vr_xr_fatal | done |
| The Quest layout, BetterVR 1.0.1's gesture fixes, the new parry, L3 held runs, bow / arrow menus on the grips, whistle with the triggers (cf0e002, 687b238, 01be7f2, c5c5089, fd28c33, 60dd635, b22923c) | `vr_gestures.h`, `vr_controls = quest` (the default) | done (test_vr_gestures); headset |
| The ability used with A, not R (1972340) | `vr_gestures.h` | done |
| Stick dead zone, the Oculus runtime's angular velocity fix, Pico / Vive Cosmos / Steam Frame bindings (6609455) | `vr_stick_deadzone`, `vr_angvel_fix`, `vr_input.c` | done; headset |
| A swing attacks only when the hand really travels (2 m/s slash, 1.5 m/s stab), none right after drawing from a slot (0de01fd, 5525eef) | `vr_input.c` motion | done |
| The shield raised from chest height (cf0e002) | `vr_melee.h` | done (test_vr_melee) |
| Walking by the controller with the hand raised overhead (cf0e002) | `vr_input.c` controller_walk_heading | done |
| The virtual screen placed level in front of the head each time it comes up (cf0e002) | `vr.c` present | done |
| The vignette's fade in real time (91242e6) | `vr.c` overlays | done |
| Each controller's tracking state in the log (4b08705) | `vr_input.c` | done |

Not brought over, needing TotK's own hooks first (below): the menu buttons by their letters in menus (251c67a: the
game's menu state), the shield only with a shield in hand (`vr_shield_allowed`: what Link holds), Magnesis / runes
aimed by the hand, hand throws, the bow's string, climbing / gliding / riding controls (the player's movement flags).

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

