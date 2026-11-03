#!/usr/bin/env python3
"""Render the original Orbit face with the exact embedded animation code."""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw
from preview_nixie import decode

ROOT = Path(__file__).resolve().parents[1]


def main():
    count = 96
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp)/'orbit-preview'
        subprocess.run(['g++','-std=c++11','-O2','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src'),str(ROOT/'tools/orbit_preview.cpp'),
                        '-o',str(binary)],check=True)
        raw = subprocess.check_output([str(binary),str(count)])
    frames = []
    for frame in range(count):
        sheet = Image.new('RGB',(940,280),(4,6,9))
        draw = ImageDraw.Draw(sheet)
        for tube in range(6):
            offset = (frame*6+tube)*64800
            x = 20+tube*153
            draw.rounded_rectangle((x-3,17,x+137,262),radius=11,
                                   fill=(10,14,19),outline=(37,45,52))
            sheet.paste(decode(raw[offset:offset+64800]),(x,20))
        frames.append(sheet)
    frames[9].resize((1880,560),Image.Resampling.LANCZOS).save(ROOT/'docs/orbit-preview.png')
    frames[1].save(ROOT/'docs/orbit-animation.gif',save_all=True,
                   append_images=frames[2:],duration=80,loop=0,optimize=True,disposal=1)
    phases = Image.new('RGB',(930,280),(4,6,9))
    for col,frame in enumerate((7,13,15,17,19,22)):
        phases.paste(frames[frame].crop((785,20,920,260)),(col*155+10,20))
    phases.save(ROOT/'docs/orbit-phases.png')
    print('Saved Orbit still and animated shared-renderer preview.')


if __name__ == '__main__':
    main()
