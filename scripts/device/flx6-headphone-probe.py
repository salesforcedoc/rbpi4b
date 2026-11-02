#!/usr/bin/env python3
"""Interactive FLX6 output-pair probe; run on the Pi with sudo python3.

Temporarily stops rblive4 and restores its previous running state on exit.
Does not change mixer controls, MIDI state, configuration, or audio maps.
"""

import math
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import tempfile
import wave


def make_tone(path, pair):
    rate = 44100
    frames = rate * 20
    # About -36 dBFS peak, with a short fade at either end.
    audio = bytearray()
    for frame in range(frames):
        fade = min(1.0, frame / 882, (frames - 1 - frame) / 882)
        value = round(512 * fade * math.sin(2 * math.pi * 440 * frame / rate))
        samples = [0, 0, 0, 0]
        samples[pair * 2] = samples[pair * 2 + 1] = value
        audio.extend(struct.pack('<4h', *samples))
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(4)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(audio)


def interrupted(_signum, _frame):
    raise KeyboardInterrupt


def main():
    if os.geteuid() != 0:
        raise SystemExit('Run on Ita with: sudo python3 flx6-headphone-probe.py')
    for program in ('systemctl', 'aplay'):
        if shutil.which(program) is None:
            raise SystemExit('Required command missing: ' + program)
    if not Path('/proc/asound/DDJFLX6').exists():
        raise SystemExit('ALSA card DDJFLX6 was not found; player left running.')

    print('FLX6 headphone routing probe (two 20-second tests).')
    print('Turn the headphone LEVEL down first, then raise it gently as needed.')
    print('Lower or switch off external speakers; tones also reach the RCA outputs.')
    print('During each tone, sweep headphone MIX: left, center, right.')
    print('Also try the physical MASTER CUE button and note any change.')
    print('Write down where the tone is audible and how its volume changes.')
    print('The player will stop temporarily; Ctrl+C restores its running state.')
    input('Press Enter to prepare the test, or Ctrl+C to cancel: ')

    signal.signal(signal.SIGTERM, interrupted)
    with tempfile.TemporaryDirectory(prefix='flx6-headphone-') as directory:
        paths = [Path(directory) / ('pair%d.wav' % pair) for pair in range(2)]
        for pair, path in enumerate(paths):
            make_tone(path, pair)
        was_active = subprocess.run(
            ['systemctl', 'is-active', '--quiet', 'rblive4.service']
        ).returncode == 0
        try:
            if was_active:
                subprocess.run(['systemctl', 'stop', 'rblive4.service'], check=True)
            for pair, path in enumerate(paths):
                print('\nTEST %s: tone ONLY on USB channels %s; other pair is silent.' %
                      ('AB'[pair], '1/2 (master)' if pair == 0 else '3/4 (headphones)'))
                input('Press Enter to play for 20 seconds: ')
                subprocess.run([
                    'aplay', '-D', 'hw:CARD=DDJFLX6,DEV=0', str(path)
                ], check=True)
        finally:
            if was_active:
                print('\nRestoring rblive4.service...')
                subprocess.run(['systemctl', 'start', 'rblive4.service'], check=True)
            else:
                print('\nPlayer was already stopped; leaving it stopped.')
    print('Finished. Report the headphone sound at left/center/right for A and B.')
    print('These results describe standalone USB playback; DJ software may initialize')
    print('the controller differently, so a negative result does not rule that out.')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\nProbe cancelled.')
