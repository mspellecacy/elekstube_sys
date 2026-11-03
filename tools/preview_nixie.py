#!/usr/bin/env python3
"""Render a looping preview with the same C++ code used by the actual clock."""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]


def decode(raw):
    pixels = []
    for n in range(0,len(raw),2):
        c = raw[n] | raw[n+1]<<8
        pixels.append(((c>>11)*255//31, ((c>>5)&63)*255//63, (c&31)*255//31))
    image = Image.new('RGB',(135,240))
    image.putdata(pixels)
    return image


def main():
    count = 96
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp)/'nixie-preview'
        subprocess.run(['g++','-std=c++11','-O2','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src'),str(ROOT/'tools/nixie_preview.cpp'),
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
    (ROOT/'docs').mkdir(exist_ok=True)
    frames[10].resize((1880,560),Image.Resampling.LANCZOS).save(ROOT/'docs/nixie-preview.png')
    frames[1].save(ROOT/'docs/nixie-animation.gif',save_all=True,
                   append_images=frames[2:],duration=80,loop=0,optimize=True,disposal=1)
    print('Saved shared-renderer Nixie still and animation.')


if __name__ == '__main__':
    main()
