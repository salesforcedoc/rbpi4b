#!/usr/bin/env python3
"""Identify FLX6 BEAT display codes; temporarily stop/restore rblive4.

Writes only B4/B5 CC 0x64, the documented BEAT display output. No audio tones,
SysEx, service mode or configuration changes. Run on the Pi with sudo python3.
"""

import argparse
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys


RESULT = Path('/tmp/flx6-beat-led-probe.json')
# Establish documented baselines before checking the omitted codes.
VALUES = (3, 4, 5, 6, 7, 8, 9, 10, 0, 1, 2, 11, 12, 13, 14, 15, 127)
KNOWN = {3: '1/4', 4: '1/2', 5: '1', 6: '2 (1 and 4 together)',
         7: '4', 8: '8', 9: '16', 10: '32'}
PAIRS = ((3, 6, False), (3, 6, True), (3, 7, False),
         (3, 7, True), (3, 8, False), (3, 8, True))


def send(fd, value):
    send_pair(fd, value, value)


def send_pair(fd, first, second, reverse=False):
    a, b = bytes((0xB4, 0x64, first)), bytes((0xB5, 0x64, second))
    message = b + a if reverse else a + b
    if os.write(fd, message) != len(message):
        raise OSError('Incomplete MIDI write; stopping the probe.')


def interrupted(_number, _frame):
    raise KeyboardInterrupt


def main(scan=False, pairs=False):
    if os.geteuid() != 0:
        raise SystemExit('Run on Ita with: sudo python3 /tmp/flx6-beat-led-probe.py')
    card = Path('/proc/asound/DDJFLX6')
    if not card.exists():
        raise SystemExit('DDJFLX6 ALSA card not found; player left running.')
    match = re.fullmatch(r'card(\d+)', card.resolve().name)
    if not match or (card / 'id').read_text().strip() != 'DDJFLX6':
        raise SystemExit('Could not identify the FLX6 MIDI device; player left running.')
    device = Path('/dev/snd/midiC%sD0' % match.group(1))
    if not device.exists():
        raise SystemExit('FLX6 raw MIDI device not found: ' + str(device))

    values = PAIRS if pairs else range(16, 127) if scan else VALUES
    print('FLX6 BEAT display test: %d values.' % len(values))
    print('The player stops temporarily. No sound is generated.')
    if pairs:
        print('Six combination tests: FX1 gets 1/4; FX2 gets 2, 4 or 8.')
        print('Each is repeated in reverse message order to check whether the')
        print('display combines them or only follows one of the two units.')
        print('Record exactly what lights, without interpreting the intended fraction.')
    elif scan:
        print('Scanning the remaining CC values, about two minutes total.')
        print('Watch the BEAT lights. If ANY light appears, press Enter to pause,')
        print('then record its labels. Unmarked values are not claimed as observations.')
    else:
        print('For each value, type ALL lit labels, e.g. "1/4 + 4", "3/4", or "none".')
    print('Include whether they blink. Type q then Enter, or Ctrl+C, to finish early.')
    print('Do not press controller buttons during the test.')
    input('Press Enter to start, or Ctrl+C to cancel: ')
    signal.signal(signal.SIGTERM, interrupted)
    was_active = subprocess.run(
        ['systemctl', 'is-active', '--quiet', 'rblive4.service']
    ).returncode == 0
    fd = None
    records = []
    try:
        if was_active:
            subprocess.run(['systemctl', 'stop', 'rblive4.service'], check=True)
        fd = os.open(device, os.O_WRONLY | os.O_NONBLOCK)
        for value in values:
            if scan or pairs:
                send(fd, 127)  # User verified this value clears the display.
            if pairs:
                first, second, reverse = value
                send_pair(fd, first, second, reverse)
                label = 'B4=%02X B5=%02X order=%s' % (
                    first, second, 'B5 then B4' if reverse else 'B4 then B5')
                print('\n' + label, flush=True)
            else:
                send(fd, value)
                label = '%02X' % value
                print('\nB4/B5 64 %02X — %s' %
                      (value, KNOWN.get(value, 'not described in the MIDI list')), flush=True)
            answer = 'not marked'
            if scan:
                readable, _, _ = select.select([sys.stdin], [], [], 1.2)
                if readable:
                    line = sys.stdin.readline()
                    if not line:
                        raise EOFError
                    if line.strip().lower() in ('q', 'quit', 'exit'):
                        break
                    answer = input('Paused on %02X. Lights visible: ' % value).strip()
            else:
                answer = input('Lights visible: ').strip()
            if answer.lower() in ('q', 'quit', 'exit'):
                break
            records.append({'value': value, 'hex': label, 'lights': answer})
            RESULT.write_text(json.dumps(records, indent=2) + '\n')
    finally:
        try:
            if fd is not None:
                try:
                    send(fd, 3)  # Known 1/4 display, never an invented OFF code.
                finally:
                    os.close(fd)
        finally:
            if was_active:
                print('\nRestoring rblive4.service...')
                subprocess.run(['systemctl', 'start', 'rblive4.service'], check=True)
            else:
                print('\nPlayer was already stopped; leaving it stopped.')
        print('Recorded observations: ' + str(RESULT))
        marked = [row for row in records if row['lights'] != 'not marked']
        for row in marked:
            print('%s -> %s' % (row['hex'], row['lights']))
        if scan and not marked:
            print('No light patterns marked during this scan.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--scan', action='store_true',
                        help='automatically scan untested values 10..7e; Enter pauses')
    mode.add_argument('--pairs', action='store_true',
                      help='six tests of simultaneous FX1/FX2 beat-light patterns')
    args = parser.parse_args()
    try:
        main(args.scan, args.pairs)
    except (KeyboardInterrupt, EOFError):
        print('\nProbe finished early.')
