#!/usr/bin/env python3
"""Preview the exact firmware flip-clock renderer, including midnight rollover."""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw
from preview_nixie import decode

ROOT = Path(__file__).resolve().parents[1]


def main():
    count = 150
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp)/'flip-preview'
        subprocess.run(['g++','-std=c++11','-O2','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src'),str(ROOT/'tools/flip_preview.cpp'),
                        '-o',str(binary)],check=True)
        raw = subprocess.check_output([str(binary),str(count)])
    frames = []
    for frame in range(count):
        sheet = Image.new('RGB',(940,280),(4,6,9))
        draw = ImageDraw.Draw(sheet)
        for tube in range(6):
            offset = (frame*6+tube)*64800
            panel = decode(raw[offset:offset+64800])
            x = 20+tube*153
            draw.rounded_rectangle((x-3,17,x+137,262),radius=11,
                                   fill=(10,14,19),outline=(37,45,52))
            sheet.paste(panel,(x,20))
        frames.append(sheet)
    frames[20].resize((1880,560),Image.Resampling.LANCZOS).save(ROOT/'docs/flip-preview.png')
    frames[1].save(ROOT/'docs/flip-animation.gif',save_all=True,
                   append_images=frames[2:],duration=40,loop=0,optimize=True,disposal=1)
    # A static contact sheet makes the moving card's perspective easy to inspect.
    samples = (74,77,79,81,84,88)
    phases = Image.new('RGB',(6*155,280),(4,6,9))
    for col,frame in enumerate(samples):
        offset = (frame*6+5)*64800
        phases.paste(decode(raw[offset:offset+64800]),(col*155+10,20))
    phases.save(ROOT/'docs/flip-phases.png')
    print('Saved shared-renderer flip clock still, animation, and fold phases.')


if __name__ == '__main__':
    main()
