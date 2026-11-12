#!/usr/bin/env python3
"""Each eye's own off-centre frustum (docs/vr.md), in play against fake_xr in stereo (its eyes are off-centre, as a
Quest's: 54 deg out, 40 deg in, 44 up, 51 down). From Savage Wind VR's test of the same name. The game is run three
times from the title through a new game's opening (an input script), the head still: with the symmetric frustum
(vr_asymmetric = 0), with the eye's own, and with the offset's sign flipped (TOTK_VR_OFFSET_SIGN=-1). The left eye's
last capture of each run is mapped to view angles with the field it was submitted with (fake_xr's `capture ... fov`
lines) and compared there with the symmetric one: the eye's own must line up (at most a couple of samples, 0.01 of a
tangent each, apart) and cover a smaller angle (the same pixels on less: sharper); the flipped one must not line up.

  .venv\\Scripts\\python tools\\vr\\test_vr_frustum.py --input <input script reaching play> [--seconds 150]

Needs build/<ver>/totk.exe built with --vr, the game files it was built for, and fake_xr built
(tools/vr/test_vr.py builds it into build/vr-tests).
"""
import argparse
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STEP = 0.01
GX = [-0.75 + STEP * i for i in range(int(1.35 / STEP))]    # (tangents both pictures cover)
GY = [0.75 - STEP * j for j in range(int(1.5 / STEP))]


def read_ppm(path):
    b = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+\d+\s', b)
    return int(m.group(1)), int(m.group(2)), b[m.end():]


def to_angles(path, fov):
    """the picture's grey levels on the common grid of tangents"""
    w, h, px = read_ppm(path)
    tl, tr, tu, td = (math.tan(a) for a in fov)
    grid = []
    for ty in GY:
        v = int((tu - ty) / (tu - td) * h)
        row = []
        for tx in GX:
            u = int((tx - tl) / (tr - tl) * w)
            if 0 <= u < w and 0 <= v < h:
                i = (v * w + u) * 3
                row.append((px[i] + px[i + 1] + px[i + 2]) / 3)
            else:
                row.append(None)
        grid.append(row)
    return grid


def best_shift(a, b, r=12):
    """the (dx, dy) in grid samples that best lines b up with a, away from the HUD's top and bottom rows"""
    best = None
    for dy in range(-r, r + 1, 2):
        for dx in range(-r, r + 1):
            s = n = 0
            for j in range(len(GY) // 5, len(GY) * 4 // 5, 3):
                ra, rb = a[j], b[j + dy] if 0 <= j + dy < len(GY) else None
                if rb is None:
                    continue
                for i in range(r, len(GX) - r, 3):
                    x, y = ra[i], rb[i + dx]
                    if x is not None and y is not None:
                        s += abs(x - y)
                        n += 1
            if n and (best is None or s / n < best[0]):
                best = (s / n, dx, dy)
    return best


def run(args, tag, env_extra):
    out = ROOT / 'build' / 'vr-frustum' / tag
    cap = out / 'cap'
    cap.mkdir(parents=True, exist_ok=True)
    for f in cap.glob('*.ppm'):
        f.unlink()
    env = dict(os.environ)
    env.update({'TOTK_VR': '1', 'TOTK_FPS_LIMIT': '240', 'TOTK_ASSUME_FOCUS': '1', 'TOTK_INPUT': 'none',
                'TOTK_BACKGROUND_WINDOW': '1', 'TOTK_VR_ROOMSCALE': '0', 'TOTK_INPUT_SCRIPT': str(args.input),
                'XR_RUNTIME_JSON': str(ROOT / 'build' / 'vr-tests' / 'fake_xr.json'),
                'FAKE_XR_LOG': str(out / 'fake_xr.log'), 'FAKE_XR_CAPTURE': str(cap), 'FAKE_XR_CAPTURE_EVERY': '300',
                'PATH': r'C:\msys64\ucrt64\bin;' + env.get('PATH', '')})
    env.update(env_extra)
    exe = ROOT / 'build' / args.version / 'totk.exe'
    with open(out / 'totk.log', 'w') as log:
        p = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(args.seconds)
        finally:
            subprocess.run(['taskkill', '/PID', str(p.pid), '/F'], capture_output=True)
            p.wait()
    text = open(out / 'fake_xr.log', errors='replace').read()
    fovs = re.findall(r'capture (frame\d+_layer0_view0) fov (\S+) (\S+) (\S+) (\S+)', text)
    if not fovs:
        sys.exit(f'{tag}: no stereo captures (did the run reach play?)')
    tag_, *f = fovs[-1]
    f = [float(x) for x in f]
    print(f'{tag}: {tag_}')
    return to_angles(cap / (tag_ + '.ppm'), f), f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--version', default='v121')
    ap.add_argument('--seconds', type=float, default=150)
    a = ap.parse_args()
    sym, fs = run(a, 'sym', {'TOTK_VR_ASYMMETRIC': '0'})
    asy, fa = run(a, 'asym', {'TOTK_VR_ASYMMETRIC': '1'})
    bad, fb = run(a, 'flipped', {'TOTK_VR_ASYMMETRIC': '1', 'TOTK_VR_OFFSET_SIGN': '-1'})
    deg = lambda f: ' '.join('{:.1f}'.format(math.degrees(x)) for x in f)  # noqa: E731
    area = lambda f: (math.tan(f[1]) - math.tan(f[0])) * (math.tan(f[2]) - math.tan(f[3]))  # noqa: E731
    print('left eye submitted with: symmetric {} | own {} (deg L R U D); angular area {:.2f} -> {:.2f} ({:+.0%})'.format(
        deg(fs), deg(fa), area(fs), area(fa), area(fa) / area(fs) - 1))
    e1, dx1, dy1 = best_shift(sym, asy)
    e2, dx2, dy2 = best_shift(sym, bad)
    print('own frustum vs symmetric: best match at ({}, {}) samples, difference {:.1f}'.format(dx1, dy1, e1))
    print('offset sign flipped     : best match at ({}, {}) samples, difference {:.1f}'.format(dx2, dy2, e2))
    ok = abs(dx1) <= 2 and abs(dy1) <= 2 and area(fa) < area(fs) * 0.9 and (abs(dx2) > 4 or abs(dy2) > 4)
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
