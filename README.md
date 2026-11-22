<h1 align="center">TOTK VR Recomp</h1>

<p align="center"><i>The Legend of Zelda: Tears of the Kingdom</i> in PC VR, native on Windows and Linux</p>

<p align="center">
  <a href="#install">Install</a> ·
  <a href="docs/vr.md">VR mode</a> ·
  <a href="docs/vr-setup.md">Setup</a> ·
  <a href="docs/vr-parity.md">What is ported</a> ·
  <a href="https://github.com/guideahon/TOTK-VR-RECOMP/issues">Issues</a> ·
  <a href="docs/vr-setup.md#en-español">Español</a>
</p>

**TOTK VR Recomp** adds a PC-VR mode (OpenXR) to a native recompilation of *Tears of the Kingdom* (Nintendo Switch):
stereo and 6DOF head tracking in play, first person (or third), the menus and loading on a virtual screen, VR
controllers with [BetterVR](https://github.com/Crementif/BotW-BetterVR)'s body gestures, motion combat, roomscale and
vibration. It is built from your own copy of the game.

The recompilation underneath is [TotK Recomp](https://github.com/valentderah/totk-pc-recomp); this project is a fork of
it that adds VR. The VR mode is ported from its sister project for *Breath of the Wild*,
[BOTW VR Recomp](https://github.com/guideahon/BOTW-VR-RECOMP), and brought up to date with it
([docs/vr-parity.md](docs/vr-parity.md)). Everything TotK Recomp does (frame rates, resolutions, DLSS, HDR, mods,
keyboard and gamepad, ...) works here too, and you can still play flat.

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from
> **your own** Nintendo Switch: the base game and the **1.2.1** update (1.4.3 and 1.1.0 build too, with the virtual
> screen only). This project does not provide game files, keys, firmware, dumping or decryption tools, or instructions
> on how to obtain them. The game code is recompiled locally, on your machine, from your files.

> [!WARNING]
> **Status: early.** It has run with the game (1.2.1) against a scripted OpenXR runtime (fake_xr), in stereo and in
> first person, on Windows and Linux; it has not been played in a headset yet. The HUD is still drawn into each eye's
> picture, and Link's arms, the weapon in hand and the bow aimed by hand are not ported yet. What works and what is
> missing: [docs/vr.md](docs/vr.md) and [docs/vr-parity.md](docs/vr-parity.md).

> [!NOTE]
> **Made almost entirely with AI.** Nearly all of the code, tooling, reverse engineering and documentation is written
> by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by a human, the same way TotK
> Recomp, BOTW VR Recomp and LAS Recomp are made. If you don't trust code written by AI, you can simply ignore this
> project.

## Install

**You need:** your own game files, Windows 10 or later or Linux 6.7 or later on x86-64, a CPU with AVX2, a GPU with
Vulkan 1.3 and `VK_EXT_external_memory_host`, about 60 GB free during setup (20 GB with the import of extracted
folders), and a PC-VR headset with an OpenXR runtime: Quest 2/3/Pro through Virtual Desktop (VDXR or SteamVR), Quest
Link or Air Link, SteamVR headsets, WMR; on Linux, the system's OpenXR runtime (WiVRn, Monado).

There are no release downloads of the VR version yet: it is built from source, on your PC.

- **Windows:** run `Install TOTK VR.bat` at the repository's root. It asks for your ExeFS and RomFS folders, installs
  what is missing (MSYS2, Python, the .NET 10 SDK), builds the game with VR and writes `Play TOTK VR.bat` with your
  options, plus a desktop shortcut. Start the headset's OpenXR runtime, then play.
- **Linux:** follow [docs/building.md](docs/building.md) ("Linux") with `tools/build_game.py --vr`, then start
  `build/v121/totk` with `TOTK_VR=1`.

Step by step, dumping your own game and the manual way: [docs/vr-setup.md](docs/vr-setup.md) (también en español).
In VR set `TOTK_FPS_LIMIT=240`: in stereo the game runs at twice the headset's rate.

## Playing

Every VR option is in the game's settings menu (F1, or the VR controllers' menu button held), on the **VR** tab, and in
`totk.ini`'s `[vr]` section: view, cutscenes, turning, controls, comfort, combat, play position.

The default layout (Quest controllers):

| Control | In the game | Control | In the game |
|---|---|---|---|
| A | accept, interact | B | cancel, put away (a tap) |
| left stick | walk; click: crouch; held: run | right stick | turn; up: jump; click: inventory; held: the scope |
| right trigger | the right hand's action (attack, draw the bow, use the ability); held: weapon menu | left trigger | shield and lock on, parry on the press; held: shield menu |
| right grip | draw or put away the bow; held: bow menu | left grip | grab; with the bow out, held: arrow menu |
| Y | the ability; held: the abilities' wheel | X | the map |
| menu button | pause; held: the settings menu (F1) | both hands at the mouth + both triggers | whistle |

Body slots (from BetterVR): right shoulder + grip, the weapon; left shoulder, the bow; left hip, the ability (held:
its wheel). Slashes and thrusts of the right hand attack. `vr_controls = bettervr` gives BetterVR's own buttons,
`classic` a gamepad's layout ([docs/vr.md](docs/vr.md)).

## Status and docs

- **[VR mode](docs/vr.md)**: status, every setting, how it hooks into the game, the tests.
- **[What is ported](docs/vr-parity.md)**: each BOTW VR feature, and what it needs in TotK.
- **[Headset checklist](docs/vr-checklist.md)**: what to try in a headset and how to report it.
- **[Setup](docs/vr-setup.md)**: from zero, dumping your own game.

Everything about the base game, which this project shares with TotK Recomp: [controls](docs/controls.md),
[settings](docs/settings.md) and [totk.ini.example](totk.ini.example), [mods](docs/mods.md), [cheats](docs/cheats.md),
[building](docs/building.md), [testers](docs/tester-guide.md). Saves go to `build/profile` (1.2.1).

## Found a bug?

If it also happens flat (`TOTK_VR=0`), it is most likely in the base game: check
[TotK Recomp's issues](https://github.com/valentderah/totk-pc-recomp/issues), and report it there only if it happens in
an official TotK Recomp build too. VR bugs go to
[this project's issues](https://github.com/guideahon/TOTK-VR-RECOMP/issues): say which headset and runtime, the game
version, what you did, and attach the log (`logs/totk.log` next to the game; the `vr:` lines matter most).

## Contributing

Help is welcome, from porting the features [docs/vr-parity.md](docs/vr-parity.md) still lists to playing it in a
headset with [docs/vr-checklist.md](docs/vr-checklist.md): anyone with an AI coding agent can pick a row. Open pull
requests against `main`. See [CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md); run
`tools/check_content.py` before a commit.

## Respect copyright

- *The Legend of Zelda: Tears of the Kingdom* is © Nintendo. This is a fan project. It is not affiliated with,
  endorsed by or sponsored by Nintendo. *Nintendo Switch* and *The Legend of Zelda* are trademarks of Nintendo.
- This repository contains no game files, no game code (also not the C code the recompiler generates from it), no
  assets, no keys.
- The game code you generate and the build made from it contain Nintendo's code. They are for your own use only:
  don't share them, upload them or ship them.
- Issues, pull requests or discussions that contain game files, keys, generated code or links to pirated copies
  will be removed. Piracy is not supported in any way.
- Follow the copyright laws of your country. Dumping a game you own for personal use is legal in some countries and
  not in others; that is your responsibility.

## Credits and license

- **VR:** ported from [BOTW VR Recomp](https://github.com/guideahon/BOTW-VR-RECOMP). Its VR design, controls and
  gestures follow [BetterVR](https://github.com/Crementif/BotW-BetterVR) by Crementif and contributors (its swing
  analysis by Acudofy, MIT); BetterVR itself credits the [BotW decompilation project](https://github.com/zeldaret/botw)
  by leoetlino and others.
- **The recompilation:** [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) by Valiantsin Dzerakh and
  contributors (MIT), built on [LAS Recomp](https://github.com/peiikai/LASRecomp),
  [MacNativeGames](https://github.com/ly05010419/MacNativeGames), [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)
  and [nxnative](https://github.com/seedds/nxnative), [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp),
  [Ryujinx](https://github.com/Ryujinx/Ryujinx), [deko3d](https://github.com/devkitPro/deko3d),
  [hactool](https://github.com/SciresM/hactool), [SDL 3](https://libsdl.org),
  [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx),
  [Zstandard](https://github.com/facebook/zstd), [OpenXR](https://www.khronos.org/openxr/), and
  [Switchbrew](https://switchbrew.org) and the Switch homebrew community.
- What was taken from each, with its license: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The code is under the [MIT License](LICENSE), as upstream's; third-party code keeps its own license. The license covers
this project's code only: it grants nothing for Nintendo's game, which you must supply yourself.
