#!/usr/bin/env python3
"""Prepare optional Samples/logo files locally; never writes to a USB or Pi."""
import argparse
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('toolkit', ROOT / 'scripts/device/rx3-toolkit.py')
toolkit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolkit)


def prepare(output, samples, logo=None, light_logo=None, mode='once', volume=50):
    if len(samples) > 8 or not 0 <= volume <= 100:
        raise ValueError('use at most eight samples and volume 0..100')
    if light_logo and not logo:
        raise ValueError('--logo-light also requires --logo')
    with tempfile.TemporaryDirectory() as temporary:
        staged = Path(temporary)
        if samples:
            bank = staged / 'rx3-samples'
            bank.mkdir()
            for number, sample in enumerate(samples, 1):
                target = bank / ('%d.wav' % number)
                if shutil.which('ffmpeg'):
                    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', str(sample),
                                    '-t', '8', '-ar', '44100', '-ac', '2', '-c:a', 'pcm_s16le',
                                    str(target)], check=True)
                else:
                    # Already converted WAVs need no external dependency.
                    shutil.copyfile(sample, target)
            toolkit.validate_samples(bank)
            settings = 'version=1\nvolume_default=%d\n' % volume
            for number in range(1, len(samples) + 1):
                settings += 'pad%d.mode=%d\n' % (number, ('once','hold','loop','latch').index(mode))
            (bank / 'settings.ini').write_text(settings)
        for image_path, name in ((logo, 'rx3-logo-main.rgb565'),
                                 (light_logo, 'rx3-logo-main-light.rgb565')):
            if image_path is None:
                continue
            if image_path.suffix == '.rgb565':
                toolkit.validate_logo(image_path)
                shutil.copyfile(image_path, staged / name)
                continue
            try:
                from PIL import Image
            except ImportError as error:
                raise ValueError('PNG/JPEG/WebP logos require Pillow; preconverted .rgb565 also works') from error
            with Image.open(image_path) as image:
                image = image.convert('RGBA')
                image.thumbnail((492, 70))
                # Magenta RGB565 is the native transparency key.
                pixels = bytearray()
                for red, green, blue, alpha in image.getdata():
                    value = 0xf81f if alpha < 128 else ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
                    pixels.extend(struct.pack('<H', value))
                header = b'RX3LOGO1' + struct.pack('<II', *image.size)
                (staged / name).write_bytes(header + pixels)
        if logo and light_logo and toolkit.validate_logo(staged / 'rx3-logo-main.rgb565') != toolkit.validate_logo(staged / 'rx3-logo-main-light.rgb565'):
            raise ValueError('dark/light logos must have the same aspect ratio and resulting size')
        # Publish only after all conversions validate. No destructive directory sync.
        for path in staged.rglob('*'):
            if path.is_file():
                toolkit.atomic_write(output / path.relative_to(staged), path.read_bytes())
    print('User assets:', output.resolve())
    if samples:
        print('Only supplied slots were replaced; remove any unwanted older numbered WAVs explicitly.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'work/rx3-toolkit-user-assets')
    parser.add_argument('--samples', nargs='+', type=Path, default=[])
    parser.add_argument('--mode', choices=('once','hold','loop','latch'), default='once')
    parser.add_argument('--volume', type=int, default=50)
    parser.add_argument('--logo', type=Path)
    parser.add_argument('--logo-light', type=Path)
    args = parser.parse_args()
    try:
        if not args.samples and not args.logo:
            raise ValueError('provide --samples and/or --logo')
        prepare(args.output, args.samples, args.logo, args.logo_light, args.mode, args.volume)
    except (OSError, ValueError, wave.Error, EOFError, subprocess.CalledProcessError) as error:
        parser.exit(1, 'prepare-rx3-toolkit-assets: %s\n' % error)
