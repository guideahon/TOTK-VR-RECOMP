#!/usr/bin/env python3
"""Find, in your own TotK main, the code the VR mode's head tracking and stereo will hook (docs/vr.md, phase 2).

TotK and BotW share Nintendo's sead library. Savage Wind VR hooks two of its functions in BotW 1.9.0 (its
tools/game_sites.py): sead::LookAtCamera::doUpdateMatrix, where the view cameras' eye / at / up become the view
matrix (the head goes on the camera there), and sead::PerspectiveProjection's matrix, where tan(fovy / 2) and the aspect
are loaded (the headset's field of view goes there). This looks for the same instruction patterns in TotK's main and
prints where they are, as module offsets with the instruction words there: what a `site` line of
recomp/patches-<ver>.txt needs (recomp/patches.py). Nothing is written; nothing from the game is kept.

  .venv\\Scripts\\python tools\\vr\\find_vr_sites.py <extracted exefs main>
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'recomp'))

# (name, pattern of instruction words, the word of the pattern the hook goes on, what it is) - from Savage Wind VR
PATTERNS = [
    ('lookat_update', '2d470801 2d480c00 1e232020 2d491404 1e240440 1e250400 54000000/ff00001f 1e233823', 0,
     'sead::LookAtCamera::doUpdateMatrix: ldp s1, s2, [x0, #0x38] (eye x, y; at +0x44, up +0x50)'),
    ('persp_matrix', 'bd409800 1e2c1003 52b7f008 2d558801 1e202800 2d569404', 3,
     'sead::PerspectiveProjection matrix: ldp s1, s2, [x0, #0xac] (tan(fovy / 2), aspect)'),
    ('persp_matrix_offset', 'bd409800 1e2c1003 52b7f008 2d558801 1e202800 2d569404', 5,
     'sead::PerspectiveProjection matrix: ldp s4, s5, [x0, #0xb4] (the frustum offset)'),
]


def compile_pattern(p):
    """'xxxxxxxx' words, or 'value/mask' (hex, little endian in the file) -> a bytes regex."""
    parts = []
    for w in p.split():
        if '/' in w:
            v, m = (int(x, 16) for x in w.split('/'))
        else:
            v, m = int(w, 16), 0xFFFFFFFF
        for i in range(4):
            vb, mb = (v >> (8 * i)) & 0xFF, (m >> (8 * i)) & 0xFF
            if mb == 0xFF:
                parts.append(re.escape(bytes([vb])))
            elif mb == 0:
                parts.append(b'.')
            else:
                parts.append(b'[' + b''.join(re.escape(bytes([b])) for b in range(256) if b & mb == vb & mb) + b']')
    return re.compile(b''.join(parts), re.DOTALL)


def load_text(path):
    try:
        from nso import Nso  # (recomp/nso.py: decompresses the segments)
        n = Nso(str(path))
        off, size = n.text
        return bytes(n.image[off:off + size]), off
    except Exception as e:  # (a plain decompressed main.text dump)
        print(f'(recomp/nso.py could not read it: {e}; taking the file as raw .text)')
        return Path(path).read_bytes(), 0


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    text, text_off = load_text(sys.argv[1])
    print(f'main .text: {len(text):#x} bytes at module offset {text_off:#x}')
    found = 0
    for name, pat, word, what in PATTERNS:
        rx, hits, pos = compile_pattern(pat), [], 0
        while len(hits) < 4:
            m = rx.search(text, pos)
            if not m:
                break
            if m.start() % 4 == 0:
                hits.append(m.start())
            pos = m.start() + 1
        print(f'\n{name}: {what}')
        if not hits:
            print('  not found: the compiler made it differently in TotK; look for it by hand (docs/vr.md)')
            continue
        for h in hits:
            at = h + 4 * word
            w = struct.unpack_from('<I', text, at)[0]
            print(f'  main+{text_off + at:x}  {w:08x}')
            found += 1
        if len(hits) > 1:
            print('  (more than one: the right one is the one the game calls every frame for its view camera)')
    print(f'\n{found} site(s). They go into a "vr" set of recomp/patches-<ver>.txt as a bl to a host callback (as the '
          'mouse_direct set does), with the camera code of Savage Wind VR (runtime/vr/vr_camera.c) behind it: '
          'docs/vr.md, "Head tracking and stereo".')
    return 0


if __name__ == '__main__':
    sys.exit(main())
