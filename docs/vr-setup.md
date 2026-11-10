# TOTK VR Recomp: setting it up from zero

[The VR mode: docs/vr.md](vr.md) · *En español: [más abajo](#en-español)*

This repository is TotK Recomp (a native PC port of *Tears of the Kingdom*, built from your own game) with a PC-VR mode
through OpenXR, ported from [BOTW VR Recomp](https://github.com/guideahon/BOTW-VR-RECOMP). It contains **no game files,
keys or firmware**: you need your own copy, dumped from your own Nintendo Switch. The VR mode shows menus and loading
on a virtual screen, and in play the world in stereo with 6DOF head tracking (game 1.2.1 for now; 1.4.3 gets the
virtual screen only), with VR controllers and motion combat ([vr.md](vr.md), "Status").

## 1. What you need

- Your own *Tears of the Kingdom* (cartridge or eShop) with the **1.2.1 or 1.4.3 update**, on a Switch you own that can
  run homebrew.
- Windows 10/11 x86-64 with AVX2, a GPU with Vulkan 1.3, about 60 GB free (20 with the import below).
- A PC-VR headset with an OpenXR runtime: Quest through Virtual Desktop (VDXR or SteamVR), Quest Link, SteamVR, WMR.

## 2. Dumping your own game

Usual community tools; this project ships none of them. Follow your country's laws.

**A. As NSP files (the recomp's own path).** On your Switch, dump the base game and the update with
[nxdumptool](https://github.com/DarkMatterCore/nxdumptool), and your console's keys (`prod.keys`) with
[Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM). Then set `config.local.toml` (copy of `config.example.toml`)
to them and run `tools/extract.py`: it decrypts, merges and checks every hash ([building.md](building.md), "Build",
step 3).

**B. As extracted folders.** If you extracted the ExeFS and the RomFS yourself (an emulator's "Extract RomFS / ExeFS"
with the update installed, or nxdumptool with the update applied), import them:

```
.venv\Scripts\python tools\import_dump.py --exefs <folder with main, main.npdm, rtld, sdk, subsdk0> --romfs <RomFS folder>
```

It checks the ExeFS against the supported versions (that also tells it the version), copies it to
`build\extracted\<ver>\exefs` and packs the RomFS (it must be the merged one, base + update) into
`build\extracted\<ver>\romfs\romfs.bin` with its index. With both in one folder:
`--exefs D:\TOTK --romfs D:\TOTK --exclude totk-pc-recomp` (anything else in the folder that is not RomFS goes in an
`--exclude`).

Never share these files, your keys or anything under `build\` or `generated\`.

## 3. Where each folder goes

```
TOTK-VR-RECOMP/              this repository
  config.local.toml          path A: your NSPs and keys (git ignores it)
  build/extracted/<ver>/     written by tools/extract.py or tools/import_dump.py (git ignores build/)
    exefs/   main, main.npdm, rtld, sdk, subsdk0
    romfs/   romfs.bin, index.json
```

`<ver>` is `v121` for 1.2.1, `v143` for 1.4.3.

## 4. Building

As [building.md](building.md) says (MSYS2 UCRT64 clang, two Python environments), plus the OpenXR loader:
`C:\msys64\usr\bin\pacman -S --needed mingw-w64-ucrt-x86_64-openxr-sdk`. Then, in PowerShell with
`C:\msys64\ucrt64\bin` first on PATH:

```
.venv\Scripts\python tools\build_xlate.py
.venv\Scripts\python recomp\gen.py --exefs build\extracted\<ver>\exefs --out generated\<ver> --patches recomp\patches-<ver>.txt
.venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver>
.venv\Scripts\python tools\build_game.py --version <ver> --vr
.venv-py314\Scripts\python tools\prepare_shaders.py build\<ver> --version <ver>
.venv\Scripts\python tools\vr\test_vr.py
```

## 5. Playing

Start the headset's OpenXR runtime (Virtual Desktop connected with VDXR, for example), then run
`tools\vr\play-vr.bat` (edit `VER` in it) or:

```
set TOTK_VR=1
build\<ver>\totk.exe
```

The settings are in [vr.md](vr.md), "What it does now".

---

## En español

Este repositorio es TotK Recomp (port nativo de PC de *Tears of the Kingdom*, compilado desde tu propio juego) con un
modo PC-VR por OpenXR. **No incluye archivos del juego, keys ni firmware**: necesitás tu copia, volcada de tu Switch.
El modo VR muestra menús y cargas en una pantalla virtual y, jugando, el mundo en estéreo con head tracking 6DOF
(juego 1.2.1 por ahora; con 1.4.3 sólo la pantalla virtual), con controles VR y combate con movimiento.

1. **Qué necesitás**: tu TotK con update **1.2.1 o 1.4.3**, una Switch tuya con homebrew, Windows con AVX2 y Vulkan 1.3,
   ~60 GB libres (20 con la importación), y un visor con runtime OpenXR (Quest por Virtual Desktop con VDXR, etc.).
2. **Volcado**:
   - **A (NSP)**: volcá juego y update con nxdumptool, las keys de tu consola con Lockpick_RCM, completá
     `config.local.toml` y corré `tools\extract.py`.
   - **B (carpetas ya extraídas)**: `tools\import_dump.py --exefs <carpeta ExeFS> --romfs <carpeta RomFS>`; el RomFS
     tiene que ser el combinado (base + update). Si están en la misma carpeta:
     `--exefs D:\TOTK --romfs D:\TOTK --exclude totk-pc-recomp`.
3. **Carpetas**: todo queda en `build\extracted\<ver>\` (`v121` = 1.2.1, `v143` = 1.4.3); git lo ignora.
4. **Compilar**: los comandos de la sección 4 de arriba (con `--vr` en `build_game.py`).
5. **Jugar**: abrí el runtime del visor y ejecutá `tools\vr\play-vr.bat`, o `set TOTK_VR=1` y `build\<ver>\totk.exe`.

Nunca compartas los archivos del juego, tus keys ni nada de `build\` o `generated\`.
