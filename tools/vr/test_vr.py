#!/usr/bin/env python3
"""The VR mode's tests (docs/vr.md), none of which needs the game: the math, the controller mapping and motion combat
(unit tests), fake_xr (the scripted OpenXR runtime) driven directly, with Vulkan and through the system's OpenXR
loader, and the runtime files the VR mode touches compiled with and without TOTK_VR.

  .venv\\Scripts\\python tools\\vr\\test_vr.py            (MSYS2 UCRT64 clang first on PATH, or --cc)
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIN = os.name == 'nt'
EXE = '.exe' if WIN else ''


def run(cmd, env=None, quiet=False):
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if out and (not quiet or r.returncode):
        print('    ' + out.replace('\n', '\n    '))
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cc', default='clang')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'vr-tests')
    ap.add_argument('--vma', type=Path, default=Path(r'C:\msys64\ucrt64\include\vma') if WIN else Path('/usr/include'))
    args = ap.parse_args()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    cc = args.cc
    inc = ['-I', 'runtime', '-I', 'runtime/third_party/openxr', '-I', 'tests/vr']
    cflags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers']
    dl = [] if WIN else ['-ldl']
    vk = ['-lvulkan-1'] if WIN else ['-lvulkan']
    failed = []

    def step(name, ok):
        print('{} {}'.format('ok  ' if ok else 'FAIL', name))
        if not ok:
            failed.append(name)

    # unit tests
    for t in ('test_vr_math', 'test_vr_input_map', 'test_vr_melee'):
        exe = out / (t + EXE)
        ok = run([cc, *cflags, *inc, '-o', str(exe), f'tests/vr/{t}.c', '-lm'])
        step(t, ok and run([str(exe)]))
    # fake_xr and the tests that drive it
    lib = out / ('fake_xr.dll' if WIN else 'fake_xr.so')
    ok = run([cc, *cflags, *inc, '-fvisibility=hidden', '-shared', *([] if WIN else ['-fPIC']), '-o', str(lib),
              'tools/vr/fake_xr/fake_xr.c', '-lm'])
    step('fake_xr', ok)
    manifest = out / 'fake_xr.json'
    manifest.write_text('{\n  "file_format_version": "1.0.0",\n  "runtime": {\n    "name": "Savage Wind fake_xr",\n'
                        f'    "library_path": "./{lib.name}"\n  }}\n}}\n')
    for t, extra in (('test_fake_xr_session', dl), ('test_fake_xr_vulkan', vk + dl)):
        exe = out / (t + EXE)
        scratch = out / t
        scratch.mkdir(exist_ok=True)
        ok = run([cc, *cflags, *inc, '-o', str(exe), f'tools/vr/{t}.c', *extra, '-lm'])
        step(t, ok and run([str(exe), str(lib), str(scratch)]))
    exe = out / ('test_fake_xr_loader' + EXE)
    ok = run([cc, *cflags, *inc, '-o', str(exe), 'tools/vr/test_fake_xr_loader.c', '-lopenxr_loader', '-lm'])
    env = dict(os.environ, XR_RUNTIME_JSON=str(manifest))
    step('test_fake_xr_loader (an elevated shell ignores XR_RUNTIME_JSON)', ok and run([str(exe)], env=env))
    # runtime/vr.c and vr_input.c end to end against fake_xr (the rest of the runtime stubbed; Windows)
    if WIN:
        exe = out / ('test_vr_present' + EXE)
        scratch = out / 'test_vr_present'
        scratch.mkdir(exist_ok=True)
        ok = run([cc, *cflags, '-Wno-unused-function', '-DTOTK_VR', *inc, '-I', 'runtime/third_party', '-I', str(args.vma),
                  '-o', str(exe), 'tests/vr/test_vr_present.c', 'runtime/vr.c', 'runtime/vr_input.c', 'runtime/vr_camera.c',
                  '-lopenxr_loader', *vk, '-lm'])
        step('test_vr_present (runtime/vr.c with fake_xr)', ok and run([str(exe), str(scratch)], env=env))
    # the runtime files the VR mode touches, with and without it
    rt = ['-fsyntax-only', '-O2', '-Wall', '-Werror', '-fno-strict-aliasing', '-march=x86-64-v3', '-I', 'runtime',
          '-I', 'runtime/third_party', '-I', 'runtime/third_party/openxr', '-I', str(args.vma)]
    if WIN:
        rt.append('-D_WIN32_WINNT=0x0A00')
    for vr in ([], ['-DTOTK_VR']):
        for f in ('vk_device.c', 'gfx_present.c', 'input.c', 'vr.c', 'vr_input.c', 'vr_camera.c'):
            step('compile {}{}'.format(f, ' (TOTK_VR)' if vr else ''), run([cc, *rt, *vr, f'runtime/{f}'], quiet=True))
    print('{} failed'.format(len(failed)) if failed else 'all passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
