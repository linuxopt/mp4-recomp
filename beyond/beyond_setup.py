"""Metroid Prime 4 Beyond first-run setup, started by the game EXE (beyond_ui/fe_sys.cpp setup_start).

Checks the player's own game files (the supported build only), assembles the RomFS (base + update layered, then every file
checked against the manifest), translates the ExeFS to C with SwitchRecomp, applies the verified patches (if any), compiles
with the bundled GCC and links Metroid Prime 4 Beyond_code.dll next to the EXE.

Output protocol on stdout (one line each): '@@P <percent> <stage>|<detail>' progress, '@@E <message>' failure,
'@@D' done; everything else is log. Every stage leaves a marker so a stopped or failed setup continues where it was.

Usage: beyond_setup.py --out DIR --name "Metroid Prime 4 Beyond" --base DIR|FILE [--update DIR|FILE] [--keys prod.keys]

The game and the update can also be the player's .xci / .nsp files: the game EXE unpacks them first (--nx-extract,
runtime/nxextract.c) with the player's own prod.keys, into <name>_files next to it.

On Linux (linux/setup_game.sh) the same steps give Metroid Prime 4 Beyond_code.so, compiled with the system's gcc and with
linux/lx_compat.h force-included, so the patched (Windows) sources compile unchanged.
"""
import argparse, hashlib, json, os, shutil, struct, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
if not os.path.isfile(os.path.join(HERE, 'switchrecomp.py')):
    HERE = os.path.dirname(HERE)   # run from the repository's beyond/, not from the release's recompiler folder
sys.path.insert(0, HERE)
WIN = sys.platform.startswith('win')
LX_COMPAT = os.path.join(HERE, 'linux', 'lx_compat.h')
TITLE_ID = 0x010019A01E2F2000
BUILD_ID = '8275FBE1EC5191C279F3A943AD8524F4BAB087A3'
VERSION = 'RS15 (build 8275FBE1)'
PATCHES = os.path.join(HERE, 'beyond', 'beyond_patches.json')
MANIFEST = os.path.join(HERE, 'beyond', 'romfs_beyond.manifest')
FOV_HOOKS = os.path.join(HERE, 'beyond', 'fov_hooks.txt')
SETUP_FORMAT = 1


def P(pct, stage, detail=''):
    print('@@P %.1f %s|%s' % (pct, stage, detail), flush=True)


def log(s):
    print(s, flush=True)


class Fail(Exception):
    pass


# ------------------------------------------------------------------ .xci / .nsp files
def key_candidates():
    home = os.path.expanduser('~')
    if WIN:
        ad = os.environ.get('APPDATA', '')
        dirs = [os.path.join(home, '.switch')] + [os.path.join(ad, *p) for p in (('Ryujinx', 'system'), ('yuzu', 'keys'), ('suyu', 'keys'),
                                                                               ('sudachi', 'keys'), ('citron', 'keys'), ('eden', 'keys'))]
    else:
        cfg = os.environ.get('XDG_CONFIG_HOME') or os.path.join(home, '.config')
        dat = os.environ.get('XDG_DATA_HOME') or os.path.join(home, '.local', 'share')
        dirs = [os.path.join(home, '.switch'), os.path.join(cfg, 'Ryujinx', 'system')] + \
               [os.path.join(dat, e, 'keys') for e in ('yuzu', 'suyu', 'sudachi', 'citron', 'eden')]
    return dirs


def find_keys(given):
    """the player's prod.keys (and title.keys beside it, if any): --keys, SWR_KEYS, else where emulators keep them"""
    for p in (given, os.environ.get('SWR_KEYS', '')):
        if p:
            p = os.path.join(p, 'prod.keys') if os.path.isdir(p) else p
            if not os.path.isfile(p):
                raise Fail('The keys file does not exist: %s' % p)
            return p
    for d in key_candidates():
        if os.path.isfile(os.path.join(d, 'prod.keys')):
            return os.path.join(d, 'prod.keys')
    raise Fail('Your Switch keys (prod.keys) were not found. A .xci or .nsp is encrypted: dump the keys from your own Switch '
               '(Lockpick_RCM) and put prod.keys in %s. Or extract the game with an emulator and choose the folder instead.'
               % key_candidates()[0])


def unpack_files(a, out, name):
    """the game and/or update chosen as .xci / .nsp files: unpacked into <name>_files (exefs + romfs), which then counts
    as the game folder. An update's RomFS is a patch over the game's, so with an update file the game must be a file too,
    and the result is the updated game (no separate update folder). Returns that folder, or None (folders were chosen)"""
    base_file, upd_file = os.path.isfile(a.base), bool(a.update) and os.path.isfile(a.update)
    if not base_file and not upd_file:
        return None
    if upd_file and not base_file:
        raise Fail('An update file (.nsp) is a patch over the game: choose the game as a file (.xci or .nsp) too.')
    keys = find_keys(a.keys)
    tkeys = os.path.join(os.path.dirname(keys), 'title.keys')
    src = a.update if upd_file else a.base
    exe = os.environ.get('SWR_RUNTIME_EXE') or os.path.join(out, name + ('.exe' if WIN else ''))
    if not os.path.isfile(exe):
        raise Fail('The game program is missing next to the setup (%s): it unpacks .xci / .nsp files.' % exe)
    stage = os.path.join(out, name + '_files')
    stamp = json.dumps({'reader': 1, 'src': src, 'src_size': os.path.getsize(src), 'src_time': int(os.path.getmtime(src)),
                        'base': a.base if upd_file else '', 'base_size': os.path.getsize(a.base) if upd_file else 0})
    marker = os.path.join(stage, '.unpacked')
    if os.path.isfile(marker) and open(marker).read() == stamp:
        log('Game files: already unpacked from %s' % src)
        return stage
    need = os.path.getsize(a.base) + 3 * 2**30
    free = shutil.disk_usage(out).free
    if free < need:
        raise Fail('Not enough free space on the drive of %s to unpack the game: %.1f GB free, about %.0f GB needed.' % (out, free / 2**30, need / 2**30))
    shutil.rmtree(stage, ignore_errors=True); os.makedirs(stage)
    cmd = [exe, '--nx-extract', '--keys', keys, '--program', '%016X' % TITLE_ID, '--out', stage]
    if os.path.isfile(tkeys):
        cmd += ['--title-keys', tkeys]
    if upd_file:
        cmd += ['--base', a.base]
    cmd.append(src)
    log('Unpacking %s%s with the keys in %s' % (src, (' over ' + a.base) if upd_file else '', keys))
    import subprocess
    pr = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    err = None
    for ln in pr.stdout:
        ln = ln.rstrip('\n')
        if ln.startswith('@@P '):
            pct, _, rest = ln[4:].partition(' ')
            try:
                P(0.5 + float(pct) * 0.115, 'Unpacking the game file', rest.partition('|')[2])
            except ValueError:
                pass
        elif ln.startswith('@@E '):
            err = ln[4:]
        else:
            log(ln)
    if pr.wait() != 0 or err:
        raise Fail(err or 'Unpacking %s failed (exit code %d).' % (src, pr.returncode))
    with open(marker, 'w') as f:
        f.write(stamp)
    return stage


# ------------------------------------------------------------------ ExeFS
def nso_build_id(path):
    with open(path, 'rb') as f:
        h = f.read(0x60)
    if len(h) < 0x60 or h[:4] != b'NSO0':
        return None
    return h[0x40:0x54].hex().upper()


def npdm_program_id(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'META':
        return None
    i = d.find(b'ACI0')
    return struct.unpack_from('<Q', d, i + 0x10)[0] if i >= 0 else None


def _is_exefs(c):
    return os.path.isfile(os.path.join(c, 'main')) and os.path.isfile(os.path.join(c, 'main.npdm'))


def _is_romfs(c):
    # Prime 4's RomFS: Worlds/, UI/, Preload/ and the Kernel.pak / MaterialArchive.arc at its root
    return os.path.isdir(os.path.join(c, 'Worlds')) and (os.path.isfile(os.path.join(c, 'Kernel.pak')) or os.path.isdir(os.path.join(c, 'Preload')))


def _below(folder, test, depth=3):
    """folders up to `depth` levels below `folder` (breadth first, so the shallowest wins) that pass `test`.
    Emulator dumps nest the files (dump/<title id>/romfs, a folder per title, ...)."""
    level = [folder]
    for _ in range(depth):
        nxt = []
        for d in level:
            try:
                subs = sorted(e.path for e in os.scandir(d) if e.is_dir())
            except OSError:
                continue
            for c in subs:
                if test(c):
                    return c
            nxt += subs
        level = nxt[:400]
    return None


def exefs_in(folder):
    """the ExeFS inside a chosen folder: the folder itself, its exefs/ (any case), or one a few levels down"""
    if not folder:
        return None
    for c in (folder, os.path.join(folder, 'exefs'), os.path.join(folder, 'ExeFS')):
        if _is_exefs(c):
            return c
    return _below(folder, _is_exefs)


def romfs_in(folder):
    if not folder:
        return None
    for c in (os.path.join(folder, 'romfs'), os.path.join(folder, 'RomFS'), folder):
        if _is_romfs(c):
            return c
    return (_below(folder, lambda c: os.path.basename(c).lower() == 'romfs' and _is_romfs(c))
            or _below(folder, _is_romfs))


def pick_exefs(base, update):
    cands = [(update, 'update'), (base, 'game folder')] if update else [(base, 'game folder')]
    seen = []
    for folder, what in cands:
        ex = exefs_in(folder)
        if not ex:
            continue
        pid = npdm_program_id(os.path.join(ex, 'main.npdm'))
        bid = nso_build_id(os.path.join(ex, 'main'))
        log('ExeFS in the %s: %s (program %s, build %s)' % (what, ex, ('%016X' % pid) if pid else '?', bid or 'not an NSO'))
        if pid is not None and pid != TITLE_ID:
            raise Fail('The %s is not Metroid Prime 4 Beyond: its program ID is %016X (Metroid Prime 4 Beyond is 010019A01E2F2000). '
                       'This build compiles Metroid Prime 4 Beyond only.' % (what, pid))
        if bid == BUILD_ID:
            return ex
        seen.append((what, bid))
    if not seen:
        raise Fail('No ExeFS found (the files main and main.npdm, looked for up to 3 folders deep). A RomFS dump alone is not '
                   'enough: the game code is in the ExeFS, which is extracted separately. Extract both the ExeFS and the RomFS of '
                   'your copy with the latest update applied, then choose the folder that holds them.')
    what, bid = seen[0]
    raise Fail('The %s has Metroid Prime 4 Beyond, but not version %s (its main has build ID %s). '
               'Most likely this is the base game or another update: choose the update as well (its .nsp file, or its extracted folder), '
               'or extract the game with the update applied.'
               % (what, VERSION, (bid or 'unknown')[:16] + '...'))


EXEFS_FILES = {'main', 'main.npdm', 'rtld', 'sdk'} | {'subsdk%d' % i for i in range(10)}


def copy_exefs(exefs, dst):
    """only the ExeFS's own files: the folder can be shared with the RomFS and other dumps (a roms folder with
    .xci / .nsp files of several GB), which must not be copied"""
    n = 0
    for name in os.listdir(exefs):
        s = os.path.join(exefs, name)
        if name.lower() in EXEFS_FILES and os.path.isfile(s):
            shutil.copyfile(s, os.path.join(dst, name.lower()))
            n += 1
    log('ExeFS: %d files copied (%s)' % (n, ', '.join(sorted(f for f in os.listdir(dst)))))


# ------------------------------------------------------------------ RomFS
def load_manifest():
    rows = []
    with open(MANIFEST, 'r', encoding='utf-8') as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip():
                continue
            p, s, h = ln.rstrip('\n').split('\t')
            rows.append((p, int(s), h))
    return rows


def same_volume(a, b):
    if WIN:
        return os.path.splitdrive(os.path.abspath(a))[0].lower() == os.path.splitdrive(os.path.abspath(b))[0].lower()

    def dev(p):   # no drive letters: the device of the nearest folder that exists (the destination may not yet)
        p = os.path.abspath(p)
        while not os.path.exists(p):
            p = os.path.dirname(p)
        return os.stat(p).st_dev
    return dev(a) == dev(b)


def assemble_romfs(base_romfs, upd_romfs, dst, pct0, pct1):
    rows = load_manifest()
    mhash = hashlib.sha1(open(MANIFEST, 'rb').read()).hexdigest()
    marker = os.path.join(dst, '.romfs_ok')
    if os.path.isfile(marker) and open(marker).read().strip() == mhash:
        missing = [p for p, s, h in rows if os.path.getsize(os.path.join(dst, p)) != s] if all(os.path.isfile(os.path.join(dst, p)) for p, s, h in rows) else ['x']
        if not missing:
            log('RomFS already in place and verified earlier: kept')
            P(pct1, 'Game data', 'already verified')
            return
    if base_romfs and not upd_romfs and os.path.isdir(dst) and os.path.samefile(base_romfs, dst):
        # a rebuild after an update (the front end passes the game's own _data folder): check in place, never copy a
        # file onto itself
        log('RomFS: checking the installed files in place')
        bad = [p for p, s, h in rows if not os.path.isfile(os.path.join(dst, p)) or os.path.getsize(os.path.join(dst, p)) != s]
        if bad:
            raise Fail('%d installed RomFS files are missing or changed (for example %s). Run setup again with your game folder.'
                       % (len(bad), ', '.join(bad[:3])))
        with open(marker, 'w') as f:
            f.write(mhash)
        P(pct1, 'Game data', 'checked')
        return
    # choose a source for every file: the update's when it has it, else the base game's
    src = {}; missing = []; from_update = 0
    for p, s, h in rows:
        rel = p.replace('/', os.sep)
        u = os.path.join(upd_romfs, rel) if upd_romfs else None
        b = os.path.join(base_romfs, rel) if base_romfs else None
        if u and os.path.isfile(u):
            src[p] = u; from_update += 1
        elif b and os.path.isfile(b):
            src[p] = b
        else:
            missing.append(p)
    if missing:
        hint = ''
        if upd_romfs and not base_romfs:
            hint = ' An update RomFS on its own only has the changed files: also choose the base game folder.'
        elif base_romfs and not upd_romfs:
            hint = (' Files only the update adds are missing: choose the extracted update as "Update folder", or extract a RomFS with the '
                    'update applied (an emulator\'s "Extract RomFS" with the update installed does this).')
        raise Fail('The RomFS is incomplete: %d of %d files of version %s are missing (for example %s).%s'
                   % (len(missing), len(rows), VERSION, ', '.join(missing[:3]), hint))
    if upd_romfs:
        log('RomFS: %d files from the update, %d from the base game' % (from_update, len(rows) - from_update))
    total = sum(s for p, s, h in rows) or 1
    os.makedirs(dst, exist_ok=True)
    link = same_volume(next(iter(src.values())), dst)
    log('RomFS: %s %d files (%.2f GB) into %s, checking each against the %s file list'
        % ('hard-linking' if link else 'copying', len(rows), total / 2**30, dst, VERSION))
    done = 0; bad = []; t0 = time.time(); last = 0
    for p, s, h in rows:
        s_path = src[p]; d_path = os.path.join(dst, p.replace('/', os.sep))
        if os.path.getsize(s_path) != s:
            bad.append(p); continue
        sh = hashlib.sha1()
        os.makedirs(os.path.dirname(d_path), exist_ok=True)
        if link:
            with open(s_path, 'rb') as f:
                for blk in iter(lambda: f.read(1 << 20), b''):
                    sh.update(blk)
            if sh.hexdigest() == h:
                if os.path.lexists(d_path):
                    os.remove(d_path)
                try:
                    os.link(s_path, d_path)
                except OSError:
                    shutil.copyfile(s_path, d_path)
        else:
            tmp = d_path + '.part'
            with open(s_path, 'rb') as f, open(tmp, 'wb') as o:
                for blk in iter(lambda: f.read(1 << 20), b''):
                    sh.update(blk); o.write(blk)
            if sh.hexdigest() == h:
                os.replace(tmp, d_path)
            else:
                os.remove(tmp)
        if sh.hexdigest() != h:
            bad.append(p)
        done += s
        now = time.time()
        if now - last > 0.25:
            last = now
            P(pct0 + (pct1 - pct0) * done / total, 'Game data', '%.1f of %.1f GB checked' % (done / 2**30, total / 2**30))
    if bad:
        raise Fail('%d RomFS files are not the %s versions (for example %s). The RomFS may be modified, damaged, from another version, '
                   'or an update RomFS extracted without its base game (an unmerged patch). Extract it again with the update merged '
                   '(an emulator\'s "Extract RomFS" with the update installed does this).' % (len(bad), VERSION, ', '.join(bad[:3])))
    with open(marker, 'w') as f:
        f.write(mhash)
    log('RomFS: all %d files match version %s (%.0f s)' % (len(rows), VERSION, time.time() - t0))


# ------------------------------------------------------------------ code
def apply_patches(build):
    """beyond_patches.json, format 2 (beyond/make_patches.py): per generated file the SHA-256 of the codegen output it
    expects and of the result, and hunks [first_line, end_line, lines] replacing those lines. No generated code is stored:
    the codegen output is deterministic for the verified ExeFS, and the hashes check it before and after"""
    if not os.path.isfile(PATCHES):
        log('No game-code patches for this build yet: the code is compiled as translated')
        return
    with open(PATCHES, 'r', encoding='utf-8') as f:
        patches = json.load(f)
    if patches.get('format') != 2:
        raise Fail('beyond_patches.json is not in the expected format (2).')
    n = 0
    for name, spec in patches['files'].items():
        path = os.path.join(build, name)
        raw = open(path, 'rb').read().decode('latin-1')
        crlf = '\r\n' in raw
        text = raw.replace('\r\n', '\n')
        have = hashlib.sha256(text.encode('latin-1')).hexdigest()
        if have == spec['result']:
            continue   # already applied (a resumed setup)
        if have != spec['pristine']:
            raise Fail('The generated code does not match the verified %s build (%s). Delete the folder "%s" and run setup again.'
                       % (VERSION, name, build))
        lines = text.split('\n')
        for first, end, add in sorted(spec['hunks'], key=lambda h: h[0], reverse=True):
            lines[first:end] = add
            n += 1
        text = '\n'.join(lines)
        if hashlib.sha256(text.encode('latin-1')).hexdigest() != spec['result']:
            raise Fail('Patching %s did not give the verified %s result.' % (name, VERSION))
        if crlf:
            text = text.replace('\n', '\r\n')
        with open(path, 'wb') as f:
            f.write(text.encode('latin-1'))
    log('Applied %d verified Metroid Prime 4 Beyond %s patches' % (n, VERSION))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True); ap.add_argument('--name', default='Metroid Prime 4 Beyond')
    ap.add_argument('--base', required=True); ap.add_argument('--update', default='')
    ap.add_argument('--keys', default='')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    out, name = os.path.abspath(a.out), a.name
    data = os.path.join(out, name + '_data'); build = os.path.join(out, name + '_build')
    dll = os.path.join(out, name + ('_code.dll' if WIN else '_code.so'))
    log('Metroid Prime 4 Beyond setup (format %d) - SwitchRecomp by MZ Labs; use only your own copy of the game.' % SETUP_FORMAT)
    log('Game folder: %s' % a.base)
    if a.update:
        log('Update folder: %s' % a.update)
    try:
        P(0.5, 'Checking game files', 'ExeFS')
        unpacked = unpack_files(a, out, name)
        if unpacked:
            a.base, a.update = unpacked, ''
        if not os.path.isdir(a.base):
            raise Fail('The game folder does not exist: %s' % a.base)
        if a.update and not os.path.isdir(a.update):
            raise Fail('The update folder does not exist: %s' % a.update)
        exefs = pick_exefs(a.base, a.update)
        base_romfs = romfs_in(a.base)
        upd_romfs = romfs_in(a.update) if a.update else None
        if upd_romfs and base_romfs and os.path.samefile(upd_romfs, base_romfs):
            upd_romfs = None
        if not base_romfs and not upd_romfs:
            raise Fail('No RomFS found. The game folder should contain a "romfs" folder (with system, packs, textures...).')
        log('RomFS: base %s, update %s' % (base_romfs or '-', upd_romfs or '-'))
        free = shutil.disk_usage(out).free
        need = 3 * 2**30 if same_volume(base_romfs or upd_romfs, out) else 9 * 2**30
        if free < need:
            raise Fail('Not enough free space on the drive of %s: %.1f GB free, about %.0f GB needed.' % (out, free / 2**30, need / 2**30))
        P(2, 'Checking game files', 'Metroid Prime 4 Beyond %s' % VERSION)

        assemble_romfs(base_romfs, upd_romfs, os.path.join(data, 'romfs'), 12 if unpacked else 2, 22)

        ex_dst = os.path.join(data, 'exefs'); os.makedirs(ex_dst, exist_ok=True)
        if os.path.samefile(exefs, ex_dst):
            log('ExeFS: the installed copy is used (rebuild)')
        else:
            copy_exefs(exefs, ex_dst)
        P(23, 'Translating the game code', 'loading')

        import switchrecomp, nxloader, codegen   # the SwitchRecomp translator, bundled next to this script
        gen_marker = os.path.join(build, '.generated')
        gen_key = hashlib.sha1(open(os.path.join(ex_dst, 'main'), 'rb').read()[:0x100] + (open(PATCHES, 'rb').read() if os.path.isfile(PATCHES) else b'')
                               + open(os.path.join(HERE, 'codegen.py'), 'rb').read() + open(os.path.join(HERE, 'metroid_perf.py'), 'rb').read()
                               + open(os.path.join(HERE, 'logic_rate.py'), 'rb').read()
                               + open(os.path.join(HERE, 'mouse_aim.py'), 'rb').read()
                               # the instruction templates, and the switch that turns the second inline SIMD tier on
                               + open(os.path.join(HERE, 'a64.py'), 'rb').read()
                               + ('simd_inline=%s' % os.environ.get('SWR_SIMD_INLINE', '0')).encode()).hexdigest()
        if os.path.isfile(gen_marker) and open(gen_marker).read().strip() == gen_key:
            log('Generated code already in place: kept')
            files = sorted(os.path.join(build, f) for f in os.listdir(build) if f.startswith('code_') and f.endswith('.c'))
            files.insert(0, os.path.join(build, 'module.c'))
        else:
            # regenerating (new patches or translator after an update): remember each file's content, so a file that
            # comes out the same keeps its date and its compiled object is reused; only changed files are compiled
            before = {}
            if os.path.isdir(build):
                for f in os.listdir(build):
                    if f.endswith('.c'):
                        fp = os.path.join(build, f)
                        before[f] = (hashlib.sha1(open(fp, 'rb').read()).hexdigest(), os.stat(fp).st_mtime_ns)
            mods, npdm, _ = nxloader.load_program(data)
            for m in mods:
                log('  %-8s %010X  code %.1f MB' % (m.name, m.base, m.text[1] / 1048576.0))
            prog = codegen.Program(mods, [])   # no hints: the verified build was generated without them
            state = {'p': 23.0}

            def say(msg):
                log(msg)
                if 'C generated:' in msg:
                    try:
                        k, n = msg.split('C generated:')[1].split('chunks')[0].strip().split('/')
                        state['p'] = 26 + 4 * int(k) / int(n)
                    except ValueError:
                        pass
                    P(state['p'], 'Translating the game code', msg.strip())
                elif msg.startswith('Functions'):
                    P(25.5, 'Translating the game code', msg.strip())
            P(23.5, 'Translating the game code', 'analysing the game code')
            prog.analyze(say)
            os.makedirs(build, exist_ok=True)
            files = switchrecomp.gen_module(prog, mods, npdm, build, name, 30000, say)
            P(30, 'Applying Metroid Prime 4 Beyond fixes', '')
            apply_patches(build)
            kept = 0
            for f, (h, mt) in before.items():
                fp = os.path.join(build, f)
                if os.path.isfile(fp) and hashlib.sha1(open(fp, 'rb').read()).hexdigest() == h:
                    os.utime(fp, ns=(mt, mt)); kept += 1
            if before:
                log('Regenerated: %d of %d files unchanged (their compiled objects are reused)' % (kept, len(files)))
            with open(gen_marker, 'w') as f:
                f.write(gen_key)
        P(31, 'Compiling with GCC', '0 of %d files' % len(files))

        if WIN:
            # the bundled GCC, never one that merely happens to be on PATH; a build from source names its own with $CC
            cc = os.path.join(HERE, 'gcc', 'Library', 'bin', 'gcc.exe')
            cc = cc if os.path.isfile(cc) else None
            if not cc and os.environ.get('CC'):
                cc = shutil.which(os.environ['CC'])
                if not cc:
                    raise Fail('CC is set to %s, which was not found.' % os.environ['CC'])
            if not cc:
                raise Fail('The bundled GCC is missing (recompiler\\gcc). Extract the release again '
                           '(a build from source: set CC to a MinGW-w64 gcc.exe, for example MSYS2 UCRT64\'s).')
        else:
            # $CC, else the GCC bundled next to this script (the AppImage's recompiler/gcc), else the system's
            cc = shutil.which(os.environ['CC']) if os.environ.get('CC') else None
            for n in ('gcc', 'x86_64-buildroot-linux-gnu-gcc'):
                if not cc and os.path.isfile(os.path.join(HERE, 'gcc', 'bin', n)):
                    cc = os.path.join(HERE, 'gcc', 'bin', n)
            cc = cc or shutil.which('gcc')
            if not cc:
                raise Fail('gcc not found. Install it first (bash linux/install_deps.sh).')
            if not os.path.isfile(LX_COMPAT):
                raise Fail('linux/lx_compat.h is missing next to the recompiler.')
        objs = compile_all(files, cc, build, a.jobs, 31, 96)
        P(96, 'Linking', os.path.basename(dll))
        switchrecomp.check_writable(dll)
        switchrecomp.link_module(objs, cc, dll, WIN, log)
        if not os.path.isfile(dll) or os.path.getsize(dll) < 50 * 2**20:
            raise Fail('Linking produced no usable game code.')
        if os.path.isfile(FOV_HOOKS):   # the [Gameplay] fov setting's hook list (runtime/cheats.c)
            shutil.copyfile(FOV_HOOKS, os.path.join(data, 'fov_hooks.txt'))
        os.makedirs(os.path.join(data, 'cheats'), exist_ok=True)   # Atmosphere-format cheat files go here
        with open(os.path.join(data, 'setup.ok'), 'w') as f:
            json.dump({'game': 'Metroid Prime 4 Beyond', 'title_id': '%016X' % TITLE_ID, 'version': VERSION, 'build_id': BUILD_ID,
                       'format': SETUP_FORMAT, 'date': time.strftime('%Y-%m-%d %H:%M:%S'),
                       'dll_sha256': hashlib.sha256(open(dll, 'rb').read()).hexdigest()}, f, indent=1)
        if unpacked:   # the RomFS files are hard links (or copies) in the game's data now: the unpacked ones can go
            shutil.rmtree(unpacked, ignore_errors=True)
        P(100, 'Done', '')
        log('Setup complete.')
        print('@@D', flush=True)
    except Fail as e:
        print('@@E ' + str(e), flush=True); sys.exit(2)
    except SystemExit as e:   # switchrecomp reports compiler and linker errors this way
        if e.code not in (None, 0):
            msg = str(e.code)
            log(msg)
            first = msg.strip().splitlines()[0] if msg.strip() else 'unknown error'
            print('@@E ' + ('GCC stopped: %s (full message in the log)' % first[:300]), flush=True)
            sys.exit(3)
        raise
    except Exception as e:
        import traceback; traceback.print_exc(file=sys.stdout)
        print('@@E Unexpected error: %s' % e, flush=True); sys.exit(4)


def compile_all(files, cc, build, jobs, p0, p1):
    """switchrecomp.compile_all with a progress line per file (same flags and object names, so objects are reused)"""
    import switchrecomp
    flags = ['-O2', '-fno-strict-aliasing', '-fwrapv', '-w', '-I', switchrecomp.RT, '-foptimize-sibling-calls', '-fno-semantic-interposition']
    if not WIN:
        flags += ['-fPIC', '-fvisibility=hidden', '-include', LX_COMPAT]
        if os.environ.get('SWR_CFLAGS'):
            flags += os.environ['SWR_CFLAGS'].split()   # e.g. -march=native, or -march=znver2 for a Steam Deck
    with open(os.path.join(switchrecomp.RT, 'cpu.h'), 'rb') as f1, open(os.path.join(switchrecomp.RT, 'cpu_helpers.c'), 'rb') as f2:
        key = '|'.join([cc] + flags) + '|' + f1.read().decode('latin1') + '|' + f2.read().decode('latin1')
    sig = hashlib.sha1(key.encode('utf-8', 'replace')).hexdigest()[:10]
    # biggest first: a big file started last would leave every other core idle at the end
    order = sorted(files, key=lambda s: -os.path.getsize(s))
    sizes = {s: os.path.getsize(s) for s in files}; total = sum(sizes.values()) or 1
    lock = threading.Lock(); st = {'n': 0, 'bytes': 0, 'reused': 0}; t0 = time.time()
    errors = []

    def one(src):
        if errors:
            return None
        obj = os.path.splitext(src)[0] + '.' + sig + '.o'
        if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
            with lock:
                st['reused'] += 1
        else:
            tmp = obj + '.tmp'
            err = switchrecomp.run_cc([cc] + flags + ['-c', src, '-o', tmp], cc)
            if err is None:
                os.replace(tmp, obj)
            else:
                if os.path.exists(tmp):
                    os.remove(tmp)
                errors.append((src, err)); return None
        with lock:
            st['n'] += 1; st['bytes'] += sizes[src]
            n = st['n']
            P(p0 + (p1 - p0) * st['bytes'] / total, 'Compiling with GCC',
              '%d of %d files%s' % (n, len(files), (', %d reused from before' % st['reused']) if st['reused'] else ''))
            log('  compiled %s (%d/%d, %.0f s)' % (os.path.basename(src), n, len(files), time.time() - t0))
        return obj
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(one, order))
    if errors:
        src, err = errors[0]
        raise SystemExit('Compilation of %s failed:\n%s' % (os.path.basename(src), err[-3000:]))
    return [os.path.splitext(s)[0] + '.' + sig + '.o' for s in files]


if __name__ == '__main__':
    main()
