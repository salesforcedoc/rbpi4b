#!/usr/bin/env python3
"""Prepare an optional toolkit player without modifying the baseline executable."""
import argparse
import hashlib
import json
import ipaddress
import shlex
import socket
import struct
import wave
import os
from pathlib import Path
import tempfile


REVISION = 'cc17a1fde3016a44440948e50bde72f430da34d9'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path, data, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == data:
        path.chmod(mode)
        return
    fd, name = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def guarded_patch(data, patches):
    result = bytearray(data)
    occupied = set()
    for patch in patches:
        offset = patch['offset']
        before, after = bytes.fromhex(patch['before']), bytes.fromhex(patch['after'])
        if len(before) != 4 or len(after) != 4 or offset < 0 or offset % 4:
            raise ValueError('invalid patch: ' + patch['name'])
        addresses = set(range(offset, offset + 4))
        if occupied & addresses:
            raise ValueError('overlapping patches: ' + patch['name'])
        occupied.update(addresses)
        if result[offset:offset + 4] not in (before, after):
            raise ValueError('instruction mismatch: ' + patch['name'])
        result[offset:offset + 4] = after
    return bytes(result)


def within(root, relative):
    path = (root / relative.lstrip('/')).resolve()
    if root not in path.parents:
        raise ValueError('path escapes root: ' + relative)
    return path


# Names shared by the build, runtime validation and launch environment generator.
FLAGS = ('beatjump32', 'keyshift', 'immediate', 'decoder_sleep', 'key_sync',
         'key_match', 'samples', 'browse_columns', 'search_latin', 'hide_titles',
         'logo', 'now_playing', 'render_probe', 'stems')
RUNTIME_FLAGS = set(FLAGS) - {'beatjump32', 'immediate', 'decoder_sleep', 'render_probe'}
SETTINGS = dict(key_sync_range='1', key_sync_mode='identical', key_match_rules='2',
                browse_field='13', theme='off', samples_dir='/root/pdj/rx3-samples',
                now_playing_host='127.0.0.1', decoder_sleep_ns='100000', stems_dir='auto')


def configuration(environment):
    result = {}
    for name in FLAGS:
        value = environment.get('RB_TOOLKIT_' + name.upper()) or '0'
        if value not in ('0', '1'):
            raise ValueError(name + ' must be 0 or 1')
        result[name] = int(value)
    for name, default in SETTINGS.items():
        result[name] = environment.get('RB_TOOLKIT_' + name.upper()) or default
    for name, allowed in (('theme', ('off', 'dark', 'light', 'wait', 'switch')),
                          ('key_sync_mode', ('identical', 'harmonic')),
                          ('browse_field', ('7', '11', '13', '15'))):
        if result[name] not in allowed:
            raise ValueError('invalid ' + name)
    for name, low, high in (('key_sync_range', 1, 12), ('key_match_rules', 0, 15),
                            ('decoder_sleep_ns', 1, 1000000000)):
        value = result[name]
        if not value.isascii() or not value.isdecimal() or not low <= int(value) <= high:
            raise ValueError('invalid ' + name)
    if not result['samples_dir'].startswith('/'):
        raise ValueError('samples_dir must be an absolute chroot path')
    if result['stems_dir'] != 'auto' and not result['stems_dir'].startswith('/'):
        raise ValueError('stems_dir must be auto or an absolute chroot path')
    if result['key_sync']:
        result['keyshift'] = result['key_match'] = 1
    result['core'] = int(any(result[n] for n in RUNTIME_FLAGS) or result['theme'] != 'off')
    if result['render_probe'] and not result['core']:
        raise ValueError('render_probe requires a runtime module')
    result['now_playing_host'] = str(ipaddress.IPv4Address(result['now_playing_host']))
    return result


def validate_samples(directory):
    """Reject unsupported banks before the native asynchronous loader sees them."""
    total = 0
    for number in range(1, 9):
        path = directory / ('%d.wav' % number)
        if not path.exists():
            continue
        if path.is_symlink():
            raise ValueError('sample WAV must be a regular file, not a symlink: ' + str(path))
        if path.stat().st_size > 0x1000000:
            raise ValueError('sample file too large: ' + str(path))
        with wave.open(str(path), 'rb') as sample:
            if (sample.getnchannels(), sample.getsampwidth(), sample.getframerate(),
                sample.getcomptype()) != (2, 2, 44100, 'NONE'):
                raise ValueError('samples require stereo PCM16 WAV at 44100 Hz: ' + str(path))
            frames = sample.getnframes()
            if not 1 <= frames <= 44100 * 8 or len(sample.readframes(frames)) != frames * 4:
                raise ValueError('sample must contain 1 frame through 8 seconds: ' + str(path))
            total += frames * 4
    if total > 0x1000000:
        raise ValueError('sample bank exceeds 16 MiB')


def validate_sample_settings(data):
    if not data or len(data) > 4096 or b'\x00' in data:
        raise ValueError('invalid sample settings.ini')
    seen = set()
    for line in data.decode('utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        if '=' not in line:
            raise ValueError('malformed sample setting')
        key, value = line.split('=', 1)
        ranges = {'version':(1,1), 'volume_default':(0,100), 'shift.silence':(0,1)}
        for n in range(1, 9):
            ranges['pad%d.mode' % n] = (0,3)
            ranges['pad%d.gain' % n] = (0,200)
        known = key in ranges or key in {'pad%d.color' % n for n in range(1,9)}
        if not known:
            continue
        if key in seen:
            raise ValueError('duplicate sample setting: ' + key)
        seen.add(key)
        if key in ranges:
            low, high = ranges[key]
            if not value.isascii() or not value.isdecimal() or not low <= int(value) <= high:
                raise ValueError('invalid sample setting: ' + key)
        elif len(value) != 7 or value[0] != '#' or any(c not in '0123456789abcdefABCDEF' for c in value[1:]):
            raise ValueError('invalid sample colour')
    if 'version' not in seen:
        raise ValueError('sample settings need version=1')


def validate_logo(path):
    data = path.read_bytes()
    width, height, offset = 492, 70, 0
    if data.startswith(b'RX3LOGO1') and len(data) >= 16:
        width, height = struct.unpack_from('<II', data, 8)
        offset = 16
    if not 1 <= width <= 888 or not 1 <= height <= 445 or len(data) != offset + width * height * 2:
        raise ValueError('invalid RX3 RGB565 logo: ' + str(path))
    return width, height


def launch_environment(config, player):
    c = config
    env = dict(RB_TOOLKIT_CORE=str(c['core']), RX3_KEYSHIFT=str(c['keyshift']),
               RX3_KEY_SYNC=str(c['key_sync']), RX3_KEY_MATCH=str(c['key_match']),
               RX3_KEY_SYNC_RANGE=c['key_sync_range'], RX3_KEY_SYNC_MODE=c['key_sync_mode'],
               RX3_KEY_MATCH_RULES=str(int(c['key_match_rules']) & 14),
               RX3_BROWSE_COLUMNS=str(c['browse_columns']), RX3_BROWSE_FIELD=c['browse_field'],
               RX3_SEARCH_LATIN=str(c['search_latin']), RX3_ASSHOLE_MODE=str(c['hide_titles']),
               RX3_LOGO=str(c['logo']), RX3_NOW_PLAYING=str(c['now_playing']),
               RX3_PI_NOW_PLAYING_HOST=c['now_playing_host'], RX3_MESSAGES='0',
               RX3_RENDER_PROBE=str(c['render_probe']), RX3_LOG_FILE='/tmp/rx3-toolkit.log',
               RX3_THEME={'off':'', 'dark':'d', 'light':'l', 'wait':'w', 'switch':'s'}[c['theme']],
               RX3_STEMS_DIR=c['stems_dir'] if c['stems'] else '',
               RX3_OVERCUE_ROOT='', RX3_SAMPLES_DIR=c['samples_dir'] if c['samples'] else '',
               RX3_SAMPLES_CONFIG=c['samples_dir'] + '/settings.ini' if c['samples'] else '',
               RX3_PI_PLAYER=player + '.toolkit/rbp')
    panels = {0:'key-selected' if c['keyshift'] else '',
              1:'stems-selected' if c['stems'] else '', 2:'status-none-selected',
              3:'none-selected' if c['keyshift'] else '',
              4:'samples-selected' if c['samples'] else '',
              5:'samples-none-selected' if c['samples'] else '',
              6:'samples-beatfx-selected' if c['samples'] else '',
              7:'single-key-none' if c['keyshift'] else '',
              8:'single-key-selected' if c['keyshift'] else '',
              9:'single-stems-none' if c['stems'] else '',
              10:'single-stems-selected' if c['stems'] else ''}
    for slot in range(11):
        for mode in ('DARK', 'LIGHT'):
            asset = panels.get(slot, '')
            suffix = '-light' if mode == 'LIGHT' else ''
            env['RX3_TAB_%s_%02d' % (mode, slot)] = '/root/pdj/rx3-' + asset + suffix + '.rgb565' if asset else ''
    return env


def prepare(bundle, chroot, player, beatjump32, keyshift, config=None):
    bundle, chroot = bundle.resolve(), chroot.resolve()
    config = config or configuration(dict(RB_TOOLKIT_BEATJUMP32=str(beatjump32),
                                          RB_TOOLKIT_KEYSHIFT=str(keyshift)))
    manifest = json.loads((bundle / 'manifest.json').read_text())
    if manifest['revision'] != REVISION or manifest['schema'] not in (1, 2):
        raise ValueError('unsupported toolkit bundle; rebuild with the pinned source')
    extended = any(config[n] for n in FLAGS if n not in ('beatjump32', 'keyshift')) or config['theme'] != 'off'
    if extended and manifest['schema'] != 2:
        raise ValueError('extended features need a rebuilt schema 2 bundle')
    if config['stems'] and not {'stems', 'stemwave'} <= set(manifest.get('modules', ())):
        raise ValueError('Stems needs a rebuilt bundle containing stems and stemwave')
    if config['stems_dir'] != 'auto':
        within(chroot, config['stems_dir'])
    baseline = within(chroot, player)
    original = baseline.read_bytes()
    if digest(original) not in manifest['base_sha256']:
        raise ValueError('unsupported baseline player; expected this port\'s patched firmware 1.20')
    patches = []
    for enabled, group in ((config['beatjump32'], 'beatjump32'), (config['core'], 'keyshift'),
                           (config['immediate'], 'immediate')):
        if enabled:
            patches.extend(manifest['patches'][group])
    modified = guarded_patch(original, patches)
    # Validate the entire selected bundle and user assets before publishing.
    files = []
    if config['core']:
        for entry in manifest['files']:
            content = within(bundle, entry['source']).read_bytes()
            if digest(content) != entry['sha256']:
                raise ValueError('bundle checksum mismatch: ' + entry['source'])
            files.append((within(chroot, entry['target']), content, entry['mode']))
    if config['samples']:
        directory = within(chroot, config['samples_dir'])
        validate_samples(directory)
        sample_settings = directory / 'settings.ini'
        if sample_settings.exists():
            data = sample_settings.read_bytes()
            validate_sample_settings(data)
        # A new bank has a usable empty panel until the user supplies samples.
        else:
            files.append((sample_settings, b'version=1\nvolume_default=50\n', 0o644))
    if config['logo']:
        dimensions = validate_logo(within(chroot, '/root/pdj/rx3-logo-main.rgb565'))
        light = within(chroot, '/root/pdj/rx3-logo-main-light.rgb565')
        if light.exists() and validate_logo(light) != dimensions:
            raise ValueError('dark/light logo sizes differ')
    for path, content, mode in files:
        atomic_write(path, content, mode)
    target = within(chroot, player + '.toolkit/rbp')
    atomic_write(target, modified, 0o755)
    print('toolkit: prepared %s (features=%s theme=%s; baseline unchanged)' %
          (target, ','.join(n for n in FLAGS if config[n]), config['theme']))
    return target


def decoder_sleep(nanoseconds):
    """Use the native debug protocol, without the stock RX3 shell dependencies."""
    if not 1 <= int(nanoseconds) <= 1000000000:
        raise ValueError('invalid decoder interval')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.settimeout(2)
        connection.connect(('127.0.0.1', 20000))
        for deck in (0, 1):
            connection.send(('bufsleep %d %d\n' % (deck, int(nanoseconds))).encode())
            response = connection.recv(4096).rstrip(b'\0')
            if not response:
                raise ValueError('empty decoder response')
            print('toolkit: decoder deck %d reply: %s' % (deck, response.decode(errors='replace')))
    print('toolkit: decoder replies received; interval not independently read back')


def report(chroot, output):
    snapshot = {'revision': REVISION}
    for name in ('rx3-performance.ready', 'rx3-toolkit.log', 'rx3-toolkit.log.prev'):
        path = within(chroot.resolve(), '/tmp/' + name)
        snapshot[name] = path.read_bytes()[-131072:].decode(errors='replace') if path.is_file() else None
    atomic_write(output, (json.dumps(snapshot, indent=2) + '\n').encode(), 0o600)
    print('toolkit: diagnostic snapshot:', output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--chroot', type=Path)
    parser.add_argument('--player', default='/root/pdj/rbp')
    parser.add_argument('--beatjump32', type=int, choices=(0, 1), default=0)
    parser.add_argument('--keyshift', type=int, choices=(0, 1), default=0)
    parser.add_argument('--extended', action='store_true', help='read exported RB_TOOLKIT_* settings')
    parser.add_argument('--env-file', type=Path)
    parser.add_argument('--decoder-sleep', type=int)
    parser.add_argument('--report', type=Path, help='save bounded hook diagnostics; requires --chroot')
    args = parser.parse_args()
    try:
        if args.report is not None:
            if args.chroot is None:
                raise ValueError('--report requires --chroot')
            report(args.chroot, args.report)
            return
        if args.decoder_sleep is not None:
            decoder_sleep(args.decoder_sleep)
            return
        if args.bundle is None or args.chroot is None:
            raise ValueError('--bundle and --chroot are required')
        config = configuration(os.environ if args.extended else dict(
            RB_TOOLKIT_BEATJUMP32=str(args.beatjump32), RB_TOOLKIT_KEYSHIFT=str(args.keyshift)))
        prepare(args.bundle, args.chroot, args.player, args.beatjump32, args.keyshift, config)
        if args.env_file:
            env = launch_environment(config, args.player)
            data = ''.join('export %s=%s\n' % (k, shlex.quote(str(v))) for k,v in env.items())
            atomic_write(args.env_file, data.encode(), 0o600)
    except (OSError, ValueError, KeyError, TypeError, wave.Error, EOFError) as error:
        parser.exit(1, 'toolkit: REFUSED: %s\n' % error)


if __name__ == '__main__':
    main()
