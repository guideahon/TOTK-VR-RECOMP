#!/usr/bin/env python3
"""Set up build/extracted/<version> from a game you already extracted yourself, instead of tools/extract.py.

tools/extract.py reads the NSP/XCI with your console's keys. If you instead dumped the game from your own console and
extracted its ExeFS and RomFS to folders (with an emulator's "Extract RomFS / ExeFS", or nxdumptool with the update
applied), this takes those folders:

  - the ExeFS files (main, main.npdm, rtld, sdk, subsdk0) are copied to extracted/<version>/exefs after their sha256
    is checked against tools/data/expected-v*.json, which also says the version (only a game the recomp supports is
    accepted);
  - the RomFS tree is packed into extracted/<version>/romfs/romfs.bin (the files back to back) with the index.json that
    tools/romfs_index.py and runtime/romfs.c read. The image is not Nintendo's RomFS: its hash is not the one
    expected-v*.json lists, so tools/extract.py's check does not apply; index.json says so (`integrity`).

The RomFS must be the merged one (the base game with the update applied), as the game sees it. Nothing is printed
from the game; nothing is written outside the work folder.

  .venv\\Scripts\\python tools\\import_dump.py --exefs D:\\TOTK\\exefs --romfs D:\\TOTK\\romfs
  .venv\\Scripts\\python tools\\import_dump.py --exefs D:\\TOTK --romfs D:\\TOTK --exclude totk-pc-recomp

With one folder holding both, the ExeFS files and the --exclude names are left out of the RomFS.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXEFS = ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0')
ALIGN = 16


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def work_dir():
    cfg = ROOT / 'config.local.toml'
    if cfg.is_file():
        try:
            import tomllib
            d = tomllib.loads(cfg.read_text(encoding='utf-8')).get('work', {}).get('dir')
            if d:
                p = Path(d)
                return p if p.is_absolute() else ROOT / p
        except Exception:
            pass
    return ROOT / 'build'


def identify(exefs):
    have = {n: sha256(exefs / n) for n in EXEFS if (exefs / n).is_file()}
    missing = [n for n in EXEFS if n not in have]
    if missing:
        sys.exit(f'import_dump: {", ".join(missing)} missing from {exefs}')
    for f in sorted((ROOT / 'tools' / 'data').glob('expected-v*.json')):
        e = json.loads(f.read_text())
        want = e.get('exefs_sha256') or {}
        if want and all(have[n] == want.get(n) for n in EXEFS):
            return e
    sys.exit('import_dump: the ExeFS matches no supported version (tools/data/expected-v*.json); dump the base game '
             'with a supported update applied (docs/vr-setup.md)')


def version_dir(e):
    return 'v' + e['display_version'].replace('.', '')


def walk(romfs, skip):
    out = []
    for dirpath, dirs, files in os.walk(romfs):
        rel = Path(dirpath).relative_to(romfs)
        if rel == Path('.'):
            dirs[:] = [d for d in dirs if d not in skip]
            files = [f for f in files if f not in skip]
        dirs.sort()
        for f in files:
            out.append(((rel / f).as_posix() if rel != Path('.') else f, Path(dirpath) / f))
    out.sort(key=lambda t: t[0].encode())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--exclude', action='append', default=[], help='a top-level name of --romfs to leave out')
    ap.add_argument('--work', type=Path, help='default: [work] dir of config.local.toml, or build/')
    a = ap.parse_args()
    e = identify(a.exefs)
    ver = version_dir(e)
    out = (a.work or work_dir()) / 'extracted' / ver
    print(f'game {e["display_version"]}: ExeFS hashes match; writing {out}')
    (out / 'exefs').mkdir(parents=True, exist_ok=True)
    for n in EXEFS:
        shutil.copyfile(a.exefs / n, out / 'exefs' / n)
    skip = set(a.exclude)
    if a.romfs.resolve() == a.exefs.resolve():
        skip |= set(EXEFS)
    files = walk(a.romfs, skip)
    if not files:
        sys.exit(f'import_dump: no files in {a.romfs}')
    romfs = out / 'romfs'
    romfs.mkdir(exist_ok=True)
    image = romfs / 'romfs.bin'
    tmp = image.with_suffix('.partial')
    h, index, pos = hashlib.sha256(), [], 0
    total = sum(p.stat().st_size for _, p in files)
    print(f'RomFS: {len(files):,} files, {total / 2**30:.1f} GiB -> {image}')
    with open(tmp, 'wb') as w:
        for i, (name, p) in enumerate(files):
            pad = (-pos) % ALIGN
            if pad:
                w.write(b'\0' * pad)
                h.update(b'\0' * pad)
                pos += pad
            size = 0
            with open(p, 'rb') as f:
                for b in iter(lambda: f.read(1 << 22), b''):
                    w.write(b)
                    h.update(b)
                    size += len(b)
            index.append(dict(path=name, offset=pos, size=size))
            pos += size
            if i % 20000 == 0:
                print(f'  {i:,}/{len(files):,}  {pos / 2**30:.1f} GiB', flush=True)
    tmp.replace(image)
    report = dict(status='pass', romfs_size=pos, romfs_sha256=h.hexdigest(), files=index,
                  integrity=f'packed by tools/import_dump.py from an extracted RomFS folder; ExeFS matched '
                            f'{e["display_version"]}; not Nintendo\'s RomFS image')
    (romfs / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'done: {len(index):,} files, {pos:,} bytes. Next: recomp\\gen.py --exefs {out / "exefs"} ... '
          f'--version {ver} (docs/building.md, step 5)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
