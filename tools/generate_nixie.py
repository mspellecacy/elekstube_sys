#!/usr/bin/env python3
"""Build cathode distance fields and a close-up honeycomb Nixie cage.

Digits are original curved wire paths, not photographs or font glyphs. Distance
fields let firmware synthesize the luminous core/halo at any brightness and mix
the outgoing and incoming cathodes during a transition. Only Pillow is needed.
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 135, 240
X, Y, W, H = 0, 22, 135, 202
SCALE = 3

# M/L/Q/C: move, line, quadratic and cubic Bezier, in panel coordinates.
PATHS = (
    [('M',67,40),('C',34,40,23,62,23,120),('C',23,178,34,208,67,208),
     ('C',101,208,112,178,112,120),('C',112,62,101,40,67,40)],
    [('M',68,42),('L',68,207)],
    [('M',25,67),('C',28,35,103,29,111,66),('C',122,111,28,145,24,194),
     ('Q',24,207,40,207),('L',111,207)],
    [('M',27,43),('L',110,43),('L',62,115),
     ('C',98,108,114,129,111,165),('C',108,217,46,222,24,190)],
    [('M',88,40),('L',23,145),('L',115,145),('M',88,40),('L',88,208)],
    [('M',109,41),('L',31,41),('L',27,115),('C',65,96,111,114,111,157),
     ('C',111,208,49,225,24,190)],
    [('M',93,42),('C',49,40,24,103,24,155),('C',24,218,111,227,111,166),
     ('C',111,115,39,113,24,157)],
    [('M',23,42),('L',113,42),('L',43,208)],
    [('M',67,119),('C',18,111,24,40,67,40),('C',111,40,116,111,67,119),
     ('C',11,126,17,208,67,208),('C',118,208,123,126,67,119)],
    [('M',111,93),('C',111,33,24,25,24,88),('C',24,141,95,143,111,93),
     ('C',112,149,84,197,44,208)],
)


def polylines(path):
    lines, points, position = [], [], (0, 0)
    for command, *values in path:
        if command == 'M':
            if points:
                lines.append(points)
            position = tuple(values)
            points = [position]
        elif command == 'L':
            position = tuple(values)
            points.append(position)
        else:
            start = position
            for step in range(1, 41):
                t, u = step / 40, 1 - step / 40
                if command == 'Q':
                    point = tuple(u*u*start[a] + 2*u*t*values[a] + t*t*values[a+2] for a in (0,1))
                else:
                    point = tuple(u**3*start[a] + 3*u*u*t*values[a] +
                                  3*u*t*t*values[a+2] + t**3*values[a+4] for a in (0,1))
                points.append(point)
            position = points[-1]
    if points:
        lines.append(points)
    return lines


def distance_1d(values):
    """Exact squared Euclidean distance transform, lower envelope of parabolas."""
    n = len(values)
    sites, edges = [0] * n, [float('-inf')] + [float('inf')] * n
    k = 0
    for q in range(1, n):
        p = sites[k]
        intersection = ((values[q] + q*q) - (values[p] + p*p)) / (2*(q-p))
        while intersection <= edges[k]:
            k -= 1
            p = sites[k]
            intersection = ((values[q] + q*q) - (values[p] + p*p)) / (2*(q-p))
        k += 1
        sites[k], edges[k], edges[k+1] = q, intersection, float('inf')
    result, k = [], 0
    for q in range(n):
        while edges[k+1] < q:
            k += 1
        result.append((q-sites[k])**2 + values[sites[k]])
    return result


def distance_field(path):
    width, height = W*SCALE, H*SCALE
    mask = Image.new('L', (width, height))
    draw = ImageDraw.Draw(mask)
    for line in polylines(path):
        draw.line([(round((x-X)*SCALE), round((y-Y)*SCALE)) for x,y in line], fill=255, width=1)
    pixels = mask.tobytes()
    rows = [distance_1d([0 if pixels[y*width+x] else 10000000 for x in range(width)])
            for y in range(height)]
    field = [0] * (W*H)
    for x in range(W):
        xx = x*SCALE + SCALE//2
        column = distance_1d([row[xx] for row in rows])
        for y in range(H):
            d = math.sqrt(column[y*SCALE+SCALE//2]) / SCALE
            # Quarter-pixel distances leave two palette bits for mesh coverage.
            field[y*W+x] = min(63, round(d*4))
    return field


def artwork():
    """Original metalwork inspired by the stock face's close-up composition."""
    size = WIDTH*SCALE, HEIGHT*SCALE
    image = Image.new('RGB', size)
    pixels = image.load()
    for y in range(size[1]):
        for x in range(size[0]):
            xx, yy = x/SCALE, y/SCALE
            body = math.exp(-((xx-67)/56)**6 - ((yy-125)/103)**8)
            grain = ((x*73+y*131+(x*y)%43)%29)/29
            pixels[x,y] = (round(body*(6+5*grain)), round(body*(3+2*grain)), round(body*2))
    draw = ImageDraw.Draw(image)

    def line(points, color, width=0.5, target=draw):
        target.line([(round(x*SCALE),round(y*SCALE)) for x,y in points], fill=color,
                    width=max(1,round(width*SCALE)), joint='curve')

    # Depth comes from several unlit, offset cathodes and their fine supports.
    cathodes = Image.new('L', size)
    cathode_draw = ImageDraw.Draw(cathodes)
    for digit, path in enumerate(PATHS):
        for points in polylines(path):
            shifted = [(x+(digit%3-1)*1.1, y+(digit%4-2)*0.6) for x,y in points]
            line(shifted, (21,10,6), 1.15)
            line(shifted, 65, .45, cathode_draw)
    for index in range(9):
        xx = 40+index*6.6
        line([(xx,211),(xx+(index%3-1)*2,224)], (35,20,11), .7)
    for xx in (49,85):
        line([(xx,32),(xx,214)], (20,11,8), .7)
        line([(xx+1,32),(xx+1,214)], (6,4,3), .8)

    # Flat-top hexagonal anode mesh, with a tiny metallic edge on its dark wire.
    mesh = Image.new('L', size)
    mesh_draw = ImageDraw.Draw(mesh)
    radius = 7.0
    pitch = math.sqrt(3)*radius
    for col in range(13):
        cx = -5+col*1.5*radius
        for row in range(19):
            cy = 17+row*pitch+(col%2)*pitch/2
            points = [(cx+radius*math.cos(n*math.pi/3),
                       cy+radius*math.sin(n*math.pi/3)) for n in range(7)]
            line([(x+.45,y+.35) for x,y in points], (35,27,21), .45)
            line(points, (9,7,6), 1.05)
            line(points, 255, 1.05, mesh_draw)

    # Black vignette keeps the cage cylindrical instead of a rectangular grid.
    pixels = image.load()
    for y in range(size[1]):
        for x in range(size[0]):
            xx, yy = x/SCALE, y/SCALE
            inside = math.exp(-((xx-67)/67)**12 - ((yy-124)/103)**12)
            pixels[x,y] = tuple(round(c*inside) for c in pixels[x,y])

    # Side rails and the small top/bottom mounting rivets from the close-up view.
    for xx, side in ((5,1),(129,-1)):
        for y in range(49,207,16):
            line([(xx,y),(xx+side,y+9)], (54,45,33), .65)
            line([(xx+side,y+2),(xx+side,y+7)], (105,89,66), .35)
    for cy in (22,221):
        for radius, color in ((8,(11,10,8)),(6,(45,37,26)),(4,(77,66,47))):
            bounds = ((67-radius)*SCALE,(cy-radius)*SCALE,
                      (67+radius)*SCALE,(cy+radius)*SCALE)
            draw.ellipse(bounds,fill=color)
        draw.ellipse((63*SCALE,(cy-5)*SCALE,69*SCALE,(cy+4)*SCALE),fill=(13,11,9))
        line([(64,cy-5),(68,cy-3),(70,cy+1)], (139,127,101), 1.0)
        line([(69,cy-5),(72,cy-2)], (78,72,62), .7)

    image = image.resize((WIDTH,HEIGHT), Image.Resampling.LANCZOS)
    mesh = mesh.resize((WIDTH,HEIGHT), Image.Resampling.LANCZOS)
    cathodes = cathodes.resize((WIDTH,HEIGHT), Image.Resampling.LANCZOS)
    base = [((r&248)<<8)|((g&252)<<3)|(b>>3) for r,g,b in image.getdata()]
    coverage = []
    for y in range(Y,Y+H):
        for x in range(X,X+W):
            # Four optical transmission levels keep mesh edges antialiased.
            opacity = max(mesh.getpixel((x,y)), cathodes.getpixel((x,y)))
            coverage.append(min(3, round(opacity*3/255))*64)
    return base, coverage


def main():
    fields = []
    for digit,path in enumerate(PATHS):
        fields.append(distance_field(path))
        print('Wire distance field', digit, flush=True)
    base, mesh = artwork()
    # Fold mesh transmission into each distance byte. Sixteen background shades
    # keep the per-frame lighting lookup small; luminous cathodes retain the
    # full RGB565 emission palette and are not quantized with the metalwork.
    fields = [[distance | coverage for distance,coverage in zip(field,mesh)]
              for field in fields]
    metal = Image.new('RGB',(WIDTH,HEIGHT))
    metal.putdata([((c>>11)*255//31,((c>>5)&63)*255//63,(c&31)*255//31) for c in base])
    metal = metal.quantize(colors=16,method=Image.Quantize.MEDIANCUT).convert('RGB')
    base = [((r&248)<<8)|((g&252)<<3)|(b>>3) for r,g,b in metal.getdata()]
    colors = sorted(set(base))
    assert len(colors) <= 16
    indices = {color:index for index,color in enumerate(colors)}
    base = [indices[color] for color in base]
    emission = []
    for n in range(64):
        d = n/4
        r = 255*math.exp(-d*d/(2*3.9**2)) + 145*math.exp(-d*d/(2*7.0**2))
        g = 153*math.exp(-d*d/(2*2.7**2)) + 10*math.exp(-d*d/(2*4.4**2))
        b = 7*math.exp(-d*d/(2*2.0**2))
        taper = max(0, min(1, (15.75-d)/3))
        emission += [min(255,round(c*taper)) for c in (r,g,b)]
    header = '// Generated by tools/generate_nixie.py; original procedural wire geometry.\n#pragma once\n#include <stdint.h>\n'
    header += '#ifdef ARDUINO\n#include <Arduino.h>\n#else\n#ifndef PROGMEM\n#define PROGMEM\n#endif\n#endif\n\n'
    header += f'static const unsigned kNixieBackgroundColors = {len(colors)};\n\n'

    def array(name,ctype,values,fmt):
        nonlocal header
        header += f'static const {ctype} {name}[{len(values)}] PROGMEM = {{\n'
        header += '\n'.join('  '+','.join(format(v,fmt) for v in values[i:i+24])+','
                            for i in range(0,len(values),24))
        header += '\n};\n\n'
    array('kNixieDistance','uint8_t',sum(fields,[]),'d')
    array('kNixieBackground','uint8_t',base,'d')
    array('kNixieBackgroundPalette','uint16_t',colors,'d')
    array('kNixieEmission','uint8_t',emission,'d')
    path = ROOT/'src/nixie_assets.h'
    path.write_text(header.rstrip()+'\n')
    print('Generated',path,'with',10*W*H+len(base)+len(colors)*2+len(emission),'bytes of flash data')


if __name__ == '__main__':
    main()
