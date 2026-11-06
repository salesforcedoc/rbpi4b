#!/usr/bin/env python3
"""Build a pinned optional toolkit bundle; never run the stock-RX3 installer."""
import argparse
import importlib.util
import json
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('toolkit', ROOT / 'scripts/device/rx3-toolkit.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def patches(path):
    entries = []
    for offset, before, after, name in re.findall(
            r"^register_patch (\d+) '([^']+)' '([^']+)' (\S+)", path.read_text(), re.M):
        def decode(value):
            if not re.fullmatch(r'(\\[0-7]{3}){4}', value):
                raise ValueError('unsupported patch encoding')
            return bytes(int(n, 8) for n in re.findall(r'\\([0-7]{3})', value)).hex()
        entries.append(dict(offset=int(offset), before=decode(before), after=decode(after), name=name))
    if not entries:
        raise ValueError('patch table missing: ' + str(path))
    return entries


def check_hooks(source, player):
    """Check extracted native hook guards against our player."""
    files = [path for path in sorted((source / 'mod/modules').rglob('*'))
             if path.suffix in ('.h', '.c')]
    text = '\n'.join(path.read_text() for path in files)
    addresses = dict(re.findall(r'#define\s+(\w+)\s+(?:\(\(unsigned long\))?(0x[0-9a-fA-F]+)', text))
    sites = {}
    for path in files:
        unit = path.read_text()
        for call in re.finditer(r'(?:(?:install_(?:pc_ldr_)?hook|acquire)\(\s*&[\w\[\]]+,\s*|claim\(\s*&\w+,\s*owner,\s*\(void\s*\*\)client,\s*)(\w+),\s*(\w+),', unit):
            address, name = call.groups()
            declarations = list(re.finditer(
                r'uint8_t\s+' + re.escape(name) + r'\[(?:8)?\]\s*=\s*\{([^}]+)\}', unit[:call.start()]))
            if declarations:
                contents = declarations[-1].group(1)
            else:
                matches = re.findall(r'uint8_t\s+' + re.escape(name) +
                                     r'\[8\]\s*=\s*\{([^}]+)\}', text)
                if len(set(matches)) != 1:
                    continue
                contents = matches[0]
            values = re.findall(r'0x[0-9a-fA-F]+', contents)
            if len(values) != 8:
                continue
            address = addresses.get(address, address)
            if not re.fullmatch(r'0x[0-9a-fA-F]+[uUlL]*', address):
                continue
            address = int(address.rstrip('uUlL'), 16)
            sites.setdefault(address, set()).add(bytes(int(v, 16) for v in values))
    if len(sites) < 43:
        raise ValueError('incomplete native hook guard inventory')
    for address, alternatives in sites.items():
        if player[address - 0x8000:address - 0x8000 + 8] not in alternatives:
            raise ValueError('native hook guard mismatch at 0x%x' % address)
    return sorted(sites)


def pi_core_source(source):
    """Adapt only process recognition; preserve all upstream hook guards."""
    text = (source / 'mod/modules/core/rx3_core_hook.c').read_text()
    expected = '''static int is_player_process(void)
{
    char name[16];
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return 0;
    int length = read(fd, name, sizeof(name));
    close(fd);
    return length == 4 && !memcmp(name, "rbp\\n", 4u);
}'''
    replacement = '''#include "pi_process.h"
static int is_player_process(void)
{
    char args[512];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    int length = read(fd, args, sizeof(args));
    close(fd);
    return length > 0 && rx3_pi_player_cmdline(args, (unsigned int)length,
        getenv("RX3_PI_PLAYER"), getenv("RX3_PI_LOADER"));
}'''
    if text.count(expected) != 1:
        raise ValueError('upstream process guard changed; Pi adapter not applied')
    return text.replace(expected, replacement, 1)


def build(source, output, stock, compiler, rootfs):
    source, output = source.resolve(), output.resolve()
    if run('git', '-C', str(source), 'rev-parse', 'HEAD') != runtime.REVISION:
        raise ValueError('checkout must be pinned to ' + runtime.REVISION)
    if run('git', '-C', str(source), 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError('toolkit checkout has modified tracked files')
    if hashlib.sha1(stock.read_bytes()).hexdigest() != '3513d34b7ff7ae7b29e3041c6f44116ff2741f90':
        raise ValueError('stock player must be supported firmware 1.20')
    flags = ('-march=armv7-a -marm -mfloat-abi=softfp -mfpu=neon -fPIC '
             '-fno-stack-protector -fno-builtin-memcmp -fno-builtin-bcmp '
             '-fvisibility=hidden -O2 -Wall -Wextra -Werror -Wno-error=unused-variable')
    # Include the complete runtime; each module remains independently opt-in.
    # Keep module order consistent with the pinned upstream composition root.
    modules = source / 'mod/modules'
    core = json.loads((modules / 'core/manifest.json').read_text())
    composition = source / 'build/rx3_pi_composition.c'
    names = ('logo', 'theme', 'stems', 'samples', 'key_match', 'browse_columns',
             'keyshift', 'search', 'now_playing', 'stemwave', 'asshole')
    composition_text = ('/* SPDX-License-Identifier: MPL-2.0 */\n'
                        '#include "../mod/modules/core/api/rx3_module_api.h"\n')
    composition_text += ''.join('extern const struct rx3_module rx3_%s_module;\n' % n for n in names)
    composition_text += 'const struct rx3_module *const rx3_bundle[] = {\n'
    composition_text += ''.join('    &rx3_%s_module,\n' % n for n in names)
    composition_text += '};\nconst unsigned int rx3_bundle_count = sizeof(rx3_bundle)/sizeof(rx3_bundle[0]);\n'
    runtime.atomic_write(composition, composition_text.encode())
    units = ['mod/modules/' + p for p in core['arm_hook']['sources']
             if p != 'core/runtime/rx3_composition.c']
    # Replace USB-link-only broadcast with a validated configurable Pi address.
    feature = modules / 'now-playing/rx3_now_playing_feature.h'
    old = 'to.address = NOW_PLAYING_BROADCAST;'
    if feature.read_text().count(old) != 1:
        raise ValueError('upstream Now Playing destination changed')
    adapted_feature = source / 'build/rx3_now_playing_feature.pi.h'
    runtime.atomic_write(adapted_feature, ('#include "pi_network.h"\n' +
        feature.read_text().replace(old,
            'to.address = rx3_pi_ipv4(getenv("RX3_PI_NOW_PLAYING_HOST"));')).encode())
    module = modules / 'now-playing/rx3_now_playing_module.c'
    adapted_module = source / 'build/rx3_now_playing_module.pi.c'
    runtime.atomic_write(adapted_module, module.read_text().replace(
        '"rx3_now_playing_feature.h"', '"rx3_now_playing_feature.pi.h"').encode())
    input_source = modules / 'core/services/rx3_input.c'
    input_text = input_source.read_text()
    marker = 'static void enter(struct shared_hook *hook)'
    consumed = '    int consumed = 0;'
    if input_text.count(marker) != 1 or input_text.count(consumed) != 1:
        raise ValueError('upstream input broker changed; adapter not applied')
    input_text = input_text.replace(marker, '#include "pi_input.inc"\n' + marker, 1)
    input_text = input_text.replace(consumed,
        '    int consumed = rx3_pi_pad_control(key, operation, channel);', 1)
    adapted_input = source / 'build/rx3_input.pi.c'
    runtime.atomic_write(adapted_input, input_text.encode())
    # The stock toolkit points all tracks at one directory. On the Pi, derive
    # each track's own USB mount path, with an explicit local-directory override.
    stems_source = modules / 'stems/rx3_stems_module.c'
    stems_text = stems_source.read_text()
    start = stems_text.index('static int path_in_stems(')
    end = stems_text.index('#include "rx3_stems_panel.h"', start)
    stems_text = stems_text[:start] + '''#include "pi_stems.h"
static int stem_path_for_track(const void *track_info, char *output, size_t capacity)
{
    return rx3_pi_stem_path((const char *)track_info, stems_dir, output, capacity);
}

''' + stems_text[end:]
    adapted_stems = source / 'build/rx3_stems_module.pi.c'
    runtime.atomic_write(adapted_stems, stems_text.encode())
    units.remove('mod/modules/stems/rx3_stems_module.c')
    units.append(str(adapted_stems))
    units.remove('mod/modules/core/services/rx3_input.c')
    units.append(str(adapted_input))
    units.remove('mod/modules/now-playing/rx3_now_playing_module.c')
    units.extend((str(composition), str(adapted_module)))
    adapted = source / 'build/rx3_core_hook.pi.c'
    runtime.atomic_write(adapted, pi_core_source(source).encode())
    library = source / 'build/librx3_core.so'
    run(compiler, *flags.split(), '-shared', '-nostdlib', '-Wl,--hash-style=sysv',
        '-Wl,--build-id=none', '-I' + str(modules / 'core'), '-I' + str(modules / 'core/services'),
        '-I' + str(modules / 'now-playing'), '-I' + str(modules / 'stems'),
        '-I' + str(ROOT / 'scripts/toolkit'), '-o', str(library), str(adapted),
        *units, cwd=source)
    # GCC introduces signed division helpers in addition to Clang's imports.
    # Check the actual RX3 dependency libraries, not a host-glibc allowlist.
    nm = compiler.replace('gcc', 'nm')
    undefined = {line.split()[-1].split('@')[0] for line in run(nm, '-u', str(library)).splitlines()}
    available = set()
    needed = re.findall(r'Shared library: \[([^]]+)\]',
                        run(compiler.replace('gcc', 'readelf'), '-d', str(stock)))
    dependencies = [stock]
    for name in needed:
        matches = [rootfs / directory / name for directory in ('lib', 'usr/lib')
                   if (rootfs / directory / name).is_file()]
        if not matches:
            raise ValueError('missing RX3 dependency: ' + name)
        dependencies.append(matches[0])
    for path in dependencies:
        result = subprocess.run([nm, '-D', '--defined-only', str(path)], capture_output=True, text=True)
        if result.returncode == 0:
            available.update(line.split()[-1].split('@')[0] for line in result.stdout.splitlines() if line.split())
    if undefined - available:
        raise ValueError('unresolved RX3 runtime imports: ' + ', '.join(sorted(undefined - available)))
    patchsets = {'beatjump32': patches(modules / 'beatjump-32bars/module.sh'),
                 'keyshift': patches(modules / 'core/module.sh'),
                 'immediate': patches(modules / 'beatjump-no-quantize/module.sh')}
    # Rebuild this port's canonical baseline variants. Different display depths
    # are allowed; unknown edits or a foreign firmware fail closed at launch.
    hashes = []
    hook_sites = []
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        run('python3', str(ROOT / 'tools/patch-rbp/rbp_patch.py'), str(stock), '-o', str(temp / 'audio'))
        run('python3', str(ROOT / 'scripts/patch-rbp-nopc.py'), str(temp / 'audio'), '-o', str(temp / 'base'))
        for depth in (16, 32):
            player = temp / ('base%d' % depth)
            run('python3', str(ROOT / 'scripts/patch-rbp-depth.py'), str(temp / 'base'), '--bpp', str(depth), '-o', str(player))
            data = player.read_bytes()
            patched = runtime.guarded_patch(data, patchsets['beatjump32'] + patchsets['keyshift'] + patchsets['immediate'])
            hook_sites = check_hooks(source, patched)
            hashes.append(runtime.digest(data))
    output.mkdir(parents=True, exist_ok=True)
    files = []
    def include(path, name, target, mode=0o644):
        runtime.atomic_write(output / name, path.read_bytes(), mode)
        files.append(dict(source=name, target=target, mode=mode, sha256=runtime.digest(path.read_bytes())))
    include(library, 'librx3_core.so', '/usr/lib/librx3_core.so', 0o755)
    for module, name in [('core', 'status-none-selected'), ('keyshift', 'key-selected'),
                         ('keyshift', 'none-selected'), ('keyshift', 'single-key-none'),
                         ('keyshift', 'single-key-selected'), ('samples', 'samples-selected'),
                         ('samples', 'samples-none-selected'), ('samples', 'samples-beatfx-selected'),
                         ('stems', 'stems-selected'), ('stems', 'single-stems-none'),
                         ('stems', 'single-stems-selected')]:
        for suffix in ('', '-light'):
            path = modules / module / 'assets' / (name + suffix + '.rgb565')
            include(path, path.name, '/root/pdj/rx3-' + path.name)
    for name in ('glyph-atlas-dark.rgb565', 'glyph-atlas-light.rgb565'):
        include(modules / 'core/assets' / name, name, '/root/pdj/rx3-' + name)
    shutil.copyfile(source / 'LICENSE', output / 'LICENSE.rx3-toolkit')
    shutil.copyfile(source / 'THIRD_PARTY_NOTICES.md', output / 'THIRD_PARTY_NOTICES.rx3-toolkit.md')
    runtime.atomic_write(output / 'SOURCE.txt',
                         ('https://github.com/Tratosca/rx3-toolkit\nRevision: ' + runtime.REVISION + '\n').encode())
    # Ship the modified file and its adapter with the optional library, rather
    # than pointing only to the unmodified upstream source.
    runtime.atomic_write(output / 'source/rx3_core_hook.pi.c', adapted.read_bytes())
    runtime.atomic_write(output / 'source/pi_process.h',
                         (ROOT / 'scripts/toolkit/pi_process.h').read_bytes())
    for path in (adapted_module, adapted_feature, composition, adapted_input, adapted_stems):
        runtime.atomic_write(output / 'source' / path.name, path.read_bytes())
    runtime.atomic_write(output / 'source/pi_network.h',
                         (ROOT / 'scripts/toolkit/pi_network.h').read_bytes())
    include(source / 'scripts/now_playing.py', 'now_playing.py', '/root/pdj/rx3-now-playing.py')
    runtime.atomic_write(output / 'source/pi_input.inc',
                         (ROOT / 'scripts/toolkit/pi_input.inc').read_bytes())
    runtime.atomic_write(output / 'source/pi_stems.h',
                         (ROOT / 'scripts/toolkit/pi_stems.h').read_bytes())
    manifest = dict(schema=2, modules=list(names), revision=runtime.REVISION, base_sha256=hashes,
                    patches=patchsets, hook_sites=hook_sites, files=files)
    runtime.atomic_write(output / 'manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())
    print('Toolkit bundle:', output)
    print('Source remains ignored; no firmware executable included in bundle.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'work/rx3-toolkit')
    parser.add_argument('--output', type=Path, default=ROOT / 'work/rx3-toolkit-pi')
    parser.add_argument('--stock', type=Path, default=ROOT / 'extracted/XDJRX3/pdj/rbp')
    parser.add_argument('--rootfs', type=Path, default=ROOT / 'extracted/XDJRX3-rootfs')
    parser.add_argument('--cc', default='arm-linux-gnueabi-gcc')
    args = parser.parse_args()
    try:
        build(args.source, args.output, args.stock, args.cc, args.rootfs)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, 'build-rx3-toolkit: %s\n' % error)
