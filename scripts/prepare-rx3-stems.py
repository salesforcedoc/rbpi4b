#!/usr/bin/env python3
"""Import already separated audio using the pinned toolkit's validated packager.

For automatic separation, use the upstream desktop Stems screen. This helper
uses the same alignment, duration, gain, package and optional waveform checks
for lossless vocal/drum files produced by another separator.
"""
import argparse
import importlib.util
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('toolkit', ROOT / 'scripts/device/rx3-toolkit.py')
toolkit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolkit)


def load_source(source):
    revision = subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    if revision != toolkit.REVISION:
        raise ValueError('expected pinned toolkit revision ' + toolkit.REVISION)
    if subprocess.check_output(['git','-C',str(source),'status','--porcelain','--untracked-files=no'],text=True).strip():
        raise ValueError('toolkit tracked source is modified')
    sys.path.insert(0,str(source.resolve()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'work/rx3-toolkit')
    commands = parser.add_subparsers(dest='command',required=True)
    command = commands.add_parser('import',help='package lossless separated vocals and optional drums')
    command.add_argument('--track',type=Path,required=True,help='the original track from the rekordbox export')
    command.add_argument('--vocals',type=Path,required=True)
    command.add_argument('--drums',type=Path)
    command.add_argument('--output',type=Path,default=ROOT/'work/rx3-stems-export',help='export root; writes its RX3_STEMS folder')
    command.add_argument('--ffmpeg',default='ffmpeg')
    command.add_argument('--no-waveforms',action='store_true')
    command = commands.add_parser('inspect',help='verify package CRCs and show its manifest')
    command.add_argument('package',type=Path)
    args = parser.parse_args()
    try:
        load_source(args.source)
        if args.command == 'inspect':
            import json
            from app.stems import package
            print(json.dumps(package.read(args.package.resolve())['manifest'],ensure_ascii=False,indent=2))
            return
        if not shutil.which(args.ffmpeg):
            raise ValueError('ffmpeg is required on the preparation computer')
        executable = Path(shutil.which(args.ffmpeg))
        probe = executable.with_name('ffprobe' + ('.exe' if sys.platform == 'win32' else ''))
        if not probe.is_file() and not shutil.which('ffprobe'):
            raise ValueError('ffprobe is required alongside ffmpeg on the preparation computer')
        from app.stems import stem, importing
        from app.stems.rekordbox import Track, export_stem
        original = args.track.resolve(strict=True)
        inputs = {'vocals':args.vocals.resolve(strict=True)}
        if args.drums:
            inputs['drums'] = args.drums.resolve(strict=True)
        # Validate the timeline before creating the output directory. The
        # upstream importer checks it again against the actual prepared PCM.
        frames = stem.count_frames(original,args.ffmpeg)
        track = Track('manual',original.stem,'',math.ceil(frames/44100),original,True)
        output = args.output.resolve()
        output.mkdir(parents=True,exist_ok=True)
        importing.publish(track,inputs,output,ffmpeg=args.ffmpeg,waveforms=not args.no_waveforms)
        print(output/'RX3_STEMS'/(export_stem(original.stem)+'.rx3stem'))
    except (OSError,ValueError,RuntimeError,ImportError,struct.error,subprocess.SubprocessError) as error:
        parser.exit(1,'prepare-rx3-stems: %s\n' % error)


if __name__ == '__main__':
    main()
