#!/usr/bin/env python3
"""TOTK VR Recomp installer: a window that takes your own extracted game and builds the VR version from it.

For a source checkout (the upstream installer, installer/, is for the release package and NSP inputs). It
  1. checks the toolchain (MSYS2 UCRT64 packages and the OpenXR loader, uv and the two Python environments, the
     .NET 10 SDK, Visual Studio's C++ build tools) and installs what is missing (winget, pacman, uv; it asks first);
  2. imports your ExeFS and RomFS folders (tools/import_dump.py: checks the ExeFS against the supported versions, packs
     the RomFS), or extracts NSPs with your keys (tools/extract.py, set in config.local.toml);
  3. builds the shader translator, lifts the game code, writes the RomFS overlay, builds totk.exe with the VR mode
     and prepares the shaders;
  4. writes "Play TOTK VR.bat" with the VR options you chose, and a desktop shortcut to it.

Standard library only (tkinter). Start it with "Install TOTK VR.bat" at the repository's root.
"""
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

ROOT = Path(__file__).resolve().parents[2]
MSYS = Path(r'C:\msys64')
UCRT = MSYS / 'ucrt64' / 'bin'
EXEFS_FILES = ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0')
PACKAGES = ('clang', 'lld', 'ninja', 'gcc', 'make', 'pkgconf', 'sdl3', 'opus', 'libvpx', 'zstd', 'vulkan-devel',
            'vulkan-memory-allocator', 'shaderc', 'spirv-tools', 'openxr-sdk')
VSWHERE = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe')
NOWIN = 0x08000000

T = {
    'title': ('TOTK VR Recomp - installer', 'TOTK VR Recomp - instalador'),
    'intro': ('Your own Tears of the Kingdom (1.2.1 or 1.4.3), extracted from your own console (docs/vr-setup.md). '
              'Choose the ExeFS and RomFS folders (they may be the same folder), then Install.',
              'Tu propio Tears of the Kingdom (1.2.1 o 1.4.3), extraído de tu propia consola (docs/vr-setup.md). '
              'Elegí las carpetas ExeFS y RomFS (pueden ser la misma) y apretá Instalar.'),
    'exefs': ('ExeFS folder (main, main.npdm, rtld, sdk, subsdk0)', 'Carpeta ExeFS (main, main.npdm, rtld, sdk, subsdk0)'),
    'romfs': ('RomFS folder (base + update, merged)', 'Carpeta RomFS (base + update combinados)'),
    'browse': ('Browse...', 'Elegir...'),
    'shaders': ('Prepare the shaders (less stutter at first; ~20 min)', 'Preparar shaders (menos tirones al principio; ~20 min)'),
    'vr': ('VR options', 'Opciones VR'),
    'stereo': ('Stereo (3D)', 'Estéreo (3D)'), 'turn': ('Turning', 'Giro'),
    'motion': ('Motion combat', 'Combate con movimiento'),
    'install': ('Install', 'Instalar'), 'play': ('Play', 'Jugar'), 'close': ('Close', 'Cerrar'),
    'tools_missing': ('Missing: {}.\n\nInstall them now? (winget / pacman / uv; several GB, it can take a while; '
                      'Visual Studio asks for administrator rights)',
                      'Falta: {}.\n\n¿Instalarlo ahora? (winget / pacman / uv; varios GB, puede tardar; '
                      'Visual Studio pide permisos de administrador)'),
    'done': ('Done. Start the headset\'s OpenXR runtime (Virtual Desktop: VDXR), then Play.',
             'Listo. Abrí el runtime OpenXR del visor (Virtual Desktop: VDXR) y apretá Jugar.'),
    'failed': ('Failed: {}. The log above says why.', 'Falló: {}. El registro de arriba dice por qué.'),
}
LANG = 0
try:
    import locale
    if (locale.getlocale()[0] or '').lower().startswith(('es', 'spanish')):
        LANG = 1
except Exception:
    pass


def t(key):
    return T[key][LANG]


def check_exefs(d):
    d = Path(d)
    missing = [f for f in EXEFS_FILES if not (d / f).is_file()]
    if missing:
        return 'ExeFS: {} missing in {}'.format(', '.join(missing), d)
    if (d / 'main').read_bytes()[:4] != b'NSO0':
        return 'ExeFS: main is not an NSO (still encrypted?)'
    return None


def check_romfs(d):
    d = Path(d)
    if not ((d / 'Pack').is_dir() and (d / 'System').is_dir()):
        return 'RomFS: Pack/ and System/ not found in {} (is it the RomFS root?)'.format(d)
    return None


def vcvars():
    if not VSWHERE.is_file():
        return None
    r = subprocess.run([str(VSWHERE), '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                        '-latest', '-property', 'installationPath'], capture_output=True, text=True)
    p = Path(r.stdout.strip()) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat' if r.stdout.strip() else None
    return p if p and p.is_file() else None


def dotnet10():
    r = subprocess.run(['dotnet', '--list-sdks'], capture_output=True, text=True, shell=True)
    return any(l.startswith('10.') for l in r.stdout.splitlines())


def missing_tools():
    miss = []
    if not (UCRT / 'clang.exe').is_file():
        miss.append('MSYS2 UCRT64')
    elif not (UCRT / 'libopenxr_loader.dll').is_file() or not (UCRT / 'ninja.exe').is_file():
        miss.append('MSYS2 packages')
    if not shutil.which('uv'):
        miss.append('uv')
    if not (ROOT / '.venv' / 'Scripts' / 'python.exe').is_file() or not (ROOT / '.venv-py314' / 'Scripts' / 'python.exe').is_file():
        miss.append('Python environments')
    if not dotnet10():
        miss.append('.NET 10 SDK')
    if not vcvars():
        miss.append('Visual Studio C++ build tools')
    return miss


def write_launcher(ver, opts):
    bat = ROOT / 'Play TOTK VR.bat'
    turn = opts['turn']
    lines = ['@echo off', 'rem Written by tools/vr/installer.py: TOTK VR with the options chosen there.',
             'cd /d "%~dp0build\\{}"'.format(ver), 'set PATH=C:\\msys64\\ucrt64\\bin;%PATH%', 'set TOTK_VR=1',
             'set TOTK_FPS_LIMIT=240', 'set TOTK_VR_STEREO={}'.format(1 if opts['stereo'] else 0),
             'set TOTK_VR_TURN={}'.format('snap' if turn.startswith('snap') else 'smooth'),
             'set TOTK_VR_SNAP_DEGREES={}'.format(turn[4:] if turn.startswith('snap') else 45),
             'set TOTK_VR_MOTION={}'.format(1 if opts['motion'] else 0),
             'totk.exe',
             'if errorlevel 1 (echo. & echo The game ended with an error: see logs\\totk.log, the "vr:" lines. & pause)']
    bat.write_text('\r\n'.join(lines) + '\r\n')
    return bat


def desktop_shortcut(target, name):
    ps = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")+"\\{}.lnk");'
          '$s.TargetPath="{}";$s.WorkingDirectory="{}";$s.IconLocation="{}";$s.Save()').format(
        name, target, ROOT, ROOT / 'runtime' / 'totk.ico')
    subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True)


class App:
    def __init__(self, root):
        self.root = root
        root.title(t('title'))
        root.geometry('900x640')
        self.q = queue.Queue()
        self.busy = False
        f = ttk.Frame(root, padding=10)
        f.pack(fill='both', expand=True)
        ttk.Label(f, text=t('intro'), wraplength=860).pack(anchor='w', pady=(0, 8))
        self.paths = {}
        for key in ('exefs', 'romfs'):
            row = ttk.Frame(f)
            row.pack(fill='x', pady=2)
            ttk.Label(row, text=t(key), width=48).pack(side='left')
            v = tk.StringVar()
            self.paths[key] = v
            ttk.Entry(row, textvariable=v).pack(side='left', fill='x', expand=True, padx=4)
            ttk.Button(row, text=t('browse'), command=lambda k=key: self.browse(k)).pack(side='left')
        self.shaders = tk.BooleanVar(value=True)
        ttk.Checkbutton(f, text=t('shaders'), variable=self.shaders).pack(anchor='w', pady=4)
        box = ttk.LabelFrame(f, text=t('vr'), padding=6)
        box.pack(fill='x', pady=4)
        self.stereo = tk.BooleanVar(value=True)
        self.motion = tk.BooleanVar(value=True)
        ttk.Checkbutton(box, text=t('stereo'), variable=self.stereo).grid(row=0, column=0, sticky='w')
        ttk.Checkbutton(box, text=t('motion'), variable=self.motion).grid(row=0, column=1, sticky='w', padx=12)
        ttk.Label(box, text=t('turn')).grid(row=1, column=0, sticky='w')
        self.turn = tk.StringVar(value='smooth')
        ttk.Combobox(box, textvariable=self.turn, width=12, values=['smooth', 'snap30', 'snap45', 'snap60']).grid(
            row=1, column=1, sticky='w', padx=12)
        self.log_box = tk.Text(f, height=16, bg='#111', fg='#ddd', font=('Consolas', 9))
        self.log_box.pack(fill='both', expand=True, pady=6)
        self.bar = ttk.Progressbar(f, mode='indeterminate')
        self.bar.pack(fill='x')
        btns = ttk.Frame(f)
        btns.pack(fill='x', pady=6)
        self.b_install = ttk.Button(btns, text=t('install'), command=self.install)
        self.b_install.pack(side='left')
        self.b_play = ttk.Button(btns, text=t('play'), command=self.play)
        self.b_play.pack(side='left', padx=6)
        ttk.Button(btns, text=t('close'), command=root.destroy).pack(side='right')
        if not (ROOT / 'Play TOTK VR.bat').is_file():
            self.b_play.state(['disabled'])
        root.after(100, self.pump)

    def browse(self, key):
        d = filedialog.askdirectory(title=t(key), initialdir=self.paths[key].get() or str(Path.home()))
        if d:
            self.paths[key].set(d)
            other = 'romfs' if key == 'exefs' else 'exefs'
            if not self.paths[other].get():
                if other == 'romfs' and not check_romfs(d):
                    self.paths[other].set(d)
                for cand in (Path(d).parent / other, Path(d) / other):
                    if cand.is_dir():
                        self.paths[other].set(str(cand))

    def log(self, s):
        self.q.put(s)

    def pump(self):
        while not self.q.empty():
            s = self.q.get()
            if s is None:
                self.bar.stop()
                self.busy = False
                self.b_install.state(['!disabled'])
                if (ROOT / 'Play TOTK VR.bat').is_file():
                    self.b_play.state(['!disabled'])
                continue
            self.log_box.insert('end', s + '\n')
            self.log_box.see('end')
        self.root.after(100, self.pump)

    def run(self, cmd, env=None, cwd=ROOT, shell=False):
        self.log('$ ' + (cmd if isinstance(cmd, str) else ' '.join(map(str, cmd))))
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             errors='replace', creationflags=NOWIN, shell=shell)
        out = []
        for line in p.stdout:
            line = line.rstrip()
            out.append(line)
            if not line.startswith('[') or len(out) % 20 == 0:   # (ninja's per-file lines: every 20th)
                self.log('  ' + line)
        if p.wait():
            raise RuntimeError('{} exited with {}'.format(cmd if isinstance(cmd, str) else Path(str(cmd[0])).name,
                                                          p.returncode))
        return '\n'.join(out)

    def install(self):
        if self.busy:
            return
        p = {k: v.get().strip() for k, v in self.paths.items()}
        errs = [e for e in (check_exefs(p['exefs']) if p['exefs'] else 'ExeFS: choose the folder',
                            check_romfs(p['romfs']) if p['romfs'] else 'RomFS: choose the folder') if e]
        if errs:
            messagebox.showerror(t('title'), '\n'.join(errs))
            return
        miss = missing_tools()
        if miss and not messagebox.askyesno(t('title'), t('tools_missing').format(', '.join(miss))):
            return
        opts = {'stereo': self.stereo.get(), 'motion': self.motion.get(), 'turn': self.turn.get()}
        self.busy = True
        self.b_install.state(['disabled'])
        self.bar.start(12)
        threading.Thread(target=self.work, args=(p, miss, opts, self.shaders.get()), daemon=True).start()

    def tools(self, miss):
        w = ['winget', 'install', '-e', '--silent', '--accept-package-agreements', '--accept-source-agreements', '--id']
        if 'MSYS2 UCRT64' in miss:
            self.run(w + ['MSYS2.MSYS2'])
            for _ in range(2):
                self.run([str(MSYS / 'usr' / 'bin' / 'bash.exe'), '-lc', 'pacman -Syuu --noconfirm'])
        if 'MSYS2 UCRT64' in miss or 'MSYS2 packages' in miss:
            pk = ' '.join('mingw-w64-ucrt-x86_64-' + n for n in PACKAGES)
            self.run([str(MSYS / 'usr' / 'bin' / 'bash.exe'), '-lc', 'pacman -S --needed --noconfirm ' + pk])
        if 'uv' in miss:
            self.run(w + ['astral-sh.uv'])
        uv = shutil.which('uv') or str(Path(os.environ['LOCALAPPDATA']) / 'Microsoft' / 'WinGet' / 'Links' / 'uv.exe')
        if 'Python environments' in miss or 'uv' in miss:
            for venv, py, req in (('.venv', '3.12', 'requirements.txt'), ('.venv-py314', '3.14', 'requirements-py314.txt')):
                if not (ROOT / venv / 'Scripts' / 'python.exe').is_file():
                    self.run([uv, 'venv', '--python', py, venv])
                self.run([uv, 'pip', 'install', '--python', str(ROOT / venv / 'Scripts' / 'python.exe'), '-r',
                          str(ROOT / 'tools' / req)])
        if '.NET 10 SDK' in miss:
            self.run(w + ['Microsoft.DotNet.SDK.10'])
        if 'Visual Studio C++ build tools' in miss:
            self.run(w + ['Microsoft.VisualStudio.2022.BuildTools', '--override',
                          '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'])

    def work(self, p, miss, opts, shaders):
        try:
            if miss:
                self.log('== Installing the tools: ' + ', '.join(miss))
                self.tools(miss)
            py, py14 = str(ROOT / '.venv' / 'Scripts' / 'python.exe'), str(ROOT / '.venv-py314' / 'Scripts' / 'python.exe')
            env = dict(os.environ)
            env['PATH'] = '{};{}'.format(UCRT, env.get('PATH', ''))
            self.log('== Your game files -> build/extracted/')
            args = [py, 'tools/import_dump.py', '--exefs', p['exefs'], '--romfs', p['romfs']]
            if Path(p['exefs']).resolve() == Path(p['romfs']).resolve():
                for d in Path(p['romfs']).iterdir():   # (a folder with both: what is not the game's is left out)
                    if d.is_dir() and d.name not in ('AI', 'Actor', 'Pack', 'System') and \
                            ((d / '.git').exists() or (d / 'tools' / 'import_dump.py').is_file()):
                        args += ['--exclude', d.name]
            out = self.run(args, env=env)
            m = re.search(r'extracted[\\/](v\d+)', out)
            if not m:
                raise RuntimeError('import_dump did not say the version')
            ver = m.group(1)
            self.log('== Game version {}'.format(ver))
            if not (ROOT / 'build' / 'xlate' / 'totk_xlate.exe').is_file():
                self.log('== The shader translator (tools/build_xlate.py)')
                # (a clean PATH: a stray quote in the user's PATH breaks vcvars64.bat, and MSYS's link.exe must not
                # shadow the MSVC linker)
                clean = ';'.join([r'C:\Windows\system32', r'C:\Windows', r'C:\Windows\System32\Wbem',
                                  r'C:\Windows\System32\WindowsPowerShell\v1.0', r'C:\Program Files\dotnet',
                                  str(VSWHERE.parent), str(Path(shutil.which('git') or r'C:\Program Files\Git\cmd\git.exe').parent)])
                cmd = 'set "PATH={}" && call "{}" >nul && "{}" tools\\build_xlate.py'.format(clean, vcvars(), py)
                self.run(cmd, shell=True)
            self.log('== Lifting the game code (recomp/gen.py)')
            self.run([py, 'recomp/gen.py', '--exefs', 'build/extracted/{}/exefs'.format(ver), '--out',
                      'generated/' + ver, '--patches', 'recomp/patches-{}.txt'.format(ver)], env=env)
            self.log('== The RomFS overlay (tools/make_romfs_patches.py)')
            self.run([py14, 'tools/make_romfs_patches.py', '--version', ver], env=env)
            self.log('== Building totk.exe with the VR mode (tools/build_game.py --vr: 5-15 minutes)')
            self.run([py, 'tools/build_game.py', '--version', ver, '--vr'], env=env)
            if shaders:
                self.log('== Preparing the shaders (tools/prepare_shaders.py)')
                try:
                    if not (ROOT / 'refs' / 'MacNativeGames').is_dir():
                        bash = shutil.which('bash') or r'C:\Program Files\Git\bin\bash.exe'
                        self.run([bash, 'tools/fetch_refs.sh'], env=env)
                    self.run([py14, 'tools/prepare_shaders.py', 'build/' + ver, '--version', ver], env=env)
                except Exception as e:
                    self.log('  (shaders not prepared: {}; the game compiles them while it plays)'.format(e))
            bat = write_launcher(ver, opts)
            desktop_shortcut(bat, 'TOTK VR')
            self.log('== ' + t('done'))
            self.log('   {} (+ desktop shortcut "TOTK VR")'.format(bat))
        except Exception as e:
            self.log('== ' + t('failed').format(e))
        self.log(None)

    def play(self):
        subprocess.Popen(['cmd', '/c', str(ROOT / 'Play TOTK VR.bat')], cwd=ROOT, creationflags=0x00000010)


def main():
    if sys.platform != 'win32':
        print('This installer is for Windows; on Linux follow docs/building.md and docs/vr.md.')
        return 1
    root = tk.Tk()
    App(root)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
